#ifndef ORDER_H
#define ORDER_H

#include <cstdint>

// ============================================================================
// Side: BUY or SELL
// ============================================================================
enum Side {
    BUY,
    SELL
};

// ============================================================================
// Order: Represents a single resting order in the book.
//        orderId is uint64_t for globally unique IDs.
// ============================================================================
struct Order {
    uint64_t  orderId;
    Side      side;
    double    price;
    int       quantity;
    long long timestamp;
};

// ============================================================================
// CommandType: Discriminator for the Command tagged union.
//   NEW    — place a new order
//   CANCEL — cancel an existing order by ID
//   MODIFY — modify an existing order (qty and/or price)
//   STATS  — print latency statistics report (no order data needed)
// ============================================================================
enum CommandType {
    NEW,
    CANCEL,
    MODIFY,
    STATS,
    BOOK       // Display order book for a specific symbol
};

// ============================================================================
// Command: The unit of work transported through the SPSC queue.
//
// Design rationale:
//   The SPSC ring buffer is a fixed-size array of T — it cannot hold
//   polymorphic types. A tagged union (CommandType + flat fields) keeps
//   the struct compact while supporting all command types. Unused fields
//   for CANCEL/STATS are simply ignored — zero runtime overhead.
//
// Latency timestamps (Parts 1 & 5):
//   Five int64_t fields using std::chrono::steady_clock nanoseconds.
//   Embedded directly in the struct so they cross the SPSC queue
//   boundary without any shared mutable state or mutex.
//
//   Why steady_clock?
//     - Monotonic — never adjusts for NTP or DST
//     - Guarantees non-negative deltas between successive reads
//     - system_clock can jump backwards, producing negative "latencies"
//     - steady_clock is the standard for HFT latency measurement
//
//   Why nanoseconds?
//     - Microsecond resolution is too coarse for intra-process latencies
//       (queue push/pop can be <100ns)
//     - steady_clock::now() is typically a single rdtsc/clock_gettime call
//       with ~20ns overhead
//
// Field usage by command type:
//   NEW:    type, orderId, side, price, quantity, timestamp, ts_*
//   CANCEL: type, orderId, timestamp, ts_*
//   MODIFY: type, orderId, price, quantity, timestamp, ts_*
//   STATS:  type only (all other fields ignored)
//
// Size: ~88 bytes (fits in two cache lines)
// ============================================================================
struct Command {
    CommandType type;
    uint64_t    orderId;
    Side        side;       // Only meaningful for NEW
    double      price;      // NEW and MODIFY
    int         quantity;   // NEW and MODIFY
    long long   timestamp;  // Set by network thread for all types
    char        symbol[16]; // Symbol identifier (e.g., "AAPL", "TSLA")

    // ========================================================================
    // Latency measurement timestamps (steady_clock nanoseconds)
    //
    // Pipeline stages:
    //   recv() → ts_network_recv
    //   parse  → (no timestamp, negligible)
    //   push() → ts_queue_push     (set just before SPSC push)
    //   pop()  → ts_queue_pop      (set just after SPSC pop)
    //   OB op  → ts_process_start  (set before addOrder/cancel/modify)
    //   match  → ts_match_complete (set after matchOrders returns)
    //
    // Latency segments:
    //   Network→Queue  = ts_queue_push    - ts_network_recv
    //   Queue wait     = ts_queue_pop     - ts_queue_push
    //   OB processing  = ts_match_complete - ts_process_start
    //   End-to-end     = ts_match_complete - ts_network_recv
    // ========================================================================
    int64_t ts_network_recv;    // Immediately after recv() returns data
    int64_t ts_queue_push;      // Just before spscQueue.push()
    int64_t ts_queue_pop;       // Just after spscQueue.pop() succeeds
    int64_t ts_process_start;   // Before orderBook dispatch
    int64_t ts_match_complete;  // After matchOrders() returns
};

#endif // ORDER_H
