#pragma once
// TrackedHistogramWindow's median cursor and FifoMinMax low/high, but each symbol gets its own
// histogram scoped to its 24h band (range_bucketing.hpp) instead of one global scheme. A band that
// fits under the bucket cap gets one cent per bucket, so the median is bit-exact.
// Two bucket spaces, chosen at the first note_market(): RANGE (usable [low, high]) or FALLBACK to
// global PriceBucketing (no usable range). Both share one FlatOccupancyHistogram.
// Escape: a price outside the band would land in a coarse tail bucket. SlidingWindowStats::on_tick
// asks would_escape() before each insert and, if true, calls rebuild_market() with a wider band and
// replays every live sample from its ring (the policy only keeps counts). With the seam wired
// escape_count() stays 0; policy-level tests skip the seam to exercise the tail path.
// EnableRebuild=false (range_histogram_no_rebuild): would_escape() is always false, so an
// escaping price takes the coarse tail bucket. mean/low/high stay exact. This removes the rare
// O(live samples) rebuild, which matters most for long windows.
#include <cassert>
#include <cstdint>

#include "../fixed_price.hpp"
#include "fifo_min_max.hpp"
#include "flat_histogram.hpp"
#include "median_cursor.hpp"
#include "price_bucketing.hpp"
#include "range_bucketing.hpp"

namespace ticker {

// TailBuckets: log tail buckets per side. Fallback*: the global scheme's knobs. EnableRebuild: see
// the header.
template <int TailBuckets, int FallbackSubBits, int FallbackMaxOctave, bool EnableRebuild = true>
class RangeHistogramWindowT {
  public:
    using RangeB = detail::RangeBucketing<TailBuckets>;
    using FallbackB = detail::PriceBucketingT<FallbackSubBits, FallbackMaxOctave>;

    using Handle = detail::BucketHandle;

    explicit RangeHistogramWindowT(size_t /*capacity_hint*/) noexcept : histogram_(FallbackB::kNumBuckets) {}

    // True while note_market() would still do something; lets on_tick skip parsing the band strings
    // once a symbol is established.
    bool wants_market() const noexcept { return !established_ && size_ == 0; }

    // Establishes the band. A no-op once established or once a sample is live, since rebucketing
    // live samples needs a rebuild. Stays on global bucketing if the range is unusable.
    void note_market(Price low, Price high) noexcept {
        if (established_ || size_ != 0) return;
        established_ = true;
        if (range_.init(low, high)) {
            using_range_ = true;
            histogram_.clear_and_resize(range_.num_buckets());
        }
    }

    // True if inserting `price` would land in a tail bucket. Always false in fallback mode and when
    // EnableRebuild is false (compiles to a constant).
    bool would_escape(Price price) const noexcept {
        if constexpr (!EnableRebuild) {
            (void)price;
            return false;
        } else {
            return using_range_ && !range_.in_band(price);
        }
    }

    // Re-establishes a wider band and discards live state; the caller must re-insert
    // every live sample before the next query.
    void rebuild_market(Price new_low, Price new_high) noexcept {
        assert(using_range_);
        reset_state();
        established_ = false;
        using_range_ = false;
        ++rebuild_count_;
        note_market(new_low, new_high);
    }

    Handle insert(Price price) noexcept {
        const detail::Bucket bucket = bucket_of(price);
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

        const Price mid_lo = bucket_midpoint(cur_.bucket);
        if (size_ % 2 == 1) return mid_lo;
        const Price mid_hi = bucket_midpoint(cur_.successor_bucket(histogram_, target));
        return detail::avg_price(mid_lo, mid_hi);
    }

    Price low() noexcept { return minmax_.low(); }
    Price high() noexcept { return minmax_.high(); }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // Keeps the established bucket space and histogram size, so replay passes don't re-derive every
    // band.
    void clear() noexcept {
        histogram_.clear();
        reset_state();
    }

    // Per-symbol (bucket count varies); read by bench_main.cpp's histogram_fixed_bytes().
    size_t policy_bytes() const noexcept { return histogram_.bytes(); }

    // Diagnostics below are not part of the WindowPolicy concept.
    uint64_t fallback_count() const noexcept { return cur_.seeks; }
    // Inserts that landed in a tail bucket.
    uint64_t escape_count() const noexcept { return escape_count_; }
    // rebuild_market() calls that fired.
    uint64_t rebuild_count() const noexcept { return rebuild_count_; }
    bool using_range_bucketing() const noexcept { return using_range_; }
    // True when this symbol's median is bit-exact (one cent per bucket).
    bool exact() const noexcept { return using_range_ && range_.exact(); }

  private:
    detail::Bucket bucket_of(Price price) noexcept {
        if (using_range_) {
            if (!range_.in_band(price)) ++escape_count_;
            return range_.bucket_of(price);
        }
        return FallbackB::bucket_of(price);
    }

    Price bucket_midpoint(detail::Bucket b) const noexcept {
        if (using_range_) return range_.bucket_midpoint(b);
        return FallbackB::bucket_midpoint(b);
    }

    // Shared by clear() (same band) and rebuild_market() (about to establish a different one).
    void reset_state() noexcept {
        minmax_.reset();
        size_ = 0;
        cur_.invalidate();
    }

    size_t target_rank() const noexcept { return (size_ - 1) / 2; }

    detail::FlatOccupancyHistogram histogram_;
    RangeB range_;
    bool established_ = false;
    bool using_range_ = false;

    detail::FifoMinMax minmax_;
    size_t size_ = 0;

    detail::MedianCursor cur_;
    uint64_t escape_count_ = 0;
    uint64_t rebuild_count_ = 0;
};

// Shipped configuration: 32 log-spaced tail buckets per side (distance 2^0..2^31
// cents, so any uint32 price fits); fallback PriceBucketing<6, 26>.
using RangeHistogramWindow = RangeHistogramWindowT<32, 6, 26>;

// Rebuild disabled; meant for long (3600s) windows.
using RangeHistogramNoRebuildWindow = RangeHistogramWindowT<32, 6, 26, /*EnableRebuild=*/false>;

static_assert(sizeof(RangeHistogramWindow::Handle) == 8);
static_assert(sizeof(RangeHistogramNoRebuildWindow::Handle) == 8);

}  // namespace ticker
