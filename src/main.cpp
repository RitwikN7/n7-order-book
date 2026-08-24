#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace MatchingEngine;

constexpr double PRICE_SCALE = 10'000.0; // 10^4 fixed-point scaling factor (4 decimal places)

// ============================================================================
// 1. LIGHTWEIGHT TEST FRAMEWORK WITH ASSERTIONS
// ============================================================================

class TestRunner
{
public:
    static TestRunner& instance()
    {
        static TestRunner runner;
        return runner;
    }

    void startSuite(std::string_view suite_name)
    {
        current_suite_ = suite_name;
        suite_passed_ = true;
        total_suites_++;
        std::cout << "\n[TEST SUITE " << total_suites_ << "] " << suite_name << "\n";
        std::cout << std::string(60, '-') << "\n";
    }

    void recordCheck(bool passed, std::string_view expr, std::string_view file, int line,
                     std::string_view message = "")
    {
        total_checks_++;
        if (passed)
        {
            passed_checks_++;
            std::cout << "  [PASS] " << expr;
            if (!message.empty())
                std::cout << " (" << message << ")";
            std::cout << "\n";
        }
        else
        {
            failed_checks_++;
            suite_passed_ = false;
            std::cerr << "  [FAIL] " << expr << " at " << file << ":" << line;
            if (!message.empty())
                std::cerr << " -- " << message;
            std::cerr << "\n";
        }
    }

    void endSuite()
    {
        if (suite_passed_)
        {
            passed_suites_++;
            std::cout << "  --> Suite Passed.\n";
        }
        else
        {
            std::cout << "  --> Suite FAILED.\n";
        }
    }

    void printSummary() const
    {
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << " FUNCTIONAL TEST SUMMARY\n";
        std::cout << std::string(60, '=') << "\n";
        std::cout << " Suites: " << passed_suites_ << " / " << total_suites_ << " passed\n";
        std::cout << " Checks: " << passed_checks_ << " / " << total_checks_ << " passed ("
                  << failed_checks_ << " failed)\n";
        if (failed_checks_ == 0)
        {
            std::cout << " Status: ALL FUNCTIONAL TESTS PASSED SUCCESSFULLY!\n";
        }
        else
        {
            std::cout << " Status: TESTS FAILED!\n";
        }
        std::cout << std::string(60, '=') << "\n\n";
    }

    [[nodiscard]] bool allPassed() const noexcept
    {
        return failed_checks_ == 0;
    }

private:
    std::string current_suite_;
    bool suite_passed_{true};
    std::size_t total_suites_{0};
    std::size_t passed_suites_{0};
    std::size_t total_checks_{0};
    std::size_t passed_checks_{0};
    std::size_t failed_checks_{0};
};

#define TEST_ASSERT(cond) TestRunner::instance().recordCheck((cond), #cond, __FILE__, __LINE__)

