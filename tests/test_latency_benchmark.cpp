#include "common/utils.hpp"
#include "latency_tracker.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"
#include "order-book/order_builder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace MatchingEngine;
using namespace OrderBookBenchmark;

namespace
{

constexpr std::size_t BENCHMARK_ORDERS_COUNT = 1'000'000;
constexpr double BENCHMARK_HEADROOM_FACTOR = 1.2;

class LatencyBenchmarkTest : public ::testing::Test
{
protected:
    static inline LatencyStats s_bulk_insert_stats;
    static inline LatencyStats s_bulk_cancel_stats;
    static inline MixedBenchmarkResults s_mixed_results;
    static inline bool s_has_insert{false};
    static inline bool s_has_cancel{false};
    static inline bool s_has_mixed{false};

    static void TearDownTestSuite()
    {
        if (s_has_insert || s_has_cancel || s_has_mixed)
        {
            exportLatencyMetricsToCsv("data/latency_metrics.csv", s_bulk_insert_stats,
                                      s_bulk_cancel_stats, s_mixed_results);
        }
    }
};

TEST_F(LatencyBenchmarkTest, BulkLimitInsertions)
{
    const std::size_t num_orders = BENCHMARK_ORDERS_COUNT;
    const auto book_capacity = static_cast<std::size_t>(num_orders * BENCHMARK_HEADROOM_FACTOR);

    std::cout << "===============================================================\n";
    std::cout << " BENCHMARK: BULK LIMIT INSERTIONS (" << num_orders << " orders)\n";
    std::cout << " (Order Book Preallocated Capacity: " << book_capacity << " slots ["
              << BENCHMARK_HEADROOM_FACTOR << "x])\n";
    std::cout << "===============================================================\n";

    Book book(book_capacity);
    LatencyTracker tracker("Bulk Limit Insertions", num_orders);

    std::vector<OrderData> orders;
    orders.reserve(num_orders);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<OrderBookUtils::Price::rep> bid_prices(900'000, 999'999);
    std::uniform_int_distribution<OrderBookUtils::Price::rep> ask_prices(1'000'001, 1'100'000);
    std::uniform_int_distribution<OrderBookUtils::Quantity::rep> quantities(1, 500);

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        bool is_buy = (i % 2 == 0);
        orders.push_back(
            OrderBuilder{}
                .id(static_cast<OrderBookUtils::OrderID>(i + 1))
                .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                .limit(OrderBookUtils::Price(is_buy ? bid_prices(rng) : ask_prices(rng)),
                       OrderBookUtils::Quantity(quantities(rng)))
                .build());
    }

    auto total_start = std::chrono::steady_clock::now();

    for (const auto& order : orders)
    {
        auto t0 = std::chrono::steady_clock::now();
        book.addOrder(order);
        auto t1 = std::chrono::steady_clock::now();

        auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        tracker.record(static_cast<std::uint64_t>(dur_ns));
    }

    auto total_end = std::chrono::steady_clock::now();
    auto total_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();

    s_bulk_insert_stats = tracker.computeStats(static_cast<double>(total_elapsed_us));
    s_has_insert = true;

    LatencyTracker::printStatsTable(s_bulk_insert_stats);
    tracker.printHistogram(10, 36);
    printBookState(book);

    EXPECT_EQ(book.orderCount(), num_orders);
    EXPECT_GT(s_bulk_insert_stats.throughput_ops_sec, 0.0);
}

