#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <csignal>
#include <netinet/tcp.h>
#include "orderbook.h"
#include "order.h"
#include "spsc_queue.h"
#include "latency_tracker.h"

#define MAX_EVENTS 64

// ============================================================================
// Shared state between threads
//
// The SPSC queue transports Command structs (tagged union) which now include
// 5 steady_clock nanosecond timestamps for latency measurement.
//
// Thread ownership:
//   - Network thread (Thread 1): sole producer of the SPSC queue
//   - Matching thread (Thread 2): sole consumer + sole owner of OrderBook
//     + sole owner of LatencyTracker
//   - No mutexes anywhere around the OrderBook or LatencyTracker
// ============================================================================
SPSCQueue<Command, 1024> spscQueue;
OrderBook orderBook;
std::atomic<bool> running{true};

// ============================================================================
// Signal Handler for Graceful Shutdown
// ============================================================================
void signalHandler(int signum) {
    std::cout << "\n[Signal] Received signal " << signum << " (SIGINT/SIGTERM).\n";
    std::cout << "[Signal] Initiating graceful shutdown...\n";
    running.store(false, std::memory_order_relaxed);
}

// ============================================================================
// now_ns: Read steady_clock in nanoseconds.
//
// Why steady_clock?
//   - Monotonic: never adjusted by NTP or DST → guarantees non-negative deltas
//   - Typically backed by rdtsc or clock_gettime(CLOCK_MONOTONIC) → ~20ns cost
//   - system_clock can jump backwards → negative "latencies"
//
// Why nanoseconds?
//   - Microsecond resolution is too coarse for sub-microsecond operations
//     like SPSC push/pop (~50-200ns)
// ============================================================================
inline int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

long long getCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Helper function to set file descriptor to non-blocking
bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ============================================================================
// Thread 2: Matching Engine
//
// Pops Command structs from the SPSC queue and dispatches them.
// Instruments each command with timestamps at:
//   - ts_queue_pop:      immediately after pop succeeds
//   - ts_process_start:  before orderBook dispatch
//   - ts_match_complete: after matchOrders() returns
//
// The LatencyTracker lives as a local variable — single owner, no sync.
// ============================================================================
void matchingEngineThread() {
    std::cout << "[Thread 2] Matching engine thread started.\n";

    // LatencyTracker lives entirely on this thread — no synchronization
    LatencyTracker tracker;

    while (running.load(std::memory_order_relaxed)) {
        Command cmd;
        if (spscQueue.pop(cmd)) {
            // Timestamp 3: Queue Pop
            cmd.ts_queue_pop = now_ns();

            if (cmd.type == STATS) {
                // STATS command: print the latency report
                tracker.printStats();
                continue;
            }

            // Timestamp 4: Process Start
            cmd.ts_process_start = now_ns();

            switch (cmd.type) {
                case NEW: {
                    Order o;
                    o.orderId   = cmd.orderId;
                    o.side      = cmd.side;
                    o.price     = cmd.price;
                    o.quantity  = cmd.quantity;
                    o.timestamp = cmd.timestamp;

                    orderBook.addOrder(o);
                    orderBook.matchOrders();
                    break;
                }
                case CANCEL: {
                    orderBook.cancelOrder(cmd.orderId);
                    break;
                }
                case MODIFY: {
                    orderBook.modifyOrder(cmd.orderId, cmd.quantity,
                                         cmd.price, cmd.timestamp);
                    orderBook.matchOrders();
                    break;
                }
                default:
                    break;
            }

            // Timestamp 5: Match Complete
            cmd.ts_match_complete = now_ns();

            // Record latency and print per-order breakdown
            tracker.record(cmd);
            tracker.printOrderLatency(cmd);

            orderBook.printBook();
        } else {
            std::this_thread::yield();
        }
    }
    std::cout << "[Thread 2] Matching engine thread stopped.\n";
}