#define TEST_ASSERT_MSG(cond, msg)                                                                 \
    TestRunner::instance().recordCheck((cond), #cond, __FILE__, __LINE__, (msg))

#define TEST_ASSERT_EQ(a, b)                                                                       \
    TestRunner::instance().recordCheck(((a) == (b)), #a " == " #b, __FILE__, __LINE__)

#define TEST_ASSERT_FALSE(cond)                                                                    \
    TestRunner::instance().recordCheck(!(cond), "!(" #cond ")", __FILE__, __LINE__)

// ============================================================================
// 2. HIGH-RESOLUTION LATENCY TRACKING & STATISTICAL ANALYSIS
// ============================================================================

struct LatencyStats
{
    std::string name;
    std::size_t count{0};
    double min_ns{0.0};
    double max_ns{0.0};
    double mean_ns{0.0};
    double stddev_ns{0.0};
    double p50_ns{0.0};
    double p75_ns{0.0};
    double p90_ns{0.0};
    double p95_ns{0.0};
    double p99_ns{0.0};
    double p99_9_ns{0.0};
    double p99_99_ns{0.0};
    double throughput_ops_sec{0.0};
    double total_time_ms{0.0};
};

class LatencyTracker
{
public:
    explicit LatencyTracker(std::string name, std::size_t reserve_capacity = 0)
        : name_(std::move(name))
    {
        if (reserve_capacity > 0)
        {
            samples_.reserve(reserve_capacity);
        }
    }

    void reserve(std::size_t capacity)
    {
        samples_.reserve(capacity);
    }

    void record(std::uint64_t duration_ns)
    {
        samples_.push_back(duration_ns);
    }

    void clear()
    {
        samples_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return samples_.size();
    }

    [[nodiscard]] const std::vector<std::uint64_t>& samples() const noexcept
    {
        return samples_;
    }

    [[nodiscard]] LatencyStats computeStats(double total_duration_us = 0.0)
    {
        if (samples_.empty())
        {
            return LatencyStats{.name = name_};
        }

        std::sort(samples_.begin(), samples_.end());

        const std::size_t n = samples_.size();
        const double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        const double mean = sum / static_cast<double>(n);

        double sq_sum = 0.0;
        for (auto val : samples_)
        {
            double diff = static_cast<double>(val) - mean;
            sq_sum += diff * diff;
        }
        const double stddev = std::sqrt(sq_sum / static_cast<double>(n));

        auto getPercentile = [&](double pct) -> double {
            if (n == 0)
                return 0.0;
            std::size_t idx = static_cast<std::size_t>((pct / 100.0) * static_cast<double>(n - 1));
            return static_cast<double>(samples_[idx]);
        };

        double duration_sec = 0.0;
        if (total_duration_us > 0.0)
        {
            duration_sec = total_duration_us / 1e6;
        }
        else
        {
            duration_sec = sum / 1e9;
        }

        double throughput = (duration_sec > 0.0) ? (static_cast<double>(n) / duration_sec) : 0.0;

        return LatencyStats{
            .name = name_,
            .count = n,
            .min_ns = static_cast<double>(samples_.front()),
            .max_ns = static_cast<double>(samples_.back()),
            .mean_ns = mean,
            .stddev_ns = stddev,
            .p50_ns = getPercentile(50.0),
            .p75_ns = getPercentile(75.0),
            .p90_ns = getPercentile(90.0),
            .p95_ns = getPercentile(95.0),
            .p99_ns = getPercentile(99.0),
            .p99_9_ns = getPercentile(99.9),
            .p99_99_ns = getPercentile(99.99),
            .throughput_ops_sec = throughput,
            .total_time_ms = duration_sec * 1000.0,
        };
    }

    static void printStatsTable(const LatencyStats& s)
    {
        std::cout << "  --------------------------------------------------------\n";
        std::cout << "  Latency Metrics (" << s.name << " - " << s.count << " samples):\n";
        std::cout << "  --------------------------------------------------------\n";
        std::cout << "  Total Time:    " << std::fixed << std::setprecision(3) << s.total_time_ms
                  << " ms\n";
        std::cout << "  Throughput:    " << std::fixed << std::setprecision(0)
                  << s.throughput_ops_sec << " ops/sec\n";
        std::cout << "  Min Latency:   " << std::fixed << std::setprecision(1) << s.min_ns
                  << " ns\n";
        std::cout << "  Mean Latency:  " << std::fixed << std::setprecision(2) << s.mean_ns
                  << " ns (" << s.mean_ns / 1000.0 << " us)\n";
        std::cout << "  Std Deviation: " << std::fixed << std::setprecision(2) << s.stddev_ns
                  << " ns\n";
        std::cout << "  Median (P50):  " << std::fixed << std::setprecision(1) << s.p50_ns
                  << " ns\n";
        std::cout << "  P75 Latency:   " << std::fixed << std::setprecision(1) << s.p75_ns
                  << " ns\n";
        std::cout << "  P90 Latency:   " << std::fixed << std::setprecision(1) << s.p90_ns
                  << " ns\n";
        std::cout << "  P95 Latency:   " << std::fixed << std::setprecision(1) << s.p95_ns
                  << " ns\n";
        std::cout << "  P99 Latency:   " << std::fixed << std::setprecision(1) << s.p99_ns
                  << " ns (" << s.p99_ns / 1000.0 << " us)\n";
        std::cout << "  P99.9 Latency: " << std::fixed << std::setprecision(1) << s.p99_9_ns
                  << " ns (" << s.p99_9_ns / 1000.0 << " us)\n";
        std::cout << "  P99.99 Latency:" << std::fixed << std::setprecision(1) << s.p99_99_ns
                  << " ns (" << s.p99_99_ns / 1000.0 << " us)\n";
        std::cout << "  Max Latency:   " << std::fixed << std::setprecision(1) << s.max_ns
                  << " ns (" << s.max_ns / 1000.0 << " us)\n";
        std::cout << "  --------------------------------------------------------\n";
    }

    void printHistogram(std::size_t num_buckets = 10, std::size_t bar_width = 36) const
    {
        if (samples_.empty())
            return;

        // Use P99.9 as effective max for histogram buckets to avoid tail outlier skewing
        const std::size_t n = samples_.size();
        std::size_t p999_idx = static_cast<std::size_t>(0.999 * static_cast<double>(n - 1));
        std::uint64_t hist_min = samples_.front();
        std::uint64_t hist_max = std::max(samples_[p999_idx], hist_min + 10);

        std::uint64_t bucket_width = (hist_max - hist_min) / num_buckets;
        if (bucket_width == 0)
            bucket_width = 1;

        std::vector<std::size_t> bucket_counts(num_buckets + 1, 0);

        for (auto val : samples_)
        {
            if (val >= hist_max)
            {
                bucket_counts[num_buckets]++;
            }
            else
            {
                std::size_t b = static_cast<std::size_t>((val - hist_min) / bucket_width);
                if (b >= num_buckets)
                    b = num_buckets - 1;
                bucket_counts[b]++;
            }
        }

        std::size_t max_count = 0;
        for (auto c : bucket_counts)
        {
            max_count = std::max(c, max_count);
        }

        std::cout << "\n  --- Latency Distribution Histogram (" << name_ << ") ---\n";
        std::cout << "  " << std::setw(18) << "Latency Range (ns)" << " | " << std::setw(8)
                  << "Count" << " | " << std::setw(6) << "%" << " | " << std::setw(6) << "Cumul%"
                  << " | Graph\n";
        std::cout << "  " << std::string(75, '-') << "\n";

        double cumulative_pct = 0.0;
        for (std::size_t i = 0; i < num_buckets; ++i)
        {
            std::uint64_t low = hist_min + (i * bucket_width);
            std::uint64_t high = low + bucket_width;
            double pct = (static_cast<double>(bucket_counts[i]) / static_cast<double>(n)) * 100.0;
            cumulative_pct += pct;

            std::size_t bar_len =
                (max_count > 0)
                    ? static_cast<std::size_t>((static_cast<double>(bucket_counts[i]) / max_count) *
                                               bar_width)
                    : 0;
            std::string bar(bar_len, '#');

            std::ostringstream range_ss;
            range_ss << "[" << low << ", " << high << ")";

            std::cout << "  " << std::setw(18) << range_ss.str() << " | " << std::setw(8)
                      << bucket_counts[i] << " | " << std::setw(5) << std::fixed
                      << std::setprecision(1) << pct << "% | " << std::setw(5)
                      << std::setprecision(1) << cumulative_pct << "% | " << bar << "\n";
        }

        if (bucket_counts[num_buckets] > 0)
        {
            double pct =
                (static_cast<double>(bucket_counts[num_buckets]) / static_cast<double>(n)) * 100.0;
            cumulative_pct += pct;
            std::size_t bar_len =
                (max_count > 0)
                    ? static_cast<std::size_t>(
                          (static_cast<double>(bucket_counts[num_buckets]) / max_count) * bar_width)
                    : 0;
            std::string bar(bar_len, '#');

            std::ostringstream range_ss;
            range_ss << "[>=" << hist_max << " (Tail)]";

            std::cout << "  " << std::setw(18) << range_ss.str() << " | " << std::setw(8)
                      << bucket_counts[num_buckets] << " | " << std::setw(5) << std::fixed
                      << std::setprecision(1) << pct << "% | " << std::setw(5)
                      << std::setprecision(1) << cumulative_pct << "% | " << bar << "\n";
        }
        std::cout << "  " << std::string(75, '-') << "\n\n";
    }

private:
    std::string name_;
    std::vector<std::uint64_t> samples_;
};

// ============================================================================
// 3. BOOK STATE PRINT HELPER
// ============================================================================

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

    std::cout << "------------------------\n";
}

// ============================================================================
// 4. COMPREHENSIVE FUNCTIONAL TEST SUITES
// ============================================================================

void testSuite1_BasicLimitOrdersAndBookDepth()
{
    TestRunner::instance().startSuite("1. Basic Limit Orders & Book State");

    Book book(100);

    TEST_ASSERT_MSG(book.empty(), "Book starts empty");
    TEST_ASSERT_EQ(book.orderCount(), 0);
    TEST_ASSERT_FALSE(book.bestBid().has_value());
    TEST_ASSERT_FALSE(book.bestAsk().has_value());
    TEST_ASSERT_FALSE(book.spread().has_value());

    // Insert Bids: $100.00 (id 101, qty 50), $99.50 (id 102, qty 100)
    auto t1 = book.addOrder(OrderData{
        .order_id = 101,
        .price = 1'000'000,
        .quantity = 50,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    TEST_ASSERT(t1.empty());
    TEST_ASSERT_EQ(book.orderCount(), 1);
    TEST_ASSERT_EQ(book.bestBid().value(), 1'000'000);

    auto t2 = book.addOrder(OrderData{
        .order_id = 102,
        .price = 995'000,
        .quantity = 100,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    TEST_ASSERT(t2.empty());
    TEST_ASSERT_EQ(book.orderCount(), 2);
    TEST_ASSERT_EQ(book.bestBid().value(), 1'000'000);

    // Insert Asks: $100.50 (id 201, qty 30), $101.00 (id 202, qty 70)
    auto t3 = book.addOrder(OrderData{
        .order_id = 201,
        .price = 1'005'000,
        .quantity = 30,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    TEST_ASSERT(t3.empty());
    TEST_ASSERT_EQ(book.bestAsk().value(), 1'005'000);

    auto t4 = book.addOrder(OrderData{
        .order_id = 202,
        .price = 1'010'000,
        .quantity = 70,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    TEST_ASSERT(t4.empty());
    TEST_ASSERT_EQ(book.orderCount(), 4);
    TEST_ASSERT_EQ(book.bestAsk().value(), 1'005'000);

    // Check spread: $100.50 - $100.00 = $0.50 (5000 ticks)
    TEST_ASSERT_EQ(book.spread().value(), 5'000);

    // Verify order lookup
    const auto* node101 = book.findOrder(101);
    TEST_ASSERT(node101 != nullptr);
    TEST_ASSERT_EQ(node101->price, 1'000'000);
    TEST_ASSERT_EQ(node101->quantity, 50);

    TestRunner::instance().endSuite();
}

void testSuite2_FifoPriceTimePriority()
{
    TestRunner::instance().startSuite("2. Price-Time (FIFO) Priority Execution");

    Book book(100);

    // Insert 3 orders at same price $100.00 (1'000'000 ticks):
    // Order 1: qty 50
    // Order 2: qty 30
    // Order 3: qty 20
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'000'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    TEST_ASSERT_EQ(book.orderCount(), 3);

    // Send aggressive market sell order of qty 60.
    // Should fully fill Order 1 (50 units) and partially fill Order 2 (10 units).
    // Order 3 should remain untouched.
    auto trades = book.addOrder(OrderData{.order_id = 99,
                                          .price = 0,
                                          .quantity = 60,
                                          .side = OrderBookUtils::OrderSide::SELL,
                                          .type = OrderBookUtils::OrderType::MARKET});

    TEST_ASSERT_EQ(trades.size(), 2);
    TEST_ASSERT_EQ(trades[0].maker_order_id, 1);
    TEST_ASSERT_EQ(trades[0].quantity, 50);
    TEST_ASSERT_EQ(trades[1].maker_order_id, 2);
    TEST_ASSERT_EQ(trades[1].quantity, 10);

    // Order 1 should be gone from the book
    TEST_ASSERT(book.findOrder(1) == nullptr);

    // Order 2 should have 20 remaining
    const auto* node2 = book.findOrder(2);
    TEST_ASSERT(node2 != nullptr);
    TEST_ASSERT_EQ(node2->quantity, 20);

    // Order 3 should have 20 remaining
    const auto* node3 = book.findOrder(3);
    TEST_ASSERT(node3 != nullptr);
    TEST_ASSERT_EQ(node3->quantity, 20);

    TEST_ASSERT_EQ(book.orderCount(), 2);

    TestRunner::instance().endSuite();
}

void testSuite3_MarketOrdersAndMultiLevelSweeps()
{
    TestRunner::instance().startSuite("3. Market Orders & Multi-Level Sweeps");

    Book book(100);

    // Set up ask ladder:
    // Ask 1: $101.00 (id 1, qty 20)
    // Ask 2: $102.00 (id 2, qty 30)
    // Ask 3: $103.00 (id 3, qty 50)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'010'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'020'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'030'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Market Buy of 65:
    // - Fills 20 @ $101 (Ask 1 removed)
    // - Fills 30 @ $102 (Ask 2 removed)
    // - Fills 15 @ $103 (Ask 3 has 35 left)
    auto trades = book.addOrder(OrderData{.order_id = 10,
                                          .price = 0,
                                          .quantity = 65,
                                          .side = OrderBookUtils::OrderSide::BUY,
                                          .type = OrderBookUtils::OrderType::MARKET});

    TEST_ASSERT_EQ(trades.size(), 3);
    TEST_ASSERT_EQ(trades[0].price, 1'010'000);
    TEST_ASSERT_EQ(trades[0].quantity, 20);
    TEST_ASSERT_EQ(trades[1].price, 1'020'000);
    TEST_ASSERT_EQ(trades[1].quantity, 30);
    TEST_ASSERT_EQ(trades[2].price, 1'030'000);
    TEST_ASSERT_EQ(trades[2].quantity, 15);

    // Book state verification: only Ask 3 remains with qty 35
    TEST_ASSERT_EQ(book.orderCount(), 1);
    TEST_ASSERT_EQ(book.bestAsk().value(), 1'030'000);
    const auto* node3 = book.findOrder(3);
    TEST_ASSERT(node3 != nullptr);
    TEST_ASSERT_EQ(node3->quantity, 35);

    // Sweep remaining 35 with market sell-out
    auto trades2 = book.addOrder(OrderData{.order_id = 11,
                                           .price = 0,
                                           .quantity = 100,
                                           .side = OrderBookUtils::OrderSide::BUY,
                                           .type = OrderBookUtils::OrderType::MARKET});
    TEST_ASSERT_EQ(trades2.size(), 1);
    TEST_ASSERT_EQ(trades2[0].quantity, 35);
    TEST_ASSERT_MSG(book.empty(), "Book is empty after full liquidity sweep");

    TestRunner::instance().endSuite();
}

void testSuite4_AggressiveLimitCrossingSpread()
{
    TestRunner::instance().startSuite("4. Aggressive Crossing Limit Orders");

    Book book(100);

    // Resting Ask: $100.00 (id 1, qty 40)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 40,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Incoming Limit Buy: $102.00 (id 2, qty 60)
    // Crosses ask at $100.00:
    // - Should match 40 @ $100.00 (maker price)
    // - Remaining 20 units should rest on bid side at $102.00
    auto trades = book.addOrder(OrderData{.order_id = 2,
                                          .price = 1'020'000,
                                          .quantity = 60,
                                          .side = OrderBookUtils::OrderSide::BUY,
                                          .type = OrderBookUtils::OrderType::LIMIT});

    TEST_ASSERT_EQ(trades.size(), 1);
    TEST_ASSERT_EQ(trades[0].price, 1'000'000);
    TEST_ASSERT_EQ(trades[0].quantity, 40);

    TEST_ASSERT_EQ(book.orderCount(), 1);
    TEST_ASSERT_EQ(book.bestBid().value(), 1'020'000);
    TEST_ASSERT_FALSE(book.bestAsk().has_value());

    const auto* node2 = book.findOrder(2);
    TEST_ASSERT(node2 != nullptr);
    TEST_ASSERT_EQ(node2->quantity, 20);

    TestRunner::instance().endSuite();
}

void testSuite5_OrderModifications()
{
    TestRunner::instance().startSuite("5. Order Modifications (In-place & Re-queue)");

    Book book(100);

    // Insert 2 Buy orders at $100.00:
    // Order 1: qty 50
    // Order 2: qty 50
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Modify Order 1 quantity DOWN to 30 (same price)
    // Should preserve FIFO priority
    bool mod1_ok = book.modifyOrder(1, 30);
    TEST_ASSERT(mod1_ok);
    const auto* n1 = book.findOrder(1);
    TEST_ASSERT(n1 != nullptr);
    TEST_ASSERT_EQ(n1->quantity, 30);

    // Verify priority: aggressive sell of 30 fills Order 1 completely first
    auto trades = book.addOrder(OrderData{.order_id = 99,
                                          .price = 0,
                                          .quantity = 30,
                                          .side = OrderBookUtils::OrderSide::SELL,
                                          .type = OrderBookUtils::OrderType::MARKET});
    TEST_ASSERT_EQ(trades.size(), 1);
    TEST_ASSERT_EQ(trades[0].maker_order_id, 1);
    TEST_ASSERT_EQ(trades[0].quantity, 30);
    TEST_ASSERT(book.findOrder(1) == nullptr);

    // Modify Order 2 price from $100.00 to $105.00
    bool mod2_ok = book.modifyOrder(2, 1'050'000, 50);
    TEST_ASSERT(mod2_ok);
    TEST_ASSERT_EQ(book.bestBid().value(), 1'050'000);

    // Modify Order 2 to qty 0 -> should cancel
    bool mod_zero = book.modifyOrder(2, 0);
    TEST_ASSERT(mod_zero);
    TEST_ASSERT_MSG(book.empty(), "Modifying to quantity 0 cancels the order");

    TestRunner::instance().endSuite();
}

void testSuite6_OrderCancellations()
{
    TestRunner::instance().startSuite("6. Order Cancellations & Level Cleanups");

    Book book(100);

    // Add 3 orders at $100.00 (ids: 1, 2, 3)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 10,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'000'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Add 1 order at $95.00 (id: 4)
    book.addOrder(OrderData{.order_id = 4,
                            .price = 950'000,
                            .quantity = 40,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    TEST_ASSERT_EQ(book.orderCount(), 4);
    TEST_ASSERT_EQ(book.bestBid().value(), 1'000'000);

    // Cancel middle order (id: 2)
    TEST_ASSERT(book.cancelOrder(2));
    TEST_ASSERT_EQ(book.orderCount(), 3);
    TEST_ASSERT(book.findOrder(2) == nullptr);

    // Cancel head order (id: 1)
    TEST_ASSERT(book.cancelOrder(1));
    TEST_ASSERT_EQ(book.orderCount(), 2);
    TEST_ASSERT(book.findOrder(1) == nullptr);

    // Cancel last order at $100.00 (id: 3) -> best bid should shift to $95.00
    TEST_ASSERT(book.cancelOrder(3));
    TEST_ASSERT_EQ(book.orderCount(), 1);
    TEST_ASSERT_EQ(book.bestBid().value(), 950'000);

    // Cancel id: 4 -> book empty
    TEST_ASSERT(book.cancelOrder(4));
    TEST_ASSERT_MSG(book.empty(), "Book is empty after cancelling all orders");

    TestRunner::instance().endSuite();
}

void testSuite7_EdgeCasesAndInputValidation()
{
    TestRunner::instance().startSuite("7. Edge Cases & Robustness");

    Book book(100);

    // Cancel non-existent order
    TEST_ASSERT_FALSE(book.cancelOrder(9999));

    // Modify non-existent order
    TEST_ASSERT_FALSE(book.modifyOrder(9999, 100));
    TEST_ASSERT_FALSE(book.modifyOrder(9999, 1'000'000, 100));

    // Add zero-quantity order
    auto zero_trades = book.addOrder(OrderData{.order_id = 1,
                                               .price = 1'000'000,
                                               .quantity = 0,
                                               .side = OrderBookUtils::OrderSide::BUY,
                                               .type = OrderBookUtils::OrderType::LIMIT});
    TEST_ASSERT(zero_trades.empty());
    TEST_ASSERT(book.empty());

    // Market order on completely empty book
    auto mkt_empty = book.addOrder(OrderData{.order_id = 2,
                                             .price = 0,
                                             .quantity = 100,
                                             .side = OrderBookUtils::OrderSide::BUY,
                                             .type = OrderBookUtils::OrderType::MARKET});
    TEST_ASSERT(mkt_empty.empty());
    TEST_ASSERT(book.empty());

    // Find non-existent order
    TEST_ASSERT(book.findOrder(9999) == nullptr);

    TestRunner::instance().endSuite();
}

void runAllFunctionalTests()
{
    std::cout << "===============================================================\n";
    std::cout << " 1. COMPREHENSIVE FUNCTIONAL TEST SUITE\n";
    std::cout << "===============================================================\n";

    testSuite1_BasicLimitOrdersAndBookDepth();
    testSuite2_FifoPriceTimePriority();
    testSuite3_MarketOrdersAndMultiLevelSweeps();
    testSuite4_AggressiveLimitCrossingSpread();
    testSuite5_OrderModifications();
    testSuite6_OrderCancellations();
    testSuite7_EdgeCasesAndInputValidation();

    TestRunner::instance().printSummary();
}

// ============================================================================
// 5. BENCHMARKS WITH HIGH-RESOLUTION LATENCY PROFILING
// ============================================================================

LatencyStats runBenchmarkLimitInsertions(std::size_t num_orders)
{
    std::cout << "===============================================================\n";
    std::cout << " 2. BENCHMARK: BULK LIMIT INSERTIONS (" << num_orders << " orders)\n";
    std::cout << "===============================================================\n";

    Book book(num_orders + 10'000);
    LatencyTracker tracker("Bulk Limit Insertions", num_orders);

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

    auto stats = tracker.computeStats(static_cast<double>(total_elapsed_us));
    LatencyTracker::printStatsTable(stats);
    tracker.printHistogram(10, 36);
    printBookState(book);

    return stats;
}

LatencyStats runBenchmarkCancellations(std::size_t num_orders)
{
    std::cout << "\n===============================================================\n";
    std::cout << " 3. BENCHMARK: O(1) CANCELLATIONS (" << num_orders << " cancels)\n";
    std::cout << "===============================================================\n";

    Book book(num_orders + 10'000);
    LatencyTracker tracker("Bulk Cancellations", num_orders);

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

    auto stats = tracker.computeStats(static_cast<double>(total_elapsed_us));
    LatencyTracker::printStatsTable(stats);
    tracker.printHistogram(10, 36);
    printBookState(book);

    return stats;
}

struct MixedBenchmarkResults
{
    LatencyStats limit_stats;
    LatencyStats cancel_stats;
    LatencyStats market_stats;
    LatencyStats modify_stats;
    LatencyStats overall_stats;
};

MixedBenchmarkResults runBenchmarkRealisticTradingWorkload(std::size_t num_events)
{
    std::cout << "\n===============================================================\n";
    std::cout << " 4. BENCHMARK: MIXED HIGH-FREQUENCY SIMULATION (" << num_events << " events)\n";
    std::cout << "    (60% Limit Orders, 20% Cancels, 15% Market Orders, 5% Modifies)\n";
    std::cout << "===============================================================\n";

    Book book(num_events + 50'000);

    LatencyTracker tracker_limit("Mixed: New Limit Orders", num_events * 6 / 10);
    LatencyTracker tracker_cancel("Mixed: Cancellations", num_events * 2 / 10);
    LatencyTracker tracker_market("Mixed: Market Orders", num_events * 15 / 100);
    LatencyTracker tracker_modify("Mixed: Modifications", num_events * 5 / 100);
    LatencyTracker tracker_overall("Mixed: Overall Workload", num_events);

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
    auto cancel_stats = tracker_cancel.computeStats();
    auto market_stats = tracker_market.computeStats();
    auto modify_stats = tracker_modify.computeStats();
    auto overall_stats = tracker_overall.computeStats(static_cast<double>(total_elapsed_us));

    std::cout << "Total Events Processed: " << num_events << "\n";
    std::cout << "Total Matches Executed: " << total_trades << "\n";

    // Detailed per-action table
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

    printRow("Limit Add (60%)", limit_stats);
    printRow("Cancel    (20%)", cancel_stats);
    printRow("Market    (15%)", market_stats);
    printRow("Modify     (5%)", modify_stats);
    std::cout << "  " << std::string(104, '-') << "\n";
    printRow("OVERALL   (100%)", overall_stats);
    std::cout << "  "
                 "================================================================================="
                 "=======================\n";

    // Histograms
    tracker_overall.printHistogram(10, 36);
    tracker_limit.printHistogram(8, 30);
    tracker_market.printHistogram(8, 30);

    printBookState(book);

    return MixedBenchmarkResults{
        .limit_stats = limit_stats,
        .cancel_stats = cancel_stats,
        .market_stats = market_stats,
        .modify_stats = modify_stats,
        .overall_stats = overall_stats,
    };
}

// ============================================================================
// 6. CSV LATENCY DATA EXPORT FOR PLOTTING & ANALYSIS
// ============================================================================

void exportLatencyMetricsToCsv(const std::string& filepath, const LatencyStats& bulk_insert,
                               const LatencyStats& bulk_cancel, const MixedBenchmarkResults& mixed)
{
    std::filesystem::path p(filepath);
    if (p.has_parent_path())
    {
        std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream out(filepath);
    if (!out.is_open())
    {
        std::cerr << "Warning: Could not open " << filepath << " for latency CSV export.\n";
        return;
    }

    out << "scenario,count,min_ns,mean_ns,stddev_ns,p50_ns,p75_ns,p90_ns,p95_ns,p99_ns,p99_9_ns,"
           "p99_99_ns,max_ns,throughput_ops_sec\n";

    auto writeRow = [&](const std::string& label, const LatencyStats& s) {
        out << label << "," << s.count << "," << s.min_ns << "," << s.mean_ns << "," << s.stddev_ns
            << "," << s.p50_ns << "," << s.p75_ns << "," << s.p90_ns << "," << s.p95_ns << ","
            << s.p99_ns << "," << s.p99_9_ns << "," << s.p99_99_ns << "," << s.max_ns << ","
            << s.throughput_ops_sec << "\n";
    };

    writeRow("Bulk Limit Insertions", bulk_insert);
    writeRow("Bulk Cancellations", bulk_cancel);
    writeRow("Mixed: Limit Add", mixed.limit_stats);
    writeRow("Mixed: Cancel", mixed.cancel_stats);
    writeRow("Mixed: Market Order", mixed.market_stats);
    writeRow("Mixed: Modify", mixed.modify_stats);
    writeRow("Mixed: Overall Workload", mixed.overall_stats);

    std::cout << "--> Exported benchmark latency metrics to: " << filepath << "\n";
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main()
{
    std::cout << "===============================================================\n";
    std::cout << "      N7 LOW-LATENCY ORDER BOOK BENCHMARK & TEST SUITE         \n";
    std::cout << "===============================================================\n";

    // 1. Comprehensive Functional Test Suite
    runAllFunctionalTests();

    // 2. High-Resolution Benchmarks Across All Scenarios
    auto bulk_insert_stats = runBenchmarkLimitInsertions(1'000'000);
    auto bulk_cancel_stats = runBenchmarkCancellations(1'000'000);
    auto mixed_results = runBenchmarkRealisticTradingWorkload(1'000'000);

    // 3. Export CSV Data for Graph Generation to data/ folder
    exportLatencyMetricsToCsv("data/latency_metrics.csv", bulk_insert_stats, bulk_cancel_stats,
                              mixed_results);

    std::cout << "\nAll benchmarks and tests completed successfully.\n";
    return 0;
}
