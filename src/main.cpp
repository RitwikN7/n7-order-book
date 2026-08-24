#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"
#include "order-book/trade.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using namespace MatchingEngine;

constexpr double PRICE_SCALE = 10'000.0; // 10^4 fixed-point scaling factor (4 decimal places)

void printBookState(const Book& book)
{
    std::cout << "--- Order Book State ---\n";
    std::cout << "Total Active Orders: " << book.orderCount() << "\n";

    if (auto best_bid = book.bestBid())
        std::cout << "Best Bid:            $" << std::fixed << std::setprecision(4)
                  << (*best_bid / PRICE_SCALE) << " (" << *best_bid << " ticks)\n";
    else
        std::cout << "Best Bid:            None\n";

    if (auto best_ask = book.bestAsk())
        std::cout << "Best Ask:            $" << std::fixed << std::setprecision(4)
                  << (*best_ask / PRICE_SCALE) << " (" << *best_ask << " ticks)\n";
    else
        std::cout << "Best Ask:            None\n";

    if (auto spread = book.spread())
        std::cout << "Spread:              $" << std::fixed << std::setprecision(4)
                  << (*spread / PRICE_SCALE) << "\n";
    else
        std::cout << "Spread:              N/A\n";

    std::cout << "------------------------\n\n";
}

