#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "order.h"
#include "orderbook.h"
#include "exchange.h"
#include "spsc_queue.h"
#include "mutex_queue.h"
#include "latency_tracker.h"

// ============================================================================
// now_ns: Read steady_clock in nanoseconds (same as server.cpp)
// ============================================================================
inline int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

// ============================================================================
// Symbols used for multi-book benchmarking
// ============================================================================
static const char* BENCH_SYMBOLS[] = {"AAPL", "TSLA", "INFY", "GOOG", "MSFT"};
static const int NUM_SYMBOLS = 5;

// ============================================================================
// makeCommand: Generate a deterministic synthetic order with symbol rotation.
// ============================================================================
Command makeCommand(uint64_t id) {
    Command cmd{};
    cmd.type    = NEW;
    cmd.orderId = id;

    // Rotate symbols deterministically
    const char* sym = BENCH_SYMBOLS[id % NUM_SYMBOLS];
    std::strncpy(cmd.symbol, sym, sizeof(cmd.symbol) - 1);
    cmd.symbol[sizeof(cmd.symbol) - 1] = '\0';

    if (id % 100 == 0 && id > 0) {
        if (id % 200 == 0) {
            cmd.side  = BUY;
            cmd.price = 109.5;
        } else {
            cmd.side  = SELL;
            cmd.price = 100.0;
        }
    } else if (id % 2 == 0) {
        cmd.side  = BUY;
        cmd.price = 100.0 + (id % 10) * 0.5;
    } else {
        cmd.side  = SELL;
        cmd.price = 105.0 + (id % 10) * 0.5;
    }

    cmd.quantity  = 1 + static_cast<int>(id % 100);
    cmd.timestamp = 0;
    return cmd;
}

