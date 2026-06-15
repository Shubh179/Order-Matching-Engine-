#ifndef LATENCY_TRACKER_H
#define LATENCY_TRACKER_H

#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cmath>
#include <limits>
#include "order.h"

// ============================================================================
// LatencyTracker: Accumulates per-order latency measurements and computes
// running statistics including percentiles and profiling breakdowns.
//
// Design rationale:
//   - Lives as a local variable on the matching thread → single owner,
//     zero synchronization overhead.
//   - Stores raw samples in vectors for exact percentile computation.
//   - Percentiles are computed at report time (STATS command) by sorting,
//     which is O(n log n) but called infrequently (human-driven).
//   - Also tracks cumulative time-in-category for profiling (Part 5).
//
// Memory:
//   Each vector stores one int64_t per order processed.
//   At 1M orders: 3 vectors × 8 bytes × 1M = ~24 MB — acceptable.
//
// Thread safety: NONE (single-threaded by design)
// ============================================================================
class LatencyTracker {
private:
    // Raw latency samples in nanoseconds
    std::vector<int64_t> e2e_samples;      // end-to-end (recv → match complete)
    std::vector<int64_t> queue_samples;    // queue wait (push → pop)
    std::vector<int64_t> match_samples;    // OB+matching (process start → match complete)
    std::vector<int64_t> network_samples;  // network→queue (recv → push)

    // Running aggregates
    int64_t totalCount = 0;
    int64_t minE2E     = std::numeric_limits<int64_t>::max();
    int64_t maxE2E     = 0;
    int64_t sumE2E     = 0;

    // Profiling: cumulative time in each category (nanoseconds)
    int64_t time_network  = 0;  // recv → queue push
    int64_t time_queue    = 0;  // queue push → queue pop
    int64_t time_orderbook = 0; // process start → match complete

    // ========================================================================
    // percentile: Compute the p-th percentile from a sorted vector.
    //   p is in [0.0, 1.0], e.g. 0.95 for P95.
    //   Uses nearest-rank method: index = ceil(p * n) - 1
    // ========================================================================
    static int64_t percentile(std::vector<int64_t>& samples, double p) {
        if (samples.empty()) return 0;
        std::sort(samples.begin(), samples.end());
        size_t idx = static_cast<size_t>(std::ceil(p * samples.size())) - 1;
        if (idx >= samples.size()) idx = samples.size() - 1;
        return samples[idx];
    }

public:
    // ========================================================================
    // record: Record latency segments from a completed Command.
    //   Called once per order after matching completes.
    //   All timestamps are steady_clock nanoseconds.
    //
    //   Segments:
    //     network  = ts_queue_push    - ts_network_recv
    //     queue    = ts_queue_pop     - ts_queue_push
    //     matching = ts_match_complete - ts_process_start
    //     e2e      = ts_match_complete - ts_network_recv
    // ========================================================================
    void record(const Command& cmd) {
        int64_t net_lat   = cmd.ts_queue_push    - cmd.ts_network_recv;
        int64_t q_lat     = cmd.ts_queue_pop     - cmd.ts_queue_push;
        int64_t match_lat = cmd.ts_match_complete - cmd.ts_process_start;
        int64_t e2e_lat   = cmd.ts_match_complete - cmd.ts_network_recv;

        e2e_samples.push_back(e2e_lat);
        queue_samples.push_back(q_lat);
        match_samples.push_back(match_lat);
        network_samples.push_back(net_lat);

        totalCount++;
        sumE2E += e2e_lat;
        if (e2e_lat < minE2E) minE2E = e2e_lat;
        if (e2e_lat > maxE2E) maxE2E = e2e_lat;

        // Profiling accumulators
        time_network  += net_lat;
        time_queue    += q_lat;
        time_orderbook += match_lat;
    }

    // ========================================================================
    // printOrderLatency: Print per-order latency breakdown.
    //   Displays both nanoseconds (raw) and microseconds (human-readable).
    //   Called after each order in interactive/server mode.
    // ========================================================================
    void printOrderLatency(const Command& cmd) const {
        int64_t net_lat   = cmd.ts_queue_push    - cmd.ts_network_recv;
        int64_t q_lat     = cmd.ts_queue_pop     - cmd.ts_queue_push;
        int64_t proc_lat  = cmd.ts_match_complete - cmd.ts_process_start;
        int64_t e2e_lat   = cmd.ts_match_complete - cmd.ts_network_recv;

        std::cout << "\n--- Latency Report: Order #" << cmd.orderId << " ---\n";
        std::cout << std::fixed << std::setprecision(2);

        std::cout << "Network Receive  : " << cmd.ts_network_recv  << " ns\n";
        std::cout << "Queue Push       : " << cmd.ts_queue_push    << " ns\n";
        std::cout << "Queue Pop        : " << cmd.ts_queue_pop     << " ns\n";
        std::cout << "Process Start    : " << cmd.ts_process_start << " ns\n";
        std::cout << "Match Complete   : " << cmd.ts_match_complete << " ns\n";
        std::cout << "\n";
        std::cout << "Network → Queue  : " << net_lat  << " ns ("
                  << (net_lat / 1000.0) << " us)\n";
        std::cout << "Queue Latency    : " << q_lat    << " ns ("
                  << (q_lat / 1000.0) << " us)\n";
        std::cout << "Match Latency    : " << proc_lat << " ns ("
                  << (proc_lat / 1000.0) << " us)\n";
        std::cout << "End-to-End       : " << e2e_lat  << " ns ("
                  << (e2e_lat / 1000.0) << " us)\n";
        std::cout << "---\n\n";
    }

