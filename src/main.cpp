#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"
#include "order-book/order_builder.hpp"
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
    std::cout << "  Active Resting Orders: " << book.activeOrderCount() << "\n";

    if (auto best_bid = book.bestBid())
        std::cout << "  Best Bid:              $" << std::fixed << std::setprecision(4)
                  << (static_cast<double>(best_bid->value()) / PRICE_SCALE) << " ("
                  << best_bid->value() << " ticks)\n";
    else
        std::cout << "  Best Bid:              None\n";

    if (auto best_ask = book.bestAsk())
        std::cout << "  Best Ask:              $" << std::fixed << std::setprecision(4)
                  << (static_cast<double>(best_ask->value()) / PRICE_SCALE) << " ("
                  << best_ask->value() << " ticks)\n";
    else
        std::cout << "  Best Ask:              None\n";

    if (auto spread = book.spread())
        std::cout << "  Spread:                $" << std::fixed << std::setprecision(4)
                  << (static_cast<double>(spread->value()) / PRICE_SCALE) << " (" << spread->value()
                  << " ticks)\n";
    else
        std::cout << "  Spread:                N/A\n";

    if (auto ltp = book.lastTradedPrice())
        std::cout << "  Last Traded Price:     $" << std::fixed << std::setprecision(4)
                  << (static_cast<double>(ltp->value()) / PRICE_SCALE) << " (" << ltp->value()
                  << " ticks)\n";
    else
        std::cout << "  Last Traded Price:     None\n";

    if (book.stopOrderCount() > 0)
        std::cout << "  Dormant Stop Orders:   " << book.stopOrderCount() << "\n";

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
                  << " vs Taker Order " << t.taker_order_id << " | Qty: " << t.quantity.value()
                  << " @ $" << std::fixed << std::setprecision(4)
                  << (static_cast<double>(t.price.value()) / PRICE_SCALE) << " (" << t.price.value()
                  << " ticks)\n";
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
    book.addOrder(OrderBuilder{}
                      .id(101)
                      .buy()
                      .limit(OrderBookUtils::Price(1'000'000), OrderBookUtils::Quantity(50))
                      .build());
    book.addOrder(OrderBuilder{}
                      .id(102)
                      .buy()
                      .limit(OrderBookUtils::Price(995'000), OrderBookUtils::Quantity(100))
                      .build());

    // Resting asks
    book.addOrder(OrderBuilder{}
                      .id(201)
                      .sell()
                      .limit(OrderBookUtils::Price(1'005'000), OrderBookUtils::Quantity(30))
                      .build());
    book.addOrder(OrderBuilder{}
                      .id(202)
                      .sell()
                      .limit(OrderBookUtils::Price(1'010'000), OrderBookUtils::Quantity(70))
                      .build());

    printBookState(book);

    std::cout << "\n[STEP 2] Submitting an aggressive crossing market buy order (qty 45)...\n";
    auto trades1 =
        book.addOrder(OrderBuilder{}.id(301).buy().market(OrderBookUtils::Quantity(45)).build());
    printTrades("Market Buy Order #301", trades1);
    printBookState(book);

    std::cout << "\n[STEP 3] Modifying resting order #102: reduce quantity to 60...\n";
    book.modifyOrder(102, OrderBookUtils::Quantity(60));
    printBookState(book);

    std::cout << "\n[STEP 4] Cancelling order #202...\n";
    book.cancelOrder(202);
    printBookState(book);

    std::cout << "\n[STEP 5] Submitting a Reserve Order (#401) using OrderDataBuilder (Total: 100, "
                 "Display: 25 @ $100.50)...\n";
    OrderData reserve_order =
        OrderDataBuilder{}
            .id(401)
            .sell()
            .limit(OrderBookUtils::Price(1'005'000))
            .reserve(OrderBookUtils::Quantity(100), OrderBookUtils::Quantity(25))
            .build();
    book.addOrder(reserve_order);
    printBookState(book);

    std::cout << "\n[STEP 6] Placing a dormant Stop Sell Market Order (#501) with Stop Price "
                 "$100.00...\n";
    OrderData stop_order =
        OrderDataBuilder{}
            .id(501)
            .sell()
            .stopMarket(OrderBookUtils::Price(1'000'000), OrderBookUtils::Quantity(30))
            .build();
    book.addOrder(stop_order);
    printBookState(book);

    std::cout << "\n[STEP 7] Submitting crossing market buy (#601, qty 35) hitting Reserve Order "
                 "#401...\n";
    OrderData aggressive_buy =
        OrderDataBuilder{}.id(601).buy().market(OrderBookUtils::Quantity(35)).build();
    auto trades2 = book.addOrder(aggressive_buy);
    printTrades("Aggressive Buy vs Reserve Order #401", trades2);
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
