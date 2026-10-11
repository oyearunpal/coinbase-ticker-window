#pragma once
// Flat histogram over a BOUNDED core price band, plus two small sorted-vector tails
// (sorted_price_run.hpp) for prices outside it. Read range_histogram.hpp first.
//   [ left tail ][ histogram core, one bucket per cent ][ right tail ]
// range_histogram sizes the histogram to a symbol's whole 24h band, but a sliding window only
// occupies a moving slice of it. This caps the core at MaxCoreBuckets and positions it around the data, so memory is
// O(min(band_cents, MaxCoreBuckets) + tails).
// note_market() fixes the core's bucket count; its position comes from the first insert. It is
// re-centred through the same would_escape()/rebuild_market() seam as range_histogram, but only when a tail
// would exceed MaxTailSize, so small excursions cost a sorted-vector insert rather than a window
// replay.
// Bit-exact whenever width_ == 1 (one cent per core bucket; tails store real prices). If a
// re-centre must cover a live span wider than MaxCoreBuckets cents,
// buckets widen and the median is only bounded until a later re-centre. That is sticky: a wide core
// makes tail overflow, the only trigger, rarer.
// low/high come from FifoMinMax (O(1), exact) rather than the tail ends: the core boundary is not
// the live extreme, so reading it would need a search of the core.
// A symbol with no usable range runs on the global PriceBucketing scheme with empty tails and no
// re-centring.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../fixed_price.hpp"
#include "fifo_min_max.hpp"
#include "flat_histogram.hpp"
#include "median_cursor.hpp"
#include "price_bucketing.hpp"
#include "range_bucketing.hpp"
#include "sorted_price_run.hpp"

namespace ticker {

// MaxCoreBuckets: cap on histogram buckets per symbol (65,536 one-cent buckets = 256 KB).
// MaxTailSize: samples either tail holds before a re-centre. Fallback*: the global scheme's knobs.
template <detail::Bucket MaxCoreBuckets, size_t MaxTailSize, int FallbackSubBits, int FallbackMaxOctave>
class HybridHistogramWindowT {
  public:
    using FallbackB = detail::PriceBucketingT<FallbackSubBits, FallbackMaxOctave>;

    // Smallest core, so even a flat band leaves room to centre the data.
    static constexpr detail::Bucket kMinCoreBuckets = 8;
    static_assert(MaxCoreBuckets >= kMinCoreBuckets, "a core needs room to be centred in");
    static_assert(MaxTailSize >= 1, "a zero-size tail would re-centre on every excursion");

    // `bucket` is a histogram index for a core sample or one of the tail sentinels below. erase()
    // needs it because a re-centre may have moved the core since the insert.
    using Handle = detail::BucketHandle;

    // Handle::bucket sentinels for tail samples.
    static constexpr detail::Bucket kLeftTail = 0xFFFFFFFFu;
    static constexpr detail::Bucket kRightTail = 0xFFFFFFFEu;
    static_assert(MaxCoreBuckets < kRightTail,
                  "core bucket indices must never collide with the tail sentinels");

    explicit HybridHistogramWindowT(size_t /*capacity_hint*/) noexcept : histogram_(FallbackB::kNumBuckets) {}

    // Same guard and purpose as RangeHistogramWindowT::wants_market().
    bool wants_market() const noexcept { return !established_ && size_ == 0; }

    // Fixes the core's bucket count from the symbol's padded 24h band (detail::band_pad() per side,
    // at most MaxCoreBuckets); the position is left to the first insert. Same establishment
    // guard as range_histogram.
    void note_market(Price low, Price high) noexcept {
        if (established_ || size_ != 0) return;
        established_ = true;
        if (high < low) return;  // unusable: stay on the global fallback scheme

        // A span of MaxCoreBuckets or more is capped anyway. Below that, span + 2 * pad stays far
        // under UINT32_MAX (pad is at most about UINT32_MAX / 1000 there).
        const Price span = high - low;
        const Price band_levels =
            (span >= MaxCoreBuckets) ? MaxCoreBuckets : span + 2 * detail::band_pad(low, high) + 1;
        core_buckets_ = std::clamp<Price>(band_levels, kMinCoreBuckets, MaxCoreBuckets);
        using_core_ = true;
        positioned_ = false;
        histogram_.clear_and_resize(core_buckets_);
    }

