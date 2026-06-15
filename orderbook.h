#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <map>
#include <list>
#include <iostream>
#include <functional>
#include <unordered_map>
#include "order.h"

// ============================================================================
// OrderLocation: Stores everything needed to locate an order in O(1).
//
// Design rationale:
//   - side:  tells us which book side (bids/asks) the order lives in
//   - price: the map key — needed to find the price-level list in O(1)
//   - it:    a std::list iterator — stable across inserts/erases elsewhere,
//            enabling O(1) erase from the middle of the queue
//
// Why std::list instead of std::deque?
//   std::deque does NOT support O(1) erase-by-iterator in the middle
//   (elements must shift, O(n)). std::list provides:
//     - O(1) erase anywhere via iterator
//     - O(1) push_back for new orders
//     - Stable iterators (never invalidated by other operations)
//   The trade-off is slightly worse cache locality during iteration,
//   but this is the standard approach in exchange simulators and is
//   required for the O(1) cancel/modify guarantee.
//
// Complexity:
//   sizeof(OrderLocation) ≈ 24 bytes (Side + double + iterator)
// ============================================================================
struct OrderLocation {
    Side   side;
    double price;
    std::list<Order>::iterator it;
};

// ============================================================================
// OrderBook: Price-time priority matching engine with O(1) cancel/modify.
//
// Data structures:
//   bids: map<double, list<Order>, greater<double>>
//         Sorted descending — best (highest) bid at begin().
//
//   asks: map<double, list<Order>>
//         Sorted ascending  — best (lowest) ask at begin().
//
//   orderMap: unordered_map<uint64_t, OrderLocation>
//         O(1) lookup by order ID → direct access to the order's
//         side, price level, and list iterator.
//
// Complexity summary:
//   | Operation          | Complexity                              |
//   |--------------------|-----------------------------------------|
//   | addOrder           | O(log P) map insert + O(1) list + O(1) umap |
//   | cancelOrder        | O(1) umap lookup + O(1) list erase      |
//   | modifyOrder        | O(1) for qty-down; O(log P) for price Δ |
//   | matchOrders (fill) | O(1) list front/pop + O(1) umap erase   |
//   | lookup             | O(1) umap find                          |
//
//   P = number of distinct price levels
// ============================================================================
class OrderBook {
private:
    // Price-level books: each level is a FIFO queue (std::list) of orders
    std::map<double, std::list<Order>, std::greater<double>> bids;
    std::map<double, std::list<Order>> asks;

    // O(1) order lookup index
    std::unordered_map<uint64_t, OrderLocation> orderMap;

    // ========================================================================
    // Internal: remove an order given its OrderLocation. O(1).
    // Cleans up empty price levels from the map.
    // ========================================================================
    void removeByLocation(const OrderLocation& loc) {
        if (loc.side == BUY) {
            auto mapIt = bids.find(loc.price);
            if (mapIt != bids.end()) {
                mapIt->second.erase(loc.it);
                if (mapIt->second.empty()) {
                    bids.erase(mapIt);
                }
            }
        } else {
            auto mapIt = asks.find(loc.price);
            if (mapIt != asks.end()) {
                mapIt->second.erase(loc.it);
                if (mapIt->second.empty()) {
                    asks.erase(mapIt);
                }
            }
        }
    }

    // ========================================================================
    // Internal: insert an order into the correct book side and register
    // it in the orderMap. Returns the list iterator for the new entry.
    // ========================================================================
    void insertOrder(const Order& o) {
        std::list<Order>::iterator it;

        if (o.side == BUY) {
            auto& queue = bids[o.price];
            queue.push_back(o);
            it = std::prev(queue.end());
        } else {
            auto& queue = asks[o.price];
            queue.push_back(o);
            it = std::prev(queue.end());
        }

        orderMap[o.orderId] = OrderLocation{o.side, o.price, it};
    }

public:
    // ========================================================================
    // addOrder: Insert a new order into the book.
    //   Complexity: O(log P) for map insertion + O(1) list + O(1) umap
    // ========================================================================
    void addOrder(const Order& o) {
        insertOrder(o);
        std::cout << "[NEW] Order ID=" << o.orderId
                  << " " << (o.side == BUY ? "BUY" : "SELL")
                  << " " << o.quantity << "@" << o.price << "\n";
    }

    // ========================================================================
    // cancelOrder: Remove an order by ID in O(1).
    //   1. O(1) lookup in orderMap
    //   2. O(1) erase from std::list via stored iterator
    //   3. O(1) erase from orderMap
    //   4. Clean up empty price level (O(log P) map erase, amortized)
    // ========================================================================
    void cancelOrder(uint64_t orderId) {
        auto mapIt = orderMap.find(orderId);
        if (mapIt == orderMap.end()) {
            std::cout << "[CANCEL REJECTED] Order ID=" << orderId
                      << " not found\n";
            return;
        }

        const OrderLocation& loc = mapIt->second;
        std::cout << "[CANCEL] Order ID=" << orderId
                  << " " << (loc.side == BUY ? "BUY" : "SELL")
                  << " " << loc.it->quantity << "@" << loc.price << "\n";

        removeByLocation(loc);
        orderMap.erase(mapIt);
    }

