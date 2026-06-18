#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <string>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include "orderbook.h"

// ============================================================================
// Exchange: Multi-symbol order routing and management.
//
// Design rationale:
//   - Each symbol gets its own independent OrderBook instance.
//   - OrderBooks are created lazily on first order for a symbol.
//   - A global orderSymbolMap provides O(1) orderId → symbol resolution
//     for CANCEL and MODIFY commands (which may not carry a symbol).
//
// Thread safety: NONE (single-threaded by design — lives on matching thread)
//
// Complexity overhead vs single OrderBook:
//   - addOrder:    +O(1) unordered_map insert (orderSymbolMap)
//   - cancelOrder: +O(1) unordered_map lookup + erase
//   - modifyOrder: +O(1) unordered_map lookup
//   - matchOrders: +O(1) unordered_map lookup (books)
// ============================================================================
class Exchange {
private:
    // Per-symbol order books — created lazily on first order
    std::unordered_map<std::string, OrderBook> books;

    // Global order ID → symbol mapping for CANCEL/MODIFY dispatch.
    // Needed because CANCEL only carries orderId (no symbol).
    std::unordered_map<uint64_t, std::string> orderSymbolMap;

public:
    // ========================================================================
    // addOrder: Route a new order to the correct symbol's OrderBook.
    //   Creates the OrderBook lazily if this is the first order for the symbol.
    //   Registers the orderId → symbol mapping for future CANCEL/MODIFY.
    // ========================================================================
    void addOrder(const std::string& symbol, const Order& o) {
        books[symbol].addOrder(o);
        orderSymbolMap[o.orderId] = symbol;
    }

    // ========================================================================
    // cancelOrder: Cancel an order by ID across all symbols.
    //   Uses orderSymbolMap for O(1) resolution of which book the order is in.
    //   Cleans up the orderSymbolMap entry after successful cancel.
    // ========================================================================
    void cancelOrder(uint64_t orderId) {
        auto it = orderSymbolMap.find(orderId);
        if (it == orderSymbolMap.end()) {
            std::cout << "[CANCEL REJECTED] Order ID=" << orderId
                      << " not found in any book\n";
            return;
        }
        books[it->second].cancelOrder(orderId);
        orderSymbolMap.erase(it);
    }

    // ========================================================================
    // modifyOrder: Modify an order with symbol validation.
    //   The symbol parameter serves as a safety check — if the order belongs
    //   to a different symbol, the modify is rejected (exchange standard).
    // ========================================================================
    void modifyOrder(uint64_t orderId, const std::string& symbol,
                     int newQty, double newPrice, long long timestamp) {
        auto it = orderSymbolMap.find(orderId);
        if (it == orderSymbolMap.end()) {
            std::cout << "[MODIFY REJECTED] Order ID=" << orderId
                      << " not found in any book\n";
            return;
        }

        // Validate symbol matches the order's actual book
        if (it->second != symbol) {
            std::cout << "[MODIFY REJECTED] Order ID=" << orderId
                      << " belongs to " << it->second
                      << ", not " << symbol << "\n";
            return;
        }

        books[symbol].modifyOrder(orderId, newQty, newPrice, timestamp);
    }

    // ========================================================================
    // matchOrders: Run matching for a specific symbol's OrderBook.
    // ========================================================================
    void matchOrders(const std::string& symbol) {
        auto it = books.find(symbol);
        if (it != books.end()) {
            it->second.matchOrders();
        }
    }

    // ========================================================================
    // printBook: Display the order book for a specific symbol.
    // ========================================================================
    void printBook(const std::string& symbol) const {
        auto it = books.find(symbol);
        if (it != books.end()) {
            it->second.printBook(symbol);
        } else {
            std::cout << "[BOOK] Symbol " << symbol << " not found\n";
        }
    }

    // ========================================================================
    // printStats: Display per-symbol statistics for all traded symbols.
    // ========================================================================
    void printStats() const {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout << "║         EXCHANGE STATISTICS                  ║\n";
        std::cout << "╠══════════════════════════════════════════════╣\n";

        if (books.empty()) {
            std::cout << "║  No symbols traded yet.                     ║\n";
        }

        for (const auto& [sym, book] : books) {
            const SymbolStats& s = book.getStats();
            std::cout << "║  " << sym << "\n";
            std::cout << "║    Orders : " << s.totalOrders << "\n";
            std::cout << "║    Trades : " << s.totalTrades << "\n";
            std::cout << "║    Volume : " << s.totalVolume << "\n";
            std::cout << "║\n";
        }

        std::cout << "╚══════════════════════════════════════════════╝\n\n";
    }
};

#endif // EXCHANGE_H