// ============================================================================
// Thread 1: Network Parser
//
// Parses one line of client input into a Command struct and pushes it
// into the SPSC queue. Instruments each command with timestamps at:
//   - ts_network_recv:  passed in from the recv() call site
//   - ts_queue_push:    just before SPSC push
//
// Supported formats:
//   BUY <qty> <price>
//   SELL <qty> <price>
//   CANCEL <orderId>
//   MODIFY <orderId> <qty> <price>
//   STATS
// ============================================================================
void processLine(const std::string& line, uint64_t& nextOrderId,
                 int64_t recvTimestamp) {
    if (line.empty()) return;

    std::stringstream ss(line);
    std::string cmdStr;
    ss >> cmdStr;

    if (cmdStr == "BUY" || cmdStr == "SELL") {
        int quantity;
        double price;

        if (!(ss >> quantity >> price)) {
            std::cout << "[Thread 1] Malformed order: " << line << "\n";
            return;
        }

        Command cmd;
        cmd.type      = NEW;
        cmd.orderId   = nextOrderId++;
        cmd.side      = (cmdStr == "BUY") ? BUY : SELL;
        cmd.price     = price;
        cmd.quantity  = quantity;
        cmd.timestamp = getCurrentTimestamp();

        // Timestamp 1: Network Receive (passed from recv site)
        cmd.ts_network_recv = recvTimestamp;

        std::cout << "[Thread 1] Pushing NEW Order ID=" << cmd.orderId
                  << " to queue...\n";

        // Timestamp 2: Queue Push
        cmd.ts_queue_push = now_ns();
        while (!spscQueue.push(cmd)) {
            std::this_thread::yield();
        }

    } else if (cmdStr == "CANCEL") {
        uint64_t orderId;

        if (!(ss >> orderId)) {
            std::cout << "[Thread 1] Malformed CANCEL: " << line << "\n";
            return;
        }

        Command cmd;
        cmd.type      = CANCEL;
        cmd.orderId   = orderId;
        cmd.timestamp = getCurrentTimestamp();
        cmd.ts_network_recv = recvTimestamp;

        std::cout << "[Thread 1] Pushing CANCEL Order ID=" << cmd.orderId
                  << " to queue...\n";

        cmd.ts_queue_push = now_ns();
        while (!spscQueue.push(cmd)) {
            std::this_thread::yield();
        }

    } else if (cmdStr == "MODIFY") {
        uint64_t orderId;
        int quantity;
        double price;

        if (!(ss >> orderId >> quantity >> price)) {
            std::cout << "[Thread 1] Malformed MODIFY: " << line << "\n";
            return;
        }

        Command cmd;
        cmd.type      = MODIFY;
        cmd.orderId   = orderId;
        cmd.quantity  = quantity;
        cmd.price     = price;
        cmd.timestamp = getCurrentTimestamp();
        cmd.ts_network_recv = recvTimestamp;

        std::cout << "[Thread 1] Pushing MODIFY Order ID=" << cmd.orderId
                  << " to queue...\n";

        cmd.ts_queue_push = now_ns();
        while (!spscQueue.push(cmd)) {
            std::this_thread::yield();
        }

    } else if (cmdStr == "STATS") {
        // STATS command: push through queue so matching thread prints report
        Command cmd;
        cmd.type = STATS;
        cmd.ts_network_recv = recvTimestamp;
        cmd.ts_queue_push = now_ns();

        std::cout << "[Thread 1] Pushing STATS to queue...\n";
        while (!spscQueue.push(cmd)) {
            std::this_thread::yield();
        }

    } else {
        std::cout << "[Thread 1] Unknown command: " << line << "\n";
    }
}

