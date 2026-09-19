#pragma once

#include "order-book/book.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OrderBookBenchmark
{

constexpr double PRICE_SCALE = 10'000.0; // 10^4 fixed-point scaling factor (4 decimal places)

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

inline void printBookState(const MatchingEngine::Book& book)
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

struct MixedBenchmarkResults
{
    LatencyStats limit_stats;
    LatencyStats cancel_stats;
    LatencyStats market_stats;
    LatencyStats modify_stats;
    LatencyStats overall_stats;
};

inline void exportLatencyMetricsToCsv(const std::string& filepath, const LatencyStats& bulk_insert,
                                      const LatencyStats& bulk_cancel,
                                      const MixedBenchmarkResults& mixed)
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

} // namespace OrderBookBenchmark