    // True only for an out-of-core price whose tail is already full: the re-centre trigger.
    // (range_histogram rebuilds on every out-of-band price.)
    bool would_escape(Price price) const noexcept {
        // positioned_ implies using_core_, so this also covers fallback mode.
        if (!positioned_) return false;
        if (in_core(price)) return false;
        const size_t tail_n = (price < origin_) ? left_tail_.size() : right_tail_.size();
        return tail_n >= MaxTailSize;
    }

    // Re-centres the core on [new_low, new_high] and discards live state. The caller re-inserts
    // every live sample; the bounds come from this policy's own low()/high(), so all of them land
    // in the core and both tails come back empty.
    void rebuild_market(Price new_low, Price new_high) noexcept {
        assert(using_core_);
        ++recenter_count_;
        clear();
        center_core(new_low, new_high);
    }

    Handle insert(Price price) noexcept {
        if (using_core_ && !positioned_) center_core(price, price);

        detail::Bucket bucket;
        if (!using_core_ || in_core(price)) {
            bucket = using_core_ ? core_bucket_of(price) : FallbackB::bucket_of(price);
            histogram_.inc(bucket);
            cur_.on_add(bucket);
        } else if (price < origin_) {
            left_tail_.insert(price);
            bucket = kLeftTail;
            note_tail_insert(left_tail_.size());
        } else {
            right_tail_.insert(price);
            bucket = kRightTail;
            note_tail_insert(right_tail_.size());
        }
        ++size_;
        minmax_.push(price);
        return Handle{price, bucket};
    }

    void erase(Handle h) noexcept {
        if (h.bucket == kLeftTail || h.bucket == kRightTail) {
            auto& tail = (h.bucket == kLeftTail) ? left_tail_ : right_tail_;
            const bool removed = tail.erase(h.price);
            assert(removed && "erase() of a tail price that isn't there -- Handle/window mismatch");
            (void)removed;
        } else {
            histogram_.dec(h.bucket);
            cur_.on_remove(h.bucket);
        }
        assert(size_ > 0);
        --size_;
        if (size_ == 0) cur_.invalidate();  // nothing left to track; next insert starts fresh
        minmax_.pop_oldest();
    }

    Price value_of(Handle h) const noexcept { return h.price; }

    // Not const: a cursor miss moves it. [left tail | core | right tail] is already globally
    // sorted, so a global rank maps to a structure by two comparisons.
    Price median() noexcept {
        const size_t n = size_;
        assert(n > 0);
        [[assume(n > 0)]];  // hot-path precondition; assert() alone is gone in Release
        const size_t left = left_tail_.size();
        const size_t core_n = n - left - right_tail_.size();
        const size_t r = (n - 1) / 2;

        const Price lo = element_at_tracked(r, left, core_n);
        if (n % 2 == 1) return lo;

        const size_t r2 = r + 1;
        Price hi;
        if (r >= left && r2 < left + core_n && cur_.valid && (r2 - left) < cur_.before + cur_.at) {
            hi = bucket_midpoint(cur_.bucket);  // successor shares the tracked bucket: O(1)
        } else {
            hi = element_at_readonly(r2, left, core_n, r);
        }
        return detail::avg_price(lo, hi);
    }

    Price low() noexcept { return minmax_.low(); }
    Price high() noexcept { return minmax_.high(); }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // Keeps the core geometry (count, origin, width) and resets contents only, so replay passes
    // don't re-derive it.
    void clear() noexcept {
        histogram_.clear();
        left_tail_.clear();
        right_tail_.clear();
        minmax_.reset();
        size_ = 0;
        cur_.invalidate();
    }

    // Includes the tails; leaving them out would flatter the structure meant to stay small.
    size_t policy_bytes() const noexcept { return histogram_.bytes() + left_tail_.bytes() + right_tail_.bytes(); }

    // Diagnostics below are not part of the WindowPolicy concept.
    uint64_t fallback_count() const noexcept { return cur_.seeks; }
    // Core re-centres (one whole-window replay each).
    uint64_t recenter_count() const noexcept { return recenter_count_; }
    // Inserts that landed outside the core.
    uint64_t tail_inserts() const noexcept { return tail_inserts_; }
    // High-water mark of either tail.
    size_t max_tail_size() const noexcept { return max_tail_size_; }
    detail::Bucket core_buckets() const noexcept { return using_core_ ? core_buckets_ : 0; }
    bool using_core_bucketing() const noexcept { return using_core_; }
    // True when this symbol's median is currently bit-exact.
    bool exact() const noexcept { return using_core_ && width_ == 1; }

