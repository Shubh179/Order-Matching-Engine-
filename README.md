# Lock-Free High-Performance Order Book Server

A high-performance C++ Order Book matching engine that processes client orders concurrently over a TCP socket using an asynchronous, non-blocking I/O loop (`epoll`), a lock-free SPSC queue, and a dedicated matching engine thread.

Supports exchange-style `NEW`, `CANCEL`, and `MODIFY` commands with **O(1) order lookup** for cancel/modify operations. It includes robust end-to-end latency tracking and throughput benchmarking.

## Architecture

The system is split into a networking thread (Thread 1) and a matching engine thread (Thread 2), separated by a lock-free ring buffer:

```
[ TCP Clients ] 
      │
      ▼  (Multiple concurrent connections)
[ Thread 1: Epoll Edge-Triggered Loop ] 
      │
      ▼  (Parse string to Command struct)
[ Lock-Free SPSC Ring Buffer (spsc_queue.h) ] (Acquire-Release ordering)
      │
      ▼  (Pop command)
[ Thread 2: Matching Engine Thread ]
      │
      ├─► NEW    → addOrder()    → matchOrders()
      ├─► CANCEL → cancelOrder() (O(1) lookup + removal)
      ├─► MODIFY → modifyOrder() → matchOrders()
      └─► printBook() (Display top 5 levels)
```

### Components

1. **Order & Command Definitions (`order.h`)**:
   - `Order` struct with `uint64_t orderId` for globally unique IDs.
   - `Command` struct: tagged union (~88 bytes) transported through the SPSC queue, fully instrumented with `steady_clock` timestamps for latency measurement.

2. **Order Book (`orderbook.h`)**:
   - Each price level is a `std::list<Order>` (FIFO queue with O(1) mid-erase via stable iterators).
   - **`std::unordered_map<uint64_t, OrderLocation>`**: O(1) lookup index by order ID.

3. **Lock-Free Queue (`spsc_queue.h`)**: A Single-Producer Single-Consumer lock-free ring buffer.

4. **Epoll Server (`server.cpp`)**: 
   - Uses an asynchronous, non-blocking TCP socket event loop with `epoll` in edge-triggered (`EPOLLET`) mode.

5. **Latency Tracking (`latency_tracker.h`)**:
   - Accumulates timestamp deltas across the pipeline. Single-threaded by design (lives on the matching thread). Computes exact latency percentiles (P50, P95, P99).

---

## Complexity Analysis

| Operation | Complexity | Details |
|-----------|------------|---------|
| Add order (NEW) | O(log P) | `std::map` insert + O(1) list push_back + O(1) unordered_map insert |
| Cancel order | **O(1)** | unordered_map lookup + list erase (stable iterator) |
| Modify (qty down) | **O(1)** | unordered_map lookup + in-place write |
| Modify (price/qty up) | O(log P) | O(1) cancel + O(log P) reinsert |
| Match (per fill) | O(1) | list front/pop + unordered_map erase |
| Lookup order | **O(1)** | unordered_map find |

Where **P** = number of distinct price levels.

---

## Building the Project

### Prerequisites
* Linux environment (due to standard `epoll` API).
* GCC compiler supporting C++17.
* Make build utility.

### Compilation
To compile all targets, run:
```bash
make all
```

This will produce three executables:
* `server`: The main epoll-based TCP server listening on port `8080`.
* `test_runner`: A standalone CLI runner to test matching behavior locally.
* `benchmark`: A standalone binary for testing throughput and latency.

---

## Performance Measurement

### Latency Measurement Methodology

Every `Command` struct is instrumented with five `std::chrono::steady_clock` timestamps (nanosecond precision) embedded directly in the struct so they cross thread boundaries without any shared mutable state or mutexes:

1. `ts_network_recv`: Captured immediately after `recv()` returns data.
2. `ts_queue_push`: Captured right before pushing into the lock-free SPSC queue.
3. `ts_queue_pop`: Captured right after popping from the queue.
4. `ts_process_start`: Captured before dispatching to the Order Book.
5. `ts_match_complete`: Captured after all crossing trades are resolved.

**Latency Segments:**
- **Network→Queue:** `ts_queue_push - ts_network_recv`
- **Queue Wait:** `ts_queue_pop - ts_queue_push`
- **Matching Processing:** `ts_match_complete - ts_process_start`
- **End-to-End:** `ts_match_complete - ts_network_recv`

### `STATS` Command

You can view a real-time latency report by sending `STATS` over TCP to the running server. The report is processed on the matching thread to guarantee thread safety.

```text
╔══════════════════════════════════════════════╗
║         LATENCY STATISTICS REPORT           ║
╠══════════════════════════════════════════════╣
║  Orders Processed:                   100000 ║
╠══════════════════════════════════════════════╣
║  END-TO-END LATENCY                         ║
║  Average :    3756011 ns ( 3756.01 us) ║
║  P50     :    3358679 ns ( 3358.68 us) ║
║  P99     :   10999755 ns (10999.75 us) ║
...
```

### Throughput & Queue Benchmark

Run the standalone benchmark to measure peak throughput:
```bash
./benchmark
```

The benchmark:
1. Pushes 1K, 10K, 100K, and 1M synthetic orders (bypassing TCP to isolate engine throughput).
2. Compares the **SPSC Lock-Free Queue** against a standard **Mutex Queue** (`std::queue` + `std::mutex`). 
   - *Note on Benchmark Results*: In synthetic tight-loop tests, SPSC might show *lower* raw throughput than Mutex because the SPSC queue enforces capacity backpressure (spin-yields when full), measuring true concurrent throughput. The unbounded Mutex queue allows the producer to dump all orders instantly, resulting in zero contention but absolutely massive queue wait latencies (often 10-20x worse than SPSC). SPSC ensures tight latency bounds.
3. Prints a **Profiling Breakdown** detailing time spent in networking, queuing, and matching.

---

## Command Format

Connect to the server with `nc localhost 8080` and send commands:

| Command | Format | Description |
|---------|--------|-------------|
| New Buy | `BUY <qty> <price>` | Place a new buy order |
| New Sell | `SELL <qty> <price>` | Place a new sell order |
| Cancel | `CANCEL <orderId>` | Cancel an existing order by ID |
| Modify | `MODIFY <orderId> <qty> <price>` | Modify an order's qty and/or price |
| Stats | `STATS` | Print latency percentiles and profiling report |

---

## Modify Semantics (CME-Style)

The modify command follows the policy used by major exchanges (CME, Eurex):

| Scenario | Behavior | Time Priority |
|----------|----------|---------------|
| Same price, qty decrease | In-place update | **Preserved** |
| Same price, qty increase | Cancel + reinsert | **Lost** |
| Price change | Cancel + reinsert | **Lost** |

---

## Data Structure Choices

### Why `std::list` instead of `std::deque`?
`std::deque` does not support O(1) erase-by-iterator in the middle (elements must shift, O(n)). `std::list` provides O(1) erase anywhere via stable iterators. The trade-off is slightly worse cache locality during iteration, but this is required for the O(1) cancel/modify guarantee.

### Why `std::unordered_map` for order lookup?
Maps `orderId → OrderLocation` (side + price + list iterator). Enables instant O(1) access for cancellations and modifications without scanning price levels.
