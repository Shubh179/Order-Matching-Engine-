#include <iostream>
#include "order.h"
#include "orderbook.h"
#include "exchange.h"

// ============================================================================
// Standalone test runner for the Multi-Symbol Exchange.
//
// Tests exercised:
//   1.  NEW orders on AAPL (resting, no crossing)
//   2.  NEW orders on TSLA (resting, no crossing — isolation test)
//   3.  Cross-symbol non-matching (AAPL BUY should NOT match TSLA SELL)
//   4.  Intra-symbol matching (AAPL BUY crosses AAPL SELL)
//   5.  CANCEL an existing order (TSLA)
//   6.  CANCEL a non-existent order
//   7.  MODIFY with quantity decrease (priority preserved)
//   8.  MODIFY with price change (cancel+reinsert)
//   9.  MODIFY with wrong symbol (rejected)
//   10. MODIFY a non-existent order
//   11. BOOK display for each symbol
//   12. STATS display (per-symbol breakdown)
//   13. Third symbol (INFY) independence
// ============================================================================

int main() {
    Exchange exchange;

    // ========================================================================
    // Test 1: Add resting orders on AAPL (no crossing)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 1: Adding AAPL resting orders\n";
    std::cout << "========================================\n";

    exchange.addOrder("AAPL", {1, BUY,  150.0, 10, 1000LL});
    exchange.addOrder("AAPL", {2, BUY,  149.5, 20, 1001LL});
    exchange.addOrder("AAPL", {3, SELL, 152.0, 15, 1002LL});
    exchange.addOrder("AAPL", {4, SELL, 153.0, 10, 1003LL});

    exchange.printBook("AAPL");
    std::cout << "\n";

    // ========================================================================
    // Test 2: Add resting orders on TSLA (isolation)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 2: Adding TSLA resting orders\n";
    std::cout << "========================================\n";

    exchange.addOrder("TSLA", {5, BUY,  250.0, 30, 1004LL});
    exchange.addOrder("TSLA", {6, SELL, 250.0, 30, 1005LL});

    // These should match within TSLA
    exchange.matchOrders("TSLA");
    exchange.printBook("TSLA");
    std::cout << "\n";

    // ========================================================================
    // Test 3: Cross-symbol non-matching
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 3: Cross-symbol isolation\n";
    std::cout << "========================================\n";

    // Add TSLA SELL at a price that would match AAPL BUY if they were same symbol
    exchange.addOrder("TSLA", {7, SELL, 149.0, 10, 1006LL});
    exchange.matchOrders("TSLA");
    exchange.matchOrders("AAPL");

    std::cout << "AAPL BUY 10@150 should NOT match TSLA SELL 10@149:\n";
    exchange.printBook("AAPL");
    exchange.printBook("TSLA");
    std::cout << "\n";

    // ========================================================================
    // Test 4: Intra-symbol matching (AAPL BUY crosses AAPL SELL)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 4: AAPL crossing order triggers TRADE\n";
    std::cout << "========================================\n";

    // BUY 25@152.5 should cross the resting SELL 15@152.0
    // Expected: TRADE price 152.0 qty 15, remaining BUY 10@152.5 rests
    exchange.addOrder("AAPL", {8, BUY, 152.5, 25, 1007LL});
    exchange.matchOrders("AAPL");
    exchange.printBook("AAPL");
    std::cout << "\n";

    // ========================================================================
    // Test 5: CANCEL an existing order (AAPL)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 5: Cancel AAPL order ID=2 (BUY 20@149.5)\n";
    std::cout << "========================================\n";

    exchange.cancelOrder(2);
    exchange.printBook("AAPL");
    std::cout << "\n";

    // ========================================================================
    // Test 6: CANCEL a non-existent order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 6: Cancel non-existent order ID=999\n";
    std::cout << "========================================\n";

    exchange.cancelOrder(999);
    std::cout << "\n";

    // ========================================================================
    // Test 7: MODIFY with quantity decrease (keep priority)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 7: Modify AAPL ID=1 qty 10->5 (priority kept)\n";
    std::cout << "========================================\n";

    exchange.modifyOrder(1, "AAPL", 5, 150.0, 2000LL);
    exchange.printBook("AAPL");
    std::cout << "\n";

    // ========================================================================
    // Test 8: MODIFY with price change (cancel+reinsert)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 8: Modify AAPL ID=4 price 153.0->151.0 (reinsert)\n";
    std::cout << "========================================\n";

    exchange.modifyOrder(4, "AAPL", 10, 151.0, 3000LL);
    exchange.matchOrders("AAPL");
    exchange.printBook("AAPL");
    std::cout << "\n";

    // ========================================================================
    // Test 9: MODIFY with wrong symbol (rejected)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 9: Modify AAPL order ID=1 with wrong symbol TSLA\n";
    std::cout << "========================================\n";

    exchange.modifyOrder(1, "TSLA", 10, 155.0, 4000LL);
    std::cout << "\n";

    // ========================================================================
    // Test 10: MODIFY a non-existent order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 10: Modify non-existent order ID=888\n";
    std::cout << "========================================\n";

    exchange.modifyOrder(888, "AAPL", 10, 50.0, 4000LL);
    std::cout << "\n";

    // ========================================================================
    // Test 11: Third symbol (INFY) independence
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 11: INFY orders (third symbol)\n";
    std::cout << "========================================\n";

    exchange.addOrder("INFY", {9,  BUY,  1800.0, 200, 5000LL});
    exchange.addOrder("INFY", {10, SELL, 1805.0, 100, 5001LL});
    exchange.addOrder("INFY", {11, BUY,  1805.0, 50,  5002LL});
    exchange.matchOrders("INFY");
    exchange.printBook("INFY");
    std::cout << "\n";

    // ========================================================================
    // Test 12: BOOK display for non-existent symbol
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 12: BOOK for non-existent symbol\n";
    std::cout << "========================================\n";

    exchange.printBook("GOOG");
    std::cout << "\n";

    // ========================================================================
    // Test 13: STATS — per-symbol breakdown
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 13: Exchange Statistics\n";
    std::cout << "========================================\n";

    exchange.printStats();

    // ========================================================================
    // Final book state — all symbols
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Final book state — all symbols\n";
    std::cout << "========================================\n";
    exchange.printBook("AAPL");
    exchange.printBook("TSLA");
    exchange.printBook("INFY");

    return 0;
}