void runBasicFunctionalTests()
{
    std::cout << "========================================\n";
    std::cout << " 1. BASIC FUNCTIONAL TESTS\n";
    std::cout << "========================================\n";

    Book book(10'000);

    std::cout << "[Step 1] Inserting Limit Bids...\n";
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

    std::cout << "[Step 2] Inserting Limit Asks...\n";
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

    std::cout << "[Step 3] Sending Market Buy Order (Qty: 40)...\n";
    OrderData market_buy{
        .order_id = 301,
        .price = 0,
        .quantity = 40,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::MARKET,
    };

    auto trades = book.addOrder(market_buy);
    std::cout << ">>> Trades Executed: " << trades.size() << "\n";
    for (const auto& trade : trades)
    {
        std::cout << "  - Trade: Filled " << trade.quantity << " units @ $" << std::fixed
                  << std::setprecision(4) << (trade.price / PRICE_SCALE) << " (Maker Order #"
                  << trade.maker_order_id << " vs Taker Order #" << trade.taker_order_id << ")\n";
    }
    std::cout << "\n";

    printBookState(book);

    std::cout << "[Step 4] Modifying Order #102: Price $99.7500, Qty 80...\n";
    book.modifyOrder(102, 997'500, 80);

    printBookState(book);

    std::cout << "[Step 5] Cancelling Order #101...\n";
    book.cancelOrder(101);

    printBookState(book);
}

void runBenchmarkLimitInsertions(std::size_t num_orders)
{
    std::cout << "========================================\n";
    std::cout << " 2. BENCHMARK: BULK LIMIT INSERTIONS (" << num_orders << " orders)\n";
    std::cout << "========================================\n";

    Book book(num_orders + 1000);

    // Generate test data before timing to measure only order book operations
    std::vector<OrderData> orders;
    orders.reserve(num_orders);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<OrderBookUtils::Price> bid_prices(900'000, 999'999);
    std::uniform_int_distribution<OrderBookUtils::Price> ask_prices(1'000'001, 1'100'000);
    std::uniform_int_distribution<OrderBookUtils::Quantity> quantities(1, 500);

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        bool is_buy = (i % 2 == 0);
        orders.push_back(OrderData{
            .order_id = static_cast<OrderBookUtils::OrderID>(i + 1),
            .price = is_buy ? bid_prices(rng) : ask_prices(rng),
            .quantity = quantities(rng),
            .side = is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL,
            .type = OrderBookUtils::OrderType::LIMIT,
        });
    }

    auto start = std::chrono::steady_clock::now();

    for (const auto& order : orders)
    {
        book.addOrder(order);
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(elapsed_ns) / static_cast<double>(num_orders);
    double avg_us = static_cast<double>(elapsed_us) / static_cast<double>(num_orders);
    double throughput = (static_cast<double>(num_orders) / (static_cast<double>(elapsed_us) / 1e6));

    std::cout << "Elapsed Time:       " << elapsed_us << " us (" << elapsed_us / 1000.0 << " ms)\n";
    std::cout << "Avg Time per Order: " << std::fixed << std::setprecision(2) << avg_ns << " ns ("
              << std::fixed << std::setprecision(4) << avg_us << " us)\n";
    std::cout << "Throughput:         " << std::fixed << std::setprecision(0) << throughput
              << " orders/sec\n";
    printBookState(book);
}

void runBenchmarkCancellations(std::size_t num_orders)
{
    std::cout << "========================================\n";
    std::cout << " 3. BENCHMARK: O(1) CANCELLATIONS (" << num_orders << " cancels)\n";
    std::cout << "========================================\n";

    Book book(num_orders + 1000);

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        book.addOrder(OrderData{
            .order_id = static_cast<OrderBookUtils::OrderID>(i + 1),
            .price = static_cast<OrderBookUtils::Price>(1'000'000 + (i % 500)),
            .quantity = 10,
            .side = (i % 2 == 0) ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL,
            .type = OrderBookUtils::OrderType::LIMIT,
        });
    }

    auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        book.cancelOrder(static_cast<OrderBookUtils::OrderID>(i + 1));
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(elapsed_ns) / static_cast<double>(num_orders);
    double avg_us = static_cast<double>(elapsed_us) / static_cast<double>(num_orders);
    double throughput = (static_cast<double>(num_orders) / (static_cast<double>(elapsed_us) / 1e6));

    std::cout << "Elapsed Time:       " << elapsed_us << " us (" << elapsed_us / 1000.0 << " ms)\n";
    std::cout << "Avg Time per Cancel:" << std::fixed << std::setprecision(2) << avg_ns << " ns ("
              << std::fixed << std::setprecision(4) << avg_us << " us)\n";
    std::cout << "Throughput:         " << std::fixed << std::setprecision(0) << throughput
              << " cancels/sec\n";
    printBookState(book);
}

void runBenchmarkRealisticTradingWorkload(std::size_t num_events)
{
    std::cout << "========================================\n";
    std::cout << " 4. BENCHMARK: MIXED HIGH-FREQUENCY SIMULATION (" << num_events << " events)\n";
    std::cout << "    (60% Limit Orders, 20% Cancels, 15% Market Orders, 5% Modifies)\n";
    std::cout << "========================================\n";

    Book book(num_events + 10'000);

    enum class ActionType : std::uint8_t
    {
        NEW_LIMIT,
        CANCEL,
        MARKET,
        MODIFY
    };

    struct Event
    {
        ActionType action;
        OrderData order;
        OrderBookUtils::OrderID target_id;
        OrderBookUtils::Price new_price;
        OrderBookUtils::Quantity new_qty;
    };

    std::vector<Event> events;
    events.reserve(num_events);

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> action_dist(1, 100);
    std::uniform_int_distribution<OrderBookUtils::Price> bid_dist(990'000, 1'000'000);
    std::uniform_int_distribution<OrderBookUtils::Price> ask_dist(1'000'010, 1'010'000);
    std::uniform_int_distribution<OrderBookUtils::Quantity> qty_dist(1, 100);

    std::vector<OrderBookUtils::OrderID> active_ids;
    OrderBookUtils::OrderID next_id = 1;

    for (std::size_t i = 0; i < num_events; ++i)
    {
        int roll = action_dist(rng);

        if (roll <= 60 || active_ids.empty()) // 60% New Limit
        {
            bool is_buy = (roll % 2 == 0);
            OrderData order{
                .order_id = next_id++,
                .price = is_buy ? bid_dist(rng) : ask_dist(rng),
                .quantity = qty_dist(rng),
                .side = is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL,
                .type = OrderBookUtils::OrderType::LIMIT,
            };
            active_ids.push_back(order.order_id);
            events.push_back(Event{.action = ActionType::NEW_LIMIT, .order = order});
        }
        else if (roll <= 80) // 20% Cancel
        {
            std::size_t idx = rng() % active_ids.size();
            OrderBookUtils::OrderID id_to_cancel = active_ids[idx];
            events.push_back(Event{.action = ActionType::CANCEL, .target_id = id_to_cancel});
        }
        else if (roll <= 95) // 15% Market Order
        {
            bool is_buy = (roll % 2 == 0);
            OrderData order{
                .order_id = next_id++,
                .price = 0,
                .quantity = (qty_dist(rng) / 2) + 1,
                .side = is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL,
                .type = OrderBookUtils::OrderType::MARKET,
            };
            events.push_back(Event{.action = ActionType::MARKET, .order = order});
        }
        else // 5% Modify
        {
            std::size_t idx = rng() % active_ids.size();
            OrderBookUtils::OrderID id_to_mod = active_ids[idx];
            events.push_back(Event{
                .action = ActionType::MODIFY,
                .target_id = id_to_mod,
                .new_price = bid_dist(rng),
                .new_qty = qty_dist(rng),
            });
        }
    }

    std::size_t total_trades = 0;

    auto start = std::chrono::steady_clock::now();

    for (const auto& ev : events)
    {
        switch (ev.action)
        {
        case ActionType::NEW_LIMIT:
        case ActionType::MARKET:
        {
            auto trades = book.addOrder(ev.order);
            total_trades += trades.size();
            break;
        }
        case ActionType::CANCEL:
        {
            book.cancelOrder(ev.target_id);
            break;
        }
        case ActionType::MODIFY:
        {
            book.modifyOrder(ev.target_id, ev.new_price, ev.new_qty);
            break;
        }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(elapsed_ns) / static_cast<double>(num_events);
    double avg_us = static_cast<double>(elapsed_us) / static_cast<double>(num_events);
    double throughput = (static_cast<double>(num_events) / (static_cast<double>(elapsed_us) / 1e6));

    std::cout << "Total Events Processed: " << num_events << "\n";
    std::cout << "Total Matches Executed: " << total_trades << "\n";
    std::cout << "Elapsed Time:           " << elapsed_us << " us (" << elapsed_us / 1000.0
              << " ms)\n";
    std::cout << "Avg Time per Event:     " << std::fixed << std::setprecision(2) << avg_ns
              << " ns (" << std::fixed << std::setprecision(4) << avg_us << " us)\n";
    std::cout << "Throughput:             " << std::fixed << std::setprecision(0) << throughput
              << " ops/sec\n";
    printBookState(book);
}

int main()
{
    std::cout << "===============================================================\n";
    std::cout << "      N7 LOW-LATENCY ORDER BOOK BENCHMARK & TEST SUITE         \n";
    std::cout << "===============================================================\n\n";

    runBasicFunctionalTests();
    runBenchmarkLimitInsertions(1'000'000);
    runBenchmarkCancellations(1'000'000);
    runBenchmarkRealisticTradingWorkload(1'000'000);

    return 0;
}