int main() {
    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::setvbuf(stdout, NULL, _IONBF, 0);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Failed to create socket: " << strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    // SO_REUSEPORT improves production readiness by allowing multiple processes/threads
    // to bind to the same port, and prevents "address already in use" errors during rolling restarts.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt SO_REUSEPORT failed: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    if (!setNonBlocking(server_fd)) {
        std::cerr << "Failed to set listening socket to non-blocking: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed on port 8080: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 30) < 0) {
        std::cerr << "Listen failed: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    std::cout << "Server listening on port 8080 with epoll (edge-triggered)...\n";

    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    // Add listening socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        std::cerr << "epoll_ctl add listener failed: " << strerror(errno) << "\n";
        close(epoll_fd);
        close(server_fd);
        return 1;
    }

    // Start matching engine thread (Thread 2)
    std::thread matchThread(matchingEngineThread);

    uint64_t nextOrderId = 1;
    struct epoll_event events[MAX_EVENTS];
    std::unordered_map<int, std::string> clientBuffers;

    while (running.load(std::memory_order_relaxed)) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue; // Interrupted by signal (e.g. SIGINT), safe to continue loop to exit
            std::cerr << "epoll_wait error: " << strerror(errno) << "\n";
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                // Accept new connections in a loop for edge-triggered
                while (true) {
                    struct sockaddr_in client_address;
                    socklen_t addrlen = sizeof(client_address);
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_address, &addrlen);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "Accept connection failed: " << strerror(errno) << "\n";
                        break;
                    }

                    if (!setNonBlocking(client_fd)) {
                        std::cerr << "Failed to set client socket to non-blocking: " << strerror(errno) << "\n";
                        close(client_fd);
                        continue;
                    }

                    // Disable Nagle's algorithm. Extremely critical for low latency so packets aren't delayed by ~40ms
                    int tcp_opt = 1;
                    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &tcp_opt, sizeof(tcp_opt)) < 0) {
                        std::cerr << "Failed to set TCP_NODELAY: " << strerror(errno) << "\n";
                    }

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET;
                    client_ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                        std::cerr << "epoll_ctl add client failed: " << strerror(errno) << "\n";
                        close(client_fd);
                    } else {
                        std::cout << "[Thread 1] New client connected on fd " << client_fd << "\n";
                        clientBuffers[client_fd] = "";
                    }
                }
            } else {
                // Existing client socket has data
                int client_fd = events[i].data.fd;
                char buffer[512];
                bool closed = false;

                // ============================================================
                // Timestamp 1: Network Receive
                // Captured immediately after recv() returns data.
                // This timestamp is passed through processLine() into the
                // Command struct, crossing the SPSC queue to the matching
                // thread where it's used to compute end-to-end latency.
                // ============================================================
                int64_t recvTimestamp = 0;

                while (true) {
                    ssize_t bytesRead = read(client_fd, buffer, sizeof(buffer));
                    if (bytesRead < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "[Thread 1] Read error on fd " << client_fd << ": " << strerror(errno) << "\n";
                        closed = true;
                        break;
                    } else if (bytesRead == 0) {
                        closed = true;
                        break;
                    }

                    // Capture recv timestamp on first successful read
                    if (recvTimestamp == 0) {
                        recvTimestamp = now_ns();
                    }

                    clientBuffers[client_fd].append(buffer, bytesRead);
                }

                // Process complete lines from the client buffer
                if (clientBuffers.count(client_fd)) {
                    std::string& dataBuffer = clientBuffers[client_fd];
                    size_t pos;
                    while ((pos = dataBuffer.find('\n')) != std::string::npos) {
                        std::string line = dataBuffer.substr(0, pos);
                        dataBuffer.erase(0, pos + 1);

                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }

                        processLine(line, nextOrderId, recvTimestamp);
                    }
                }

                if (closed) {
                    std::cout << "[Thread 1] Client disconnected on fd " << client_fd << "\n";
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    clientBuffers.erase(client_fd);
                }
            }
        }
    }

    std::cout << "[Main] Exiting event loop. Initiating shutdown sequence.\n";

    // Push a final STATS command to ensure remaining items are processed and stats are printed
    Command shutdown_cmd;
    shutdown_cmd.type = STATS;
    shutdown_cmd.ts_network_recv = now_ns();
    shutdown_cmd.ts_queue_push = now_ns();
    
    // We try to push the stats command, but don't block forever if queue is broken
    spscQueue.push(shutdown_cmd);

    // Give matching thread time to process remaining SPSC items
    running.store(false, std::memory_order_relaxed);
    if (matchThread.joinable()) {
        std::cout << "[Main] Waiting for Matching Engine thread to finish...\n";
        matchThread.join();
    }

    std::cout << "[Main] Closing all active client sockets...\n";
    for (auto const& [fd, buffer] : clientBuffers) {
        close(fd);
    }
    clientBuffers.clear();

    std::cout << "[Main] Closing server and epoll FDs...\n";
    close(server_fd);
    close(epoll_fd);
    
    std::cout << "[Main] Shutdown complete.\n";
    return 0;
}