    // ========================================================================
    // modifyOrder: Modify an existing order's quantity and/or price.
    //
    // Exchange-style modify semantics (CME policy):
    //   - If price changes → cancel old + insert new (loses time priority)
    //   - If qty increases (same price) → cancel old + insert new (loses priority)
    //   - If qty decreases only (same price) → in-place update (keeps priority)
    //
    // Why this policy?
    //   Price changes affect matching priority — the order must move to a
    //   different price level, so it naturally loses its place.
    //   Quantity increases could allow a trader to "reserve" a favorable
    //   queue position with a small order and then inflate it — most
    //   exchanges prevent this by treating it as a new order.
    //   Quantity decreases are harmless — the order is already at its
    //   priority position and is only getting smaller.
    //
    // Complexity:
    //   qty-down same price: O(1) lookup + O(1) in-place write
    //   otherwise:           O(1) cancel + O(log P) insert
    // ========================================================================
    void modifyOrder(uint64_t orderId, int newQty, double newPrice,
                     long long timestamp) {
        auto mapIt = orderMap.find(orderId);
        if (mapIt == orderMap.end()) {
            std::cout << "[MODIFY REJECTED] Order ID=" << orderId
                      << " not found\n";
            return;
        }

        const OrderLocation& loc = mapIt->second;
        Order& existing = *(loc.it);
        double oldPrice = existing.price;
        int    oldQty   = existing.quantity;

        // Case 1: Same price AND quantity decrease → in-place (keep priority)
        if (newPrice == oldPrice && newQty < oldQty) {
            std::cout << "[MODIFY] Order ID=" << orderId
                      << " qty " << oldQty << " -> " << newQty
                      << " (in-place, priority preserved)\n";
            existing.quantity = newQty;
            return;
        }

        // Case 2: Price change or quantity increase → cancel + reinsert
        Side side = existing.side;
        std::cout << "[MODIFY] Order ID=" << orderId
                  << " " << oldQty << "@" << oldPrice
                  << " -> " << newQty << "@" << newPrice
                  << " (cancel+reinsert, new timestamp)\n";

        // Remove old
        removeByLocation(loc);
        orderMap.erase(mapIt);

        // Insert new with fresh timestamp (loses time priority)
        Order newOrder;
        newOrder.orderId   = orderId;  // Keep same ID
        newOrder.side      = side;
        newOrder.price     = newPrice;
        newOrder.quantity  = newQty;
        newOrder.timestamp = timestamp;

        insertOrder(newOrder);
    }

    // ========================================================================
    // matchOrders: Match crossing orders using price-time priority.
    //
    // A trade occurs when Best Bid >= Best Ask.
    // Trade price = resting order's price (the one with earlier timestamp).
    //
    // When an order is fully filled:
    //   - Removed from the price-level list
    //   - Removed from orderMap (critical for consistency)
    //   - Empty price levels cleaned up
    //
    // Complexity per fill: O(1) list front/pop + O(1) umap erase
    // ========================================================================
    void matchOrders() {
        while (!bids.empty() && !asks.empty()) {
            auto bidIt = bids.begin();
            auto askIt = asks.begin();

            double bidPrice = bidIt->first;
            double askPrice = askIt->first;

            if (bidPrice < askPrice) {
                break; // No match possible
            }

            auto& bidQueue = bidIt->second;
            auto& askQueue = askIt->second;

            auto& firstBid = bidQueue.front();
            auto& firstAsk = askQueue.front();

            int tradeQty = std::min(firstBid.quantity, firstAsk.quantity);
            // Trade price = resting order's price (earlier timestamp)
            double tradePrice = (firstBid.timestamp <= firstAsk.timestamp)
                                ? firstBid.price
                                : firstAsk.price;

            std::cout << "[TRADE] price " << tradePrice
                      << " qty " << tradeQty
                      << " (buyer=" << firstBid.orderId
                      << ", seller=" << firstAsk.orderId << ")\n";

            firstBid.quantity -= tradeQty;
            firstAsk.quantity -= tradeQty;

            // Remove fully filled orders from both list and orderMap
            if (firstBid.quantity == 0) {
                orderMap.erase(firstBid.orderId);
                bidQueue.pop_front();
            }
            if (firstAsk.quantity == 0) {
                orderMap.erase(firstAsk.orderId);
                askQueue.pop_front();
            }

            // Clean up empty price levels
            if (bidQueue.empty()) {
                bids.erase(bidIt);
            }
            if (askQueue.empty()) {
                asks.erase(askIt);
            }
        }
    }

    // ========================================================================
    // printBook: Display top 5 price levels for each side.
    // ========================================================================
    void printBook() const {
        std::cout << "=== Bids (Top 5) ===\n";
        int count = 0;
        for (const auto& [price, queue] : bids) {
            if (count >= 5) break;
            int totalQty = 0;
            for (const auto& o : queue) {
                totalQty += o.quantity;
            }
            std::cout << "Price: " << price
                      << " | Total Qty: " << totalQty
                      << " (" << queue.size() << " orders)\n";
            count++;
        }
        if (count == 0) {
            std::cout << "Empty\n";
        }

        std::cout << "=== Asks (Top 5) ===\n";
        count = 0;
        for (const auto& [price, queue] : asks) {
            if (count >= 5) break;
            int totalQty = 0;
            for (const auto& o : queue) {
                totalQty += o.quantity;
            }
            std::cout << "Price: " << price
                      << " | Total Qty: " << totalQty
                      << " (" << queue.size() << " orders)\n";
            count++;
        }
        if (count == 0) {
            std::cout << "Empty\n";
        }
    }
};

#endif // ORDERBOOK_H
