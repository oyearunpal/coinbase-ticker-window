#pragma once
// Log-linear histogram (price_bucketing.hpp) over a flat count array (flat_histogram.hpp) with
// an incremental median cursor (median_cursor.hpp). insert/erase are O(1); median() is O(1) while
// the median stays in its bucket, which is most of the time since a bucket usually holds many
// samples, and otherwise steps to the neighbouring occupied bucket. The median is approximate
// (max relative error 1/127 ~= 0.79%); low/high are exact via FifoMinMax, and the mean is exact
// because Handle carries the exact price. capacity_hint is ignored. erase() must be called in
// FIFO order, which is all SlidingWindowStats does.
#include <cassert>
#include <cstdint>

#include "../fixed_price.hpp"
#include "fifo_min_max.hpp"
#include "flat_histogram.hpp"
#include "median_cursor.hpp"
#include "price_bucketing.hpp"

namespace ticker {

template <int SubBits, int MaxOctave>
class TrackedHistogramWindowT {
  public:
    using Bucketing = detail::PriceBucketingT<SubBits, MaxOctave>;
    using Handle = detail::BucketHandle;

    explicit TrackedHistogramWindowT(size_t /*capacity_hint*/) : histogram_(Bucketing::kNumBuckets) {}

    Handle insert(Price price) noexcept {
        const detail::Bucket bucket = Bucketing::bucket_of(price);
        histogram_.inc(bucket);
        ++size_;
        cur_.on_add(bucket);
        minmax_.push(price);
        return Handle{price, bucket};
    }

    void erase(Handle h) noexcept {
#ifdef TICKER_DEBUG_INVARIANTS
        assert(histogram_.count_at(h.bucket) >= 1);
#endif
        histogram_.dec(h.bucket);
        assert(size_ > 0);
        --size_;
        cur_.on_remove(h.bucket);
        if (size_ == 0) cur_.invalidate();  // nothing left to track; next insert starts fresh
        minmax_.pop_oldest();
    }

    Price value_of(Handle h) const noexcept { return h.price; }

    // Not const: a cursor miss moves it.
    Price median() noexcept {
        assert(size_ > 0);
        [[assume(size_ > 0)]];  // hot-path precondition; assert() alone is gone in Release
        const size_t target = target_rank();
        cur_.settle(histogram_, target);

        const Price mid_lo = Bucketing::bucket_midpoint(cur_.bucket);
        if (size_ % 2 == 1) return mid_lo;
        const Price mid_hi = Bucketing::bucket_midpoint(cur_.successor_bucket(histogram_, target));
        return detail::avg_price(mid_lo, mid_hi);
    }

    Price low() noexcept { return minmax_.low(); }
    Price high() noexcept { return minmax_.high(); }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        histogram_.clear();
        minmax_.reset();
        size_ = 0;
        cur_.invalidate();
    }

    size_t policy_bytes() const noexcept { return histogram_.bytes(); }

    // Diagnostic, not part of the WindowPolicy concept: median() calls that moved the cursor.
    uint64_t fallback_count() const noexcept { return cur_.seeks; }

  private:
    size_t target_rank() const noexcept { return (size_ - 1) / 2; }

    detail::FlatOccupancyHistogram histogram_;
    detail::FifoMinMax minmax_;
    size_t size_ = 0;

    detail::MedianCursor cur_;
};

// Shipped configuration: PriceBucketing<6, 26> (see price_bucketing.hpp).
using TrackedHistogramWindow = TrackedHistogramWindowT<6, 26>;

static_assert(sizeof(TrackedHistogramWindow::Handle) == 8);

}  // namespace ticker
