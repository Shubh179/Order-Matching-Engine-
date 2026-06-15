#include <iostream>
#include "order.h"
#include "orderbook.h"

// ============================================================================
// Standalone test runner for the OrderBook matching engine.
//
// Tests exercised:
//   1. NEW orders with no crossing (resting bids and asks)
//   2. NEW order that crosses and triggers a TRADE
//   3. CANCEL an existing order (verify removal)
//   4. CANCEL a non-existent order (verify rejection)
//   5. MODIFY with quantity decrease only (verify priority preserved)
//   6. MODIFY with price change (verify cancel+reinsert, priority lost)
//   7. MODIFY a non-existent order (verify rejection)
//   8. Full fill removing orders from the orderMap
// ============================================================================

int main() {
    OrderBook book;

    // ========================================================================
    // Test 1: Add resting orders (no crossing)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 1: Adding resting orders (no cross)\n";
    std::cout << "========================================\n";

    book.addOrder({1, BUY,  100.0, 10, 1000LL});
    book.addOrder({2, BUY,  99.5,  20, 1001LL});
    book.addOrder({3, SELL, 102.0, 15, 1002LL});
    book.addOrder({4, SELL, 103.0, 10, 1003LL});

    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 2: Crossing order triggers a TRADE
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 2: Crossing BUY triggers TRADE\n";
    std::cout << "========================================\n";

    // BUY 25@102.5 should cross the resting SELL 15@102.0
    // Expected: TRADE price 102.0 qty 15, remaining BUY 10@102.5 rests
    book.addOrder({5, BUY, 102.5, 25, 1004LL});
    book.matchOrders();
    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 3: CANCEL an existing order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 3: Cancel order ID=2 (BUY 20@99.5)\n";
    std::cout << "========================================\n";

    book.cancelOrder(2);
    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 4: CANCEL a non-existent order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 4: Cancel non-existent order ID=999\n";
    std::cout << "========================================\n";

    book.cancelOrder(999);
    std::cout << "\n";

    // ========================================================================
    // Test 5: MODIFY with quantity decrease (keep priority)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 5: Modify ID=1 qty 10->5 (same price, priority kept)\n";
    std::cout << "========================================\n";

    // Order 1 is BUY 10@100.0 — reduce to 5@100.0
    book.modifyOrder(1, 5, 100.0, 2000LL);
    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 6: MODIFY with price change (cancel+reinsert, priority lost)
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 6: Modify ID=4 price 103.0->101.0 (reinsert)\n";
    std::cout << "========================================\n";

    // Order 4 is SELL 10@103.0 — change to 10@101.0
    book.modifyOrder(4, 10, 101.0, 3000LL);
    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 7: MODIFY after price change should trigger matching
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 7: Match after modify (SELL 10@101.0 vs BUY 10@102.5)\n";
    std::cout << "========================================\n";

    // After Test 6, SELL 10@101.0 should cross with resting BUY 10@102.5
    book.matchOrders();
    book.printBook();
    std::cout << "\n";

    // ========================================================================
    // Test 8: MODIFY a non-existent order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 8: Modify non-existent order ID=888\n";
    std::cout << "========================================\n";

    book.modifyOrder(888, 10, 50.0, 4000LL);
    std::cout << "\n";

    // ========================================================================
    // Test 9: CANCEL an already-filled order
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Test 9: Cancel already-filled order ID=5\n";
    std::cout << "========================================\n";

    // Order 5 was fully filled in Test 7 — should be rejected
    book.cancelOrder(5);
    std::cout << "\n";

    // ========================================================================
    // Final book state
    // ========================================================================
    std::cout << "========================================\n";
    std::cout << "Final book state\n";
    std::cout << "========================================\n";
    book.printBook();

    return 0;
}
