// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// A compact HdrHistogram: constant relative error across a wide dynamic
// range, O(1) record, no allocation after construction. Values are
// nanoseconds. See docs/adr/0007-latency-measurement.md.
//
// Layout follows the canonical HdrHistogram sub-bucket scheme with
// unit_magnitude fixed at 0.
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::uint64_t highest = 60'000'000'000ULL,
                              int significant_figures = 3) {
        const double target = 2.0 * std::pow(10.0, significant_figures);
        sub_half_mag_ =
            static_cast<int>(std::ceil(std::log2(target))) - 1;
        if (sub_half_mag_ < 1) sub_half_mag_ = 1;
        sub_half_count_ = 1ULL << sub_half_mag_;
        sub_count_ = sub_half_count_ << 1;
        sub_mask_ = sub_count_ - 1;
        lzc_base_ = 64 - (sub_half_mag_ + 1);

        std::uint64_t smallest_untrackable = sub_count_;
        bucket_count_ = 1;
        while (smallest_untrackable < highest) {
            smallest_untrackable <<= 1;
            ++bucket_count_;
        }
        counts_.assign(
            static_cast<std::size_t>(bucket_count_ + 1) * sub_half_count_, 0);
    }

    void record(std::uint64_t value) noexcept {
        const std::size_t idx = counts_index(value);
        if (idx >= counts_.size()) {
            ++counts_.back();
        } else {
            ++counts_[idx];
        }
        ++total_;
        sum_ += value;
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
    }

    void merge(const LatencyHistogram& other) {
        const std::size_t n = std::min(counts_.size(), other.counts_.size());
        for (std::size_t i = 0; i < n; ++i) counts_[i] += other.counts_[i];
        total_ += other.total_;
        sum_ += other.sum_;
        if (other.min_ < min_) min_ = other.min_;
        if (other.max_ > max_) max_ = other.max_;
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_ = 0;
        sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t min() const noexcept {
        return total_ == 0 ? 0 : min_;
    }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ == 0 ? 0.0
                           : static_cast<double>(sum_) / static_cast<double>(total_);
    }

    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        double want = (p / 100.0) * static_cast<double>(total_);
        std::uint64_t target = static_cast<std::uint64_t>(std::ceil(want));
        if (target == 0) target = 1;
        std::uint64_t running = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            running += counts_[i];
            if (running >= target) return highest_equivalent(i);
        }
        return max_;
    }

    struct Bin {
        std::uint64_t lo;
        std::uint64_t hi;
        std::uint64_t count;
    };

    // Every populated sub-bucket -- full fidelity, can be thousands of bins.
    [[nodiscard]] std::vector<Bin> bins() const {
        std::vector<Bin> out;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (counts_[i] == 0) continue;
            out.push_back({value_from_index(i), highest_equivalent(i), counts_[i]});
        }
        return out;
    }

    // Log-spaced bins (`per_decade` points per power of ten) covering
    // [1, max]. Bounded size -- for charts and the dashboard snapshot.
    [[nodiscard]] std::vector<Bin> log_bins(int per_decade = 8) const {
        std::vector<Bin> out;
        if (total_ == 0) return out;

        std::vector<double> edges;
        const double step = std::pow(10.0, 1.0 / per_decade);
        for (double e = 1.0; e < static_cast<double>(max_) + 1.0; e *= step)
            edges.push_back(e);
        edges.push_back(static_cast<double>(max_) + 1.0);
        std::vector<std::uint64_t> acc(edges.size(), 0);

        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (counts_[i] == 0) continue;
            const double v = static_cast<double>(value_from_index(i));
            std::size_t k = static_cast<std::size_t>(
                std::upper_bound(edges.begin(), edges.end(), v) - edges.begin());
            if (k > 0) --k;
            if (k >= acc.size()) k = acc.size() - 1;
            acc[k] += counts_[i];
        }
        for (std::size_t k = 0; k + 1 < edges.size(); ++k) {
            if (acc[k] == 0) continue;
            out.push_back({static_cast<std::uint64_t>(edges[k]),
                           static_cast<std::uint64_t>(edges[k + 1]) - 1, acc[k]});
        }
        return out;
    }

private:
    [[nodiscard]] std::size_t counts_index(std::uint64_t value) const noexcept {
        const int bucket =
            lzc_base_ - std::countl_zero(value | sub_mask_);
        const int b = bucket < 0 ? 0 : bucket;
        const std::uint64_t sub = value >> b;
        const std::size_t base =
            static_cast<std::size_t>(b + 1) << sub_half_mag_;
        return base + static_cast<std::size_t>(sub) - sub_half_count_;
    }

    [[nodiscard]] std::uint64_t value_from_index(std::size_t i) const noexcept {
        std::int64_t bucket =
            static_cast<std::int64_t>(i >> sub_half_mag_) - 1;
        std::uint64_t sub = (i & (sub_half_count_ - 1)) + sub_half_count_;
        if (bucket < 0) {
            sub -= sub_half_count_;
            bucket = 0;
        }
        return sub << bucket;
    }

    [[nodiscard]] std::uint64_t highest_equivalent(std::size_t i) const noexcept {
        std::int64_t bucket =
            static_cast<std::int64_t>(i >> sub_half_mag_) - 1;
        if (bucket < 0) bucket = 0;
        return value_from_index(i) + ((1ULL << bucket) - 1);
    }

    int sub_half_mag_ = 0;
    std::uint64_t sub_half_count_ = 0;
    std::uint64_t sub_count_ = 0;
    std::uint64_t sub_mask_ = 0;
    int lzc_base_ = 0;
    int bucket_count_ = 0;

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace lob