TEST_F(LatencyBenchmarkTest, BulkCancellations)
{
    const std::size_t num_orders = BENCHMARK_ORDERS_COUNT;
    const auto book_capacity = static_cast<std::size_t>(num_orders * BENCHMARK_HEADROOM_FACTOR);

    std::cout << "\n===============================================================\n";
    std::cout << " BENCHMARK: O(1) CANCELLATIONS (" << num_orders << " cancels)\n";
    std::cout << " (Order Book Preallocated Capacity: " << book_capacity << " slots ["
              << BENCHMARK_HEADROOM_FACTOR << "x])\n";
    std::cout << "===============================================================\n";

    Book book(book_capacity);
    LatencyTracker tracker("Bulk Cancellations", num_orders);

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        bool is_buy = (i % 2 == 0);
        book.addOrder(
            OrderBuilder{}
                .id(static_cast<OrderBookUtils::OrderID>(i + 1))
                .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                .limit(
                    OrderBookUtils::Price(is_buy ? (990'000 - (i % 500)) : (1'010'000 + (i % 500))),
                    OrderBookUtils::Quantity(10))
                .build());
    }

    ASSERT_EQ(book.orderCount(), num_orders);

    auto total_start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < num_orders; ++i)
    {
        auto order_id = static_cast<OrderBookUtils::OrderID>(i + 1);
        auto t0 = std::chrono::steady_clock::now();
        book.cancelOrder(order_id);
        auto t1 = std::chrono::steady_clock::now();

        auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        tracker.record(static_cast<std::uint64_t>(dur_ns));
    }

    auto total_end = std::chrono::steady_clock::now();
    auto total_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();

    s_bulk_cancel_stats = tracker.computeStats(static_cast<double>(total_elapsed_us));
    s_has_cancel = true;

    LatencyTracker::printStatsTable(s_bulk_cancel_stats);
    tracker.printHistogram(10, 36);
    printBookState(book);

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.orderCount(), 0);
    EXPECT_GT(s_bulk_cancel_stats.throughput_ops_sec, 0.0);
}

