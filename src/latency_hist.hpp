#pragma once
// Allocation-free log-linear latency histogram for the benchmark tracer.
//
// Each power-of-two octave is split into 2048 linear sub-buckets (kSubBits =
// 11), so bucket width is at most ~0.05% of the value, and values below
// ~4096 ns are exact. Same idea as HdrHistogram, with a fixed layout.
//
// Counts are heap-allocated (~885 KB each; one per probe would overflow a
// thread stack). Percentiles report the bucket's lower bound, so they can
// read up to ~0.05% low, never high. min/max/mean are exact.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ticker {

class LatencyHist {
  public:
    static constexpr int kSubBits = 11;                // 2048 sub-buckets per octave
    static constexpr int kSubCount = 1 << kSubBits;     // 2048
    static constexpr int kMaxOctave = 64 - kSubBits;    // covers the full uint64 range
    static constexpr int kNumBuckets = kSubCount + kMaxOctave * kSubCount;

    void record(uint64_t ns) noexcept {
        const uint64_t x = ns + 1;  // shift so 0ns has a valid bucket
        const size_t b = bucket_of(x);
        ++counts_[b];
        ++count_;
        sum_ += ns;
        if (ns > max_) max_ = ns;
        if (count_ == 1 || ns < min_) min_ = ns;
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t sum() const noexcept { return sum_; }
    uint64_t max() const noexcept { return max_; }
    uint64_t min() const noexcept { return count_ ? min_ : 0; }
    double mean() const noexcept { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }

    // Returns the lower-bound estimate of the p-th percentile (0 <= p <= 100).
    uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        uint64_t target = static_cast<uint64_t>(std::ceil((p / 100.0) * static_cast<double>(count_)));
        if (target == 0) target = 1;
        uint64_t cum = 0;
        for (size_t b = 0; b < kNumBuckets; ++b) {
            cum += counts_[b];
            if (cum >= target) return bucket_lower_bound(b);
        }
        return max_;
    }

    static size_t bucket_of(uint64_t x) noexcept {
        // x >= 1 guaranteed by caller (record() adds 1).
        if (x < kSubCount) return static_cast<size_t>(x);  // linear region: one bucket per x (exact)
        const int e = std::bit_width(x) - 1;   // highest set bit position, e >= kSubBits since x>=kSubCount
        const int frac_bits = e - kSubBits;    // >= 0
        const uint64_t top = x >> frac_bits;   // in [kSubCount, 2*kSubCount)
        const int sub = static_cast<int>(top) - kSubCount;
        const int octave_index = e - kSubBits;  // 0-based octave beyond the linear region
        size_t b = static_cast<size_t>(kSubCount) + static_cast<size_t>(octave_index) * kSubCount + sub;
        if (b >= kNumBuckets) b = kNumBuckets - 1;
        return b;
    }

    // Lower bound of bucket b, expressed back in original (non-shifted) ns.
    static uint64_t bucket_lower_bound(size_t b) noexcept {
        if (b < static_cast<size_t>(kSubCount)) {
            return b == 0 ? 0 : b - 1;  // x=b -> ns = x-1
        }
        const size_t idx = b - kSubCount;
        const int octave_index = static_cast<int>(idx / kSubCount);
        const int sub = static_cast<int>(idx & (kSubCount - 1));
        const int e = octave_index + kSubBits;
        const int frac_bits = e - kSubBits;
        const uint64_t top = static_cast<uint64_t>(kSubCount + sub);
        const uint64_t x_low = top << frac_bits;
        return x_low - 1;  // convert back from shifted x to ns
    }

  private:
    std::vector<uint64_t> counts_ = std::vector<uint64_t>(kNumBuckets);
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
    uint64_t min_ = 0;
};

}  // namespace ticker
