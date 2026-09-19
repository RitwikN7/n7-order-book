#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"
#include "order-book/trade.hpp"

#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

using namespace MatchingEngine;

namespace
{

constexpr double PRICE_SCALE = 10'000.0; // 10^4 fixed-point scaling factor (4 decimal places)

void printBookState(const Book& book)
{
    std::cout << "  --------------------------------------------------------\n";
    std::cout << "  Current Active Orders: " << book.orderCount() << "\n";

    if (auto best_bid = book.bestBid())
        std::cout << "  Best Bid:              $" << std::fixed << std::setprecision(4)
                  << (*best_bid / PRICE_SCALE) << " (" << *best_bid << " ticks)\n";
    else
        std::cout << "  Best Bid:              None\n";

    if (auto best_ask = book.bestAsk())
        std::cout << "  Best Ask:              $" << std::fixed << std::setprecision(4)
                  << (*best_ask / PRICE_SCALE) << " (" << *best_ask << " ticks)\n";
    else
        std::cout << "  Best Ask:              None\n";

    if (auto spread = book.spread())
        std::cout << "  Spread:                $" << std::fixed << std::setprecision(4)
                  << (*spread / PRICE_SCALE) << " (" << *spread << " ticks)\n";
    else
        std::cout << "  Spread:                N/A\n";

    std::cout << "  --------------------------------------------------------\n";
}

void printTrades(std::string_view label, const std::vector<Trade>& trades)
{
    std::cout << "  --> Matches generated for " << label << " (" << trades.size() << " trade"
              << (trades.size() == 1 ? "" : "s") << "):\n";
    for (std::size_t i = 0; i < trades.size(); ++i)
    {
        const auto& t = trades[i];
        std::cout << "      Trade #" << (i + 1) << ": Maker Order " << t.maker_order_id
                  << " vs Taker Order " << t.taker_order_id << " | Qty: " << t.quantity << " @ $"
                  << std::fixed << std::setprecision(4) << (t.price / PRICE_SCALE) << " ("
                  << t.price << " ticks)\n";
    }
}

} // namespace

int main()
{
    std::cout << "===============================================================\n";
    std::cout << "           N7 LOW-LATENCY ORDER BOOK ENGINE (C++23)            \n";
    std::cout << "===============================================================\n\n";

    std::cout << "[INFO] Initializing Order Book (Capacity: " << Book::DEFAULT_MAX_ORDERS
              << " orders, " << Book::DEFAULT_HEADROOM_FACTOR << "x headroom)...\n";
    Book book;

    std::cout << "\n[STEP 1] Inserting resting limit orders into both sides...\n";
    // Resting bids
    book.addOrder(OrderData{
        .order_id = 101,
        .price = 1'000'000, // $100.0000
        .quantity = 50,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    book.addOrder(OrderData{
        .order_id = 102,
        .price = 995'000, // $99.5000
        .quantity = 100,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });

    // Resting asks
    book.addOrder(OrderData{
        .order_id = 201,
        .price = 1'005'000, // $100.5000
        .quantity = 30,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    book.addOrder(OrderData{
        .order_id = 202,
        .price = 1'010'000, // $101.0000
        .quantity = 70,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });

    printBookState(book);

    std::cout << "\n[STEP 2] Submitting an aggressive crossing market buy order (qty 45)...\n";
    auto trades1 = book.addOrder(OrderData{
        .order_id = 301,
        .price = 0,
        .quantity = 45,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::MARKET,
    });
    printTrades("Market Buy Order #301", trades1);
    printBookState(book);

    std::cout << "\n[STEP 3] Modifying resting order #102: reduce quantity to 60...\n";
    book.modifyOrder(102, 60);
    printBookState(book);

    std::cout << "\n[STEP 4] Cancelling order #202...\n";
    book.cancelOrder(202);
    printBookState(book);

    std::cout << "\n===============================================================\n";
    std::cout << "                 TEST SUITES & BENCHMARKS                      \n";
    std::cout << "===============================================================\n";
    std::cout << "Unit tests and latency benchmarks are integrated with Google Test (GTest).\n\n";
    std::cout << "  - Run all unit tests:\n";
    std::cout << "      ./build/tests/order_book_tests --gtest_filter=\"OrderBookTest.*\"\n\n";
    std::cout << "  - Run nanosecond latency benchmarks (1M orders, 1.5x preallocated):\n";
    std::cout << "      ./build/tests/order_book_tests --gtest_filter=\"LatencyBenchmark.*\"\n\n";
    std::cout << "  - Run full suite via CTest:\n";
    std::cout << "      ctest --test-dir build --output-on-failure\n\n";
    std::cout << "  - Generate latency charts (matplotlib):\n";
    std::cout << "      .venv/bin/python scripts/plot_latency.py\n";
    std::cout << "===============================================================\n";

    return 0;
}