    // ========================================================================
    // printStats: Print comprehensive latency statistics report.
    //   Includes: count, avg, min, max, P50, P95, P99 for end-to-end.
    //   Also includes per-segment P50/P99 and profiling breakdown (Part 5).
    //
    //   Percentile calculation:
    //     Sorts the sample vector (O(n log n)) then indexes directly.
    //     This is exact, not approximate. Called infrequently (STATS cmd).
    // ========================================================================
    void printStats() {
        if (totalCount == 0) {
            std::cout << "\n[STATS] No orders processed yet.\n\n";
            return;
        }

        int64_t avgE2E = sumE2E / totalCount;

        // Compute percentiles (sorts in-place — the vectors are reusable)
        int64_t p50_e2e = percentile(e2e_samples, 0.50);
        int64_t p95_e2e = percentile(e2e_samples, 0.95);
        int64_t p99_e2e = percentile(e2e_samples, 0.99);

        int64_t p50_queue = percentile(queue_samples, 0.50);
        int64_t p99_queue = percentile(queue_samples, 0.99);

        int64_t p50_match = percentile(match_samples, 0.50);
        int64_t p99_match = percentile(match_samples, 0.99);

        int64_t p50_net = percentile(network_samples, 0.50);
        int64_t p99_net = percentile(network_samples, 0.99);

        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout << "║         LATENCY STATISTICS REPORT           ║\n";
        std::cout << "╠══════════════════════════════════════════════╣\n";
        std::cout << "║  Orders Processed: " << std::setw(24) << totalCount << " ║\n";
        std::cout << "╠══════════════════════════════════════════════╣\n";
        std::cout << "║  END-TO-END LATENCY                         ║\n";
        std::cout << "║  Average : " << std::setw(10) << avgE2E
                  << " ns (" << std::setw(8) << std::fixed << std::setprecision(2)
                  << (avgE2E / 1000.0) << " us) ║\n";
        std::cout << "║  Min     : " << std::setw(10) << minE2E
                  << " ns (" << std::setw(8) << (minE2E / 1000.0) << " us) ║\n";
        std::cout << "║  Max     : " << std::setw(10) << maxE2E
                  << " ns (" << std::setw(8) << (maxE2E / 1000.0) << " us) ║\n";
        std::cout << "║  P50     : " << std::setw(10) << p50_e2e
                  << " ns (" << std::setw(8) << (p50_e2e / 1000.0) << " us) ║\n";
        std::cout << "║  P95     : " << std::setw(10) << p95_e2e
                  << " ns (" << std::setw(8) << (p95_e2e / 1000.0) << " us) ║\n";
        std::cout << "║  P99     : " << std::setw(10) << p99_e2e
                  << " ns (" << std::setw(8) << (p99_e2e / 1000.0) << " us) ║\n";
        std::cout << "╠══════════════════════════════════════════════╣\n";
        std::cout << "║  SEGMENT BREAKDOWN (P50 / P99)              ║\n";
        std::cout << "║  Network→Queue : " << std::setw(7) << p50_net
                  << " / " << std::setw(7) << p99_net << " ns   ║\n";
        std::cout << "║  Queue Wait    : " << std::setw(7) << p50_queue
                  << " / " << std::setw(7) << p99_queue << " ns   ║\n";
        std::cout << "║  OB + Matching : " << std::setw(7) << p50_match
                  << " / " << std::setw(7) << p99_match << " ns   ║\n";
        std::cout << "╠══════════════════════════════════════════════╣\n";

        // Profiling breakdown (Part 5)
        int64_t total_time = time_network + time_queue + time_orderbook;
        if (total_time > 0) {
            double pct_net = 100.0 * time_network  / total_time;
            double pct_q   = 100.0 * time_queue    / total_time;
            double pct_ob  = 100.0 * time_orderbook / total_time;

            std::cout << "║  PROFILING BREAKDOWN                        ║\n";
            std::cout << "║  Network     : " << std::setw(6) << std::setprecision(1)
                      << pct_net << " %                       ║\n";
            std::cout << "║  Queue       : " << std::setw(6) << pct_q
                      << " %                       ║\n";
            std::cout << "║  OB+Matching : " << std::setw(6) << pct_ob
                      << " %                       ║\n";
        }

        std::cout << "╚══════════════════════════════════════════════╝\n\n";
    }

    // ========================================================================
    // reset: Clear all accumulated data. Used between benchmark runs.
    // ========================================================================
    void reset() {
        e2e_samples.clear();
        queue_samples.clear();
        match_samples.clear();
        network_samples.clear();
        totalCount = 0;
        minE2E = std::numeric_limits<int64_t>::max();
        maxE2E = 0;
        sumE2E = 0;
        time_network = 0;
        time_queue = 0;
        time_orderbook = 0;
    }

    // ========================================================================
    // Accessors for benchmark use
    // ========================================================================
    int64_t getCount()  const { return totalCount; }
    int64_t getAvgE2E() const { return totalCount > 0 ? sumE2E / totalCount : 0; }
    int64_t getMinE2E() const { return minE2E; }
    int64_t getMaxE2E() const { return maxE2E; }

    int64_t getP50E2E() {
        return percentile(e2e_samples, 0.50);
    }
    int64_t getP95E2E() {
        return percentile(e2e_samples, 0.95);
    }
    int64_t getP99E2E() {
        return percentile(e2e_samples, 0.99);
    }
};

#endif // LATENCY_TRACKER_H