  private:
    // Tested by bucket index rather than against the core's top, which can pass the largest Price.
    bool in_core(Price price) const noexcept {
        return positioned_ && price >= origin_ && (price - origin_) / width_ < core_buckets_;
    }

    detail::Bucket core_bucket_of(Price price) const noexcept { return (price - origin_) / width_; }

    // Only called for an occupied bucket, so `start` <= a live price and cannot overflow.
    Price bucket_midpoint(detail::Bucket b) const noexcept {
        if (!using_core_) return FallbackB::bucket_midpoint(b);
        const Price start = origin_ + b * width_;
        // One cent per bucket: `start` is the real price. Adding half a width would break
        // exactness.
        if (width_ == 1) return start;
        return start + std::min<Price>(width_ / 2, UINT32_MAX - start);
    }

    // Positions the core to cover [lo, hi], widening buckets only if that needs more than
    // core_buckets_ cents. Called by the first insert and by every re-centre.
    void center_core(Price lo, Price hi) noexcept {
        assert(using_core_);
        if (hi < lo) std::swap(lo, hi);
        const Price span = hi - lo;

        // width_ = ceil((span + 3) / core_buckets_): +3 levels of slack so the triggering price
        // never lands just outside the core and thrashes. Written as floor((span + 2) / C) + 1,
        // split so span + 2 cannot overflow.
        const Price c = core_buckets_;
        width_ = span / c + (span % c + 2) / c + 1;

        // covered - span, where covered = c * width_ may pass UINT32_MAX: the true difference is
        // below c + 3, so unsigned wraparound gives it exactly. Centre the core on [lo, hi] and
        // clamp at 0.
        const Price slack = (c * width_ - span) / 2;
        origin_ = (lo > slack) ? lo - slack : 0;
        positioned_ = true;
    }

    void note_tail_insert(size_t new_size) noexcept {
        ++tail_inserts_;
        max_tail_size_ = std::max(max_tail_size_, new_size);
    }

    // Element of global rank `rank`, refreshing the cursor if it falls in the core and has gone
    // stale. The cursor is core-relative, so a change in the left tail's size can invalidate it;
    // covers() catches that.
    Price element_at_tracked(size_t rank, size_t left, size_t core_n) noexcept {
        if (rank < left) return left_tail_[rank];
        if (rank >= left + core_n) return right_tail_[rank - left - core_n];
        const size_t cr = rank - left;
        cur_.settle(histogram_, cr);
        return bucket_midpoint(cur_.bucket);
    }

    // Same lookup without touching the cursor, for the even-n successor (`rank` == lower_rank + 1),
    // which must not move a cursor parked on the lower middle.
    Price element_at_readonly(size_t rank, size_t left, size_t core_n, size_t lower_rank) const noexcept {
        if (rank < left) return left_tail_[rank];
        if (rank >= left + core_n) return right_tail_[rank - left - core_n];
        // The lower middle is either in the left tail (so `rank` is the core's first sample) or in
        // the core, where element_at_tracked() just parked the cursor on it.
        if (lower_rank < left) return bucket_midpoint(histogram_.first_nonempty());
        return bucket_midpoint(cur_.successor_bucket(histogram_, lower_rank - left));
    }

    detail::FlatOccupancyHistogram histogram_;
    detail::SortedPriceRun left_tail_;  // prices below origin_, sorted ascending
    detail::SortedPriceRun right_tail_;  // prices above the core, sorted ascending

    bool established_ = false;
    bool using_core_ = false;
    bool positioned_ = false;
    Price origin_ = 0;
    Price width_ = 1;  // cents per core bucket
    detail::Bucket core_buckets_ = 1;

    detail::FifoMinMax minmax_;
    size_t size_ = 0;

    // Core-relative: a change in the left tail's size can invalidate it.
    detail::MedianCursor cur_;

    uint64_t recenter_count_ = 0;
    uint64_t tail_inserts_ = 0;
    size_t max_tail_size_ = 0;
};

// Shipped configuration:
//   65536 - core buckets: one cent each covers a $655 band exactly, 256 KB per symbol.
//   64    - samples per tail before a re-centre; small enough that a tail insert
//           (sorted vector) stays cheap.
//   6, 26 - fallback PriceBucketing (see price_bucketing.hpp).
using HybridHistogramWindow = HybridHistogramWindowT<65536, 64, 6, 26>;

static_assert(sizeof(HybridHistogramWindow::Handle) == 8, "same layout as RangeHistogramWindow::Handle");

}  // namespace ticker