TEST_F(LatencyBenchmarkTest, MixedTradingWorkload)
{
    const std::size_t num_events = BENCHMARK_ORDERS_COUNT;
    const auto book_capacity = static_cast<std::size_t>(num_events * BENCHMARK_HEADROOM_FACTOR);

    std::cout << "\n===============================================================\n";
    std::cout << " BENCHMARK: MIXED HIGH-FREQUENCY SIMULATION (" << num_events << " events)\n";
    std::cout << " (Order Book Preallocated Capacity: " << book_capacity << " slots ["
              << BENCHMARK_HEADROOM_FACTOR << "x])\n";
    std::cout << " (50% Limit, 15% Reserve, 10% Stop, 15% Market, 5% Cancel, 5% Modify)\n";
    std::cout << "===============================================================\n";

    Book book(book_capacity);

    LatencyTracker tracker_limit("Mixed: New Limit Orders", num_events * 5 / 10);
    LatencyTracker tracker_reserve("Mixed: Reserve Orders", num_events * 15 / 100);
    LatencyTracker tracker_stop("Mixed: Stop Orders", num_events * 10 / 100);
    LatencyTracker tracker_cancel("Mixed: Cancellations", num_events * 5 / 100);
    LatencyTracker tracker_market("Mixed: Market Orders", num_events * 15 / 100);
    LatencyTracker tracker_modify("Mixed: Modifications", num_events * 5 / 100);
    LatencyTracker tracker_overall("Mixed: Overall Workload", num_events);

    enum class ActionType : std::uint8_t
    {
        NEW_LIMIT,
        NEW_RESERVE,
        NEW_STOP,
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
    std::uniform_int_distribution<OrderBookUtils::Price::rep> bid_dist(990'000, 1'000'000);
    std::uniform_int_distribution<OrderBookUtils::Price::rep> ask_dist(1'000'010, 1'010'000);
    std::uniform_int_distribution<OrderBookUtils::Quantity::rep> qty_dist(1, 100);
    std::uniform_int_distribution<OrderBookUtils::Quantity::rep> reserve_total_dist(100, 500);
    std::uniform_int_distribution<OrderBookUtils::Quantity::rep> reserve_disp_dist(20, 50);

    std::vector<OrderBookUtils::OrderID> active_ids;
    OrderBookUtils::OrderID next_id = 1;

    for (std::size_t i = 0; i < num_events; ++i)
    {
        int roll = action_dist(rng);

        if (roll <= 50 || active_ids.empty()) // 50% New Limit
        {
            bool is_buy = (roll % 2 == 0);
            OrderData order =
                OrderBuilder{}
                    .id(next_id++)
                    .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                    .limit(OrderBookUtils::Price(is_buy ? bid_dist(rng) : ask_dist(rng)),
                           OrderBookUtils::Quantity(qty_dist(rng)))
                    .build();
            active_ids.push_back(order.order_id);
            events.push_back(Event{.action = ActionType::NEW_LIMIT, .order = order});
        }
        else if (roll <= 65) // 15% New Reserve Order
        {
            bool is_buy = (roll % 2 == 0);
            auto tot = reserve_total_dist(rng);
            auto disp = reserve_disp_dist(rng);
            OrderData order =
                OrderBuilder{}
                    .id(next_id++)
                    .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                    .limit(OrderBookUtils::Price(is_buy ? bid_dist(rng) : ask_dist(rng)))
                    .reserve(OrderBookUtils::Quantity(tot), OrderBookUtils::Quantity(disp))
                    .build();
            active_ids.push_back(order.order_id);
            events.push_back(Event{.action = ActionType::NEW_RESERVE, .order = order});
        }
        else if (roll <= 75) // 10% New Stop Order
        {
            bool is_buy = (roll % 2 == 0);
            OrderBookUtils::Price::rep stop_p =
                is_buy ? (1'005'000 + (rng() % 5'000)) : (995'000 - (rng() % 5'000));
            OrderData order =
                OrderBuilder{}
                    .id(next_id++)
                    .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                    .stopMarket(OrderBookUtils::Price(stop_p),
                                OrderBookUtils::Quantity(qty_dist(rng)))
                    .build();
            active_ids.push_back(order.order_id);
            events.push_back(Event{.action = ActionType::NEW_STOP, .order = order});
        }
        else if (roll <= 90) // 15% Market Order
        {
            bool is_buy = (roll % 2 == 0);
            OrderData order =
                OrderBuilder{}
                    .id(next_id++)
                    .side(is_buy ? OrderBookUtils::OrderSide::BUY : OrderBookUtils::OrderSide::SELL)
                    .market(OrderBookUtils::Quantity((qty_dist(rng) / 2) + 1))
                    .build();
            events.push_back(Event{.action = ActionType::MARKET, .order = order});
        }
        else if (roll <= 95) // 5% Cancel
        {
            std::size_t idx = rng() % active_ids.size();
            OrderBookUtils::OrderID id_to_cancel = active_ids[idx];
            events.push_back(Event{.action = ActionType::CANCEL, .target_id = id_to_cancel});
        }
        else // 5% Modify
        {
            std::size_t idx = rng() % active_ids.size();
            OrderBookUtils::OrderID id_to_mod = active_ids[idx];
            events.push_back(Event{
                .action = ActionType::MODIFY,
                .target_id = id_to_mod,
                .new_price = OrderBookUtils::Price(bid_dist(rng)),
                .new_qty = OrderBookUtils::Quantity(qty_dist(rng)),
            });
        }
    }

    std::size_t total_trades = 0;

    auto total_start = std::chrono::steady_clock::now();

    for (const auto& ev : events)
    {
        auto t0 = std::chrono::steady_clock::now();

        switch (ev.action)
        {
        case ActionType::NEW_LIMIT:
        {
            auto trades = book.addOrder(ev.order);
            total_trades += trades.size();
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_limit.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        case ActionType::NEW_RESERVE:
        {
            auto trades = book.addOrder(ev.order);
            total_trades += trades.size();
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_reserve.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        case ActionType::NEW_STOP:
        {
            auto trades = book.addOrder(ev.order);
            total_trades += trades.size();
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_stop.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        case ActionType::MARKET:
        {
            auto trades = book.addOrder(ev.order);
            total_trades += trades.size();
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_market.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        case ActionType::CANCEL:
        {
            book.cancelOrder(ev.target_id);
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_cancel.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        case ActionType::MODIFY:
        {
            book.modifyOrder(ev.target_id, ev.new_price, ev.new_qty);
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            tracker_modify.record(static_cast<std::uint64_t>(dur));
            tracker_overall.record(static_cast<std::uint64_t>(dur));
            break;
        }
        }
    }

    auto total_end = std::chrono::steady_clock::now();
    auto total_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(total_end - total_start).count();

    auto limit_stats = tracker_limit.computeStats();
    auto reserve_stats = tracker_reserve.computeStats();
    auto stop_stats = tracker_stop.computeStats();
    auto cancel_stats = tracker_cancel.computeStats();
    auto market_stats = tracker_market.computeStats();
    auto modify_stats = tracker_modify.computeStats();
    auto overall_stats = tracker_overall.computeStats(static_cast<double>(total_elapsed_us));

    s_mixed_results = MixedBenchmarkResults{
        .limit_stats = limit_stats,
        .reserve_stats = reserve_stats,
        .stop_stats = stop_stats,
        .cancel_stats = cancel_stats,
        .market_stats = market_stats,
        .modify_stats = modify_stats,
        .overall_stats = overall_stats,
    };
    s_has_mixed = true;

    std::cout << "Total Events Processed: " << num_events << "\n";
    std::cout << "Total Matches Executed: " << total_trades << "\n";

    std::cout << "\n  "
                 "================================================================================="
                 "=======================\n";
    std::cout << "   MIXED SCENARIO LATENCY BREAKDOWN BY ACTION TYPE (NANOSECONDS)\n";
    std::cout << "  "
                 "================================================================================="
                 "=======================\n";
    std::cout << "  " << std::left << std::setw(16) << "Action" << " | " << std::right
              << std::setw(9) << "Count" << " | " << std::setw(8) << "Mean" << " | " << std::setw(8)
              << "P50" << " | " << std::setw(8) << "P90" << " | " << std::setw(8) << "P95" << " | "
              << std::setw(8) << "P99" << " | " << std::setw(8) << "P99.9"
              << " | " << std::setw(9) << "Max" << " | " << std::setw(12) << "Throughput\n";
    std::cout << "  " << std::string(104, '-') << "\n";

    auto printRow = [](std::string_view label, const LatencyStats& s) {
        std::cout << "  " << std::left << std::setw(16) << label << " | " << std::right
                  << std::setw(9) << s.count << " | " << std::setw(8) << std::fixed
                  << std::setprecision(1) << s.mean_ns << " | " << std::setw(8) << s.p50_ns << " | "
                  << std::setw(8) << s.p90_ns << " | " << std::setw(8) << s.p95_ns << " | "
                  << std::setw(8) << s.p99_ns << " | " << std::setw(8) << s.p99_9_ns << " | "
                  << std::setw(9) << s.max_ns << " | " << std::setw(8) << std::fixed
                  << std::setprecision(0) << s.throughput_ops_sec << " op/s\n";
    };

    printRow("Limit Add (50%)", limit_stats);
    printRow("Reserve   (15%)", reserve_stats);
    printRow("Stop Add  (10%)", stop_stats);
    printRow("Market    (15%)", market_stats);
    printRow("Cancel     (5%)", cancel_stats);
    printRow("Modify     (5%)", modify_stats);
    std::cout << "  " << std::string(104, '-') << "\n";
    printRow("OVERALL   (100%)", overall_stats);
    std::cout << "  "
                 "================================================================================="
                 "=======================\n";

    tracker_overall.printHistogram(10, 36);
    tracker_limit.printHistogram(8, 30);
    tracker_reserve.printHistogram(8, 30);
    tracker_stop.printHistogram(8, 30);
    tracker_market.printHistogram(8, 30);

    printBookState(book);

    EXPECT_GT(overall_stats.throughput_ops_sec, 0.0);
}

} // namespace