// ============================================================================
// PART 3: Throughput Benchmark
// ============================================================================
template <typename QueueType>
void runBenchmark(const std::string& queueName, int numOrders,
                  LatencyTracker& tracker) {
    std::vector<Command> commands(numOrders);
    for (int i = 0; i < numOrders; i++) {
        commands[i] = makeCommand(static_cast<uint64_t>(i + 1));
    }

    QueueType queue;
    Exchange exchange;
    std::atomic<bool> done{false};
    tracker.reset();

    std::streambuf* origBuf = std::cout.rdbuf();

    std::thread consumer([&]() {
        int processed = 0;
        while (processed < numOrders) {
            Command cmd;
            if (queue.pop(cmd)) {
                cmd.ts_queue_pop = now_ns();
                cmd.ts_process_start = now_ns();

                Order o{cmd.orderId, cmd.side, cmd.price, cmd.quantity, cmd.timestamp};
                std::string symbol(cmd.symbol);

                std::cout.rdbuf(nullptr);
                exchange.addOrder(symbol, o);
                exchange.matchOrders(symbol);
                std::cout.rdbuf(origBuf);

                cmd.ts_match_complete = now_ns();
                tracker.record(cmd);
                processed++;
            } else {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    auto wallStart = std::chrono::steady_clock::now();

    for (int i = 0; i < numOrders; i++) {
        commands[i].ts_network_recv = now_ns();
        commands[i].ts_queue_push = now_ns();
        while (!queue.push(commands[i])) {
            std::this_thread::yield();
        }
    }

    consumer.join();

    auto wallEnd = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(wallEnd - wallStart).count();
    double throughput = numOrders / elapsedSec;

    std::cout << "\n";
    std::cout << "┌──────────────────────────────────────────────┐\n";
    std::cout << "│  " << queueName << " — " << numOrders << " orders"
              << std::string(std::max(0, 34 - static_cast<int>(queueName.size())
                  - static_cast<int>(std::to_string(numOrders).size())), ' ')
              << "│\n";
    std::cout << "├──────────────────────────────────────────────┤\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "│  Elapsed Time  : " << std::setw(12) << elapsedSec
              << " sec            │\n";
    std::cout << std::setprecision(0);
    std::cout << "│  Throughput    : " << std::setw(12) << throughput
              << " orders/sec     │\n";
    std::cout << std::setprecision(2);
    std::cout << "│  Avg Latency   : " << std::setw(12) << (tracker.getAvgE2E() / 1000.0)
              << " us              │\n";
    std::cout << "│  P50 Latency   : " << std::setw(12) << (tracker.getP50E2E() / 1000.0)
              << " us              │\n";
    std::cout << "│  P95 Latency   : " << std::setw(12) << (tracker.getP95E2E() / 1000.0)
              << " us              │\n";
    std::cout << "│  P99 Latency   : " << std::setw(12) << (tracker.getP99E2E() / 1000.0)
              << " us              │\n";
    std::cout << "│  Min Latency   : " << std::setw(12) << (tracker.getMinE2E() / 1000.0)
              << " us              │\n";
    std::cout << "│  Max Latency   : " << std::setw(12) << (tracker.getMaxE2E() / 1000.0)
              << " us              │\n";
    std::cout << "└──────────────────────────────────────────────┘\n";
}

// ============================================================================
// PART 4: Queue Performance Comparison
// ============================================================================
struct BenchResult {
    double throughput;
    double avgLatUs;
    double p50LatUs;
    double p95LatUs;
    double p99LatUs;
};

// ============================================================================
// Main: Run all benchmarks
// ============================================================================
int main() {
    std::setvbuf(stdout, NULL, _IONBF, 0);

    // ========================================================================
    // PART 3: Throughput Benchmark at multiple scales
    // ========================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║     PART 3: THROUGHPUT BENCHMARK             ║\n";
    std::cout << "║     (SPSC Lock-Free Queue)                   ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    int scales[] = {1000, 10000, 100000, 1000000};
    LatencyTracker tracker;

    for (int n : scales) {
        runBenchmark<SPSCQueue<Command, 1024>>("SPSC Lock-Free", n, tracker);
    }

    // ========================================================================
    // PART 4: Queue Performance Comparison
    // ========================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║     PART 4: QUEUE PERFORMANCE COMPARISON     ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    {
        int compOrders = 100000;

        std::vector<Command> commands(compOrders);
        for (int i = 0; i < compOrders; i++) {
            commands[i] = makeCommand(static_cast<uint64_t>(i + 1));
        }

        LatencyTracker spscTracker, mutexTracker;
        double spscThroughput, mutexThroughput;
        std::streambuf* origBuf = std::cout.rdbuf();

        // SPSC benchmark
        {
            SPSCQueue<Command, 1024> queue;
            Exchange exchange;
            spscTracker.reset();

            std::thread consumer([&]() {
                int processed = 0;
                while (processed < compOrders) {
                    Command cmd;
                    if (queue.pop(cmd)) {
                        cmd.ts_queue_pop = now_ns();
                        cmd.ts_process_start = now_ns();
                        Order o{cmd.orderId, cmd.side, cmd.price, cmd.quantity, 0};
                        std::string symbol(cmd.symbol);
                        
                        std::cout.rdbuf(nullptr);
                        exchange.addOrder(symbol, o);
                        exchange.matchOrders(symbol);
                        std::cout.rdbuf(origBuf);
                        
                        cmd.ts_match_complete = now_ns();
                        spscTracker.record(cmd);
                        processed++;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });

            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < compOrders; i++) {
                commands[i].ts_network_recv = now_ns();
                commands[i].ts_queue_push = now_ns();
                while (!queue.push(commands[i])) {
                    std::this_thread::yield();
                }
            }
            consumer.join();
            auto end = std::chrono::steady_clock::now();
            spscThroughput = compOrders / std::chrono::duration<double>(end - start).count();
        }

        // Mutex benchmark
        {
            MutexQueue<Command> queue;
            Exchange exchange;
            mutexTracker.reset();

            for (int i = 0; i < compOrders; i++) {
                commands[i].ts_network_recv = 0;
                commands[i].ts_queue_push = 0;
            }

            std::thread consumer([&]() {
                int processed = 0;
                while (processed < compOrders) {
                    Command cmd;
                    if (queue.pop(cmd)) {
                        cmd.ts_queue_pop = now_ns();
                        cmd.ts_process_start = now_ns();
                        Order o{cmd.orderId, cmd.side, cmd.price, cmd.quantity, 0};
                        std::string symbol(cmd.symbol);
                        
                        std::cout.rdbuf(nullptr);
                        exchange.addOrder(symbol, o);
                        exchange.matchOrders(symbol);
                        std::cout.rdbuf(origBuf);
                        
                        cmd.ts_match_complete = now_ns();
                        mutexTracker.record(cmd);
                        processed++;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });

            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < compOrders; i++) {
                commands[i].ts_network_recv = now_ns();
                commands[i].ts_queue_push = now_ns();
                while (!queue.push(commands[i])) {
                    std::this_thread::yield();
                }
            }
            consumer.join();
            auto end = std::chrono::steady_clock::now();
            mutexThroughput = compOrders / std::chrono::duration<double>(end - start).count();
        }

        // Print comparison
        double tImprove = ((spscThroughput - mutexThroughput) / mutexThroughput) * 100.0;
        double lImprove = 0;
        if (mutexTracker.getAvgE2E() > 0) {
            lImprove = ((double)(mutexTracker.getAvgE2E() - spscTracker.getAvgE2E())
                       / mutexTracker.getAvgE2E()) * 100.0;
        }

        std::cout << "\n  Orders: " << compOrders << "\n";
        std::cout << "\n  ┌──────────────────┬──────────────┬──────────────┐\n";
        std::cout << "  │  Metric          │  SPSC Queue  │  Mutex Queue │\n";
        std::cout << "  ├──────────────────┼──────────────┼──────────────┤\n";
        std::cout << std::fixed;
        std::cout << "  │  Throughput      │ " << std::setw(10) << std::setprecision(0)
                  << spscThroughput << "   │ " << std::setw(10)
                  << mutexThroughput << "   │\n";
        std::cout << "  │  (orders/sec)    │              │              │\n";
        std::cout << "  ├──────────────────┼──────────────┼──────────────┤\n";
        std::cout << "  │  Avg Latency(us) │ " << std::setw(10) << std::setprecision(2)
                  << (spscTracker.getAvgE2E() / 1000.0) << "   │ " << std::setw(10)
                  << (mutexTracker.getAvgE2E() / 1000.0) << "   │\n";
        std::cout << "  │  P50 Latency(us) │ " << std::setw(10)
                  << (spscTracker.getP50E2E() / 1000.0) << "   │ " << std::setw(10)
                  << (mutexTracker.getP50E2E() / 1000.0) << "   │\n";
        std::cout << "  │  P99 Latency(us) │ " << std::setw(10)
                  << (spscTracker.getP99E2E() / 1000.0) << "   │ " << std::setw(10)
                  << (mutexTracker.getP99E2E() / 1000.0) << "   │\n";
        std::cout << "  └──────────────────┴──────────────┴──────────────┘\n";
        std::cout << "\n  SPSC Improvement:\n";
        std::cout << "    Throughput : +" << std::setprecision(1) << tImprove << " %\n";
        std::cout << "    Latency   : -" << lImprove << " % (lower is better)\n";
    }

    // ========================================================================
    // PART 5: Profiling Breakdown
    // ========================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║     PART 5: PROFILING BREAKDOWN              ║\n";
    std::cout << "║     (100,000 orders through SPSC queue)      ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    {
        int profOrders = 100000;
        SPSCQueue<Command, 1024> queue;
        Exchange exchange;
        LatencyTracker profTracker;
        profTracker.reset();
        std::streambuf* origBuf = std::cout.rdbuf();

        std::vector<Command> commands(profOrders);
        for (int i = 0; i < profOrders; i++) {
            commands[i] = makeCommand(static_cast<uint64_t>(i + 1));
        }

        std::thread consumer([&]() {
            int processed = 0;
            while (processed < profOrders) {
                Command cmd;
                if (queue.pop(cmd)) {
                    cmd.ts_queue_pop = now_ns();
                    cmd.ts_process_start = now_ns();
                    Order o{cmd.orderId, cmd.side, cmd.price, cmd.quantity, 0};
                    std::string symbol(cmd.symbol);
                    
                    std::cout.rdbuf(nullptr);
                    exchange.addOrder(symbol, o);
                    exchange.matchOrders(symbol);
                    std::cout.rdbuf(origBuf);
                    
                    cmd.ts_match_complete = now_ns();
                    profTracker.record(cmd);
                    processed++;
                } else {
                    std::this_thread::yield();
                }
            }
        });

        for (int i = 0; i < profOrders; i++) {
            commands[i].ts_network_recv = now_ns();
            commands[i].ts_queue_push = now_ns();
            while (!queue.push(commands[i])) {
                std::this_thread::yield();
            }
        }
        consumer.join();

        profTracker.printStats();
    }

    std::cout << "\nBenchmark complete.\n";
    return 0;
}
