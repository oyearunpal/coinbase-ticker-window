#pragma once
// Per-symbol bucketing for RangeHistogramWindow. Unlike PriceBucketingT it has instance state
// (origin, bucket width) derived from one symbol's 24h band, so the same bucket count covers a far
// smaller domain than the global log scheme.
//   [ kTailBuckets, log ][ band: width cents per bucket ][ kTailBuckets, log ]
// Buckets increase monotonically in Price, as the median cursor requires. A band that fits under the
// cap gets one cent per bucket, so the median is bit-exact; a wider one uses several cents per
// bucket. The tails are only a defensive fallback: the policy rebuilds from a wider range before an
// out-of-band price reaches bucket_of().
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>

#include "../fixed_price.hpp"
#include "flat_histogram.hpp"

namespace ticker::detail {

// Padding per side for a 24h band [low, high]: 10% of the span, but at least 0.1% of the price and
// kMinBandPad cents. Without the floor a flat or near-flat band (span < 10 cents) gets no pad, so
// every 1-cent move escapes it. Requires low <= high; never more than about UINT32_MAX / 10.
inline constexpr Price kMinBandPad = 100;
constexpr Price band_pad(Price low, Price high) noexcept {
    return std::max({(high - low) / 10, high / 1000, kMinBandPad});
}

template <int TailBuckets = 32>
class RangeBucketing {
  public:
    static constexpr Bucket kTailBuckets = TailBuckets;
    // A distance from the band edge is below 2^33, so 32 octaves cover every Price.
    static_assert(kTailBuckets >= 1 && kTailBuckets <= 32);

    RangeBucketing() = default;

    // Builds the band from the observed [low, high], padded by band_pad() on each side so ordinary
    // 24h drift stays inside. Returns false and leaves valid() false if they are unusable; the
    // caller then falls back to global bucketing.
    bool init(Price low, Price high, Bucket max_band_buckets = kDefaultMaxBandBuckets) noexcept {
        if (high < low) {
            valid_ = false;
            return false;
        }
        const Price pad = band_pad(low, high);
        // The pad can reach below zero (a cheap product, or one that moved >10x in 24h) and past
        // the largest Price; both ends clamp.
        origin_ = (low > pad) ? low - pad : 0;
        const Price padded_high = (high > UINT32_MAX - pad) ? UINT32_MAX : high + pad;

        // The band has band_distance + 1 cent steps, both endpoints included.
        // band_buckets_ = min(band_distance + 1, cap) and
        // width_ = ceil((band_distance + 1) / band_buckets_) = band_distance / band_buckets_ + 1,
        // written so band_distance + 1 is never formed.
        const Price band_distance = padded_high - origin_;
        const Bucket cap = std::max<Bucket>(max_band_buckets, 1);
        band_buckets_ = (band_distance >= cap) ? cap : band_distance + 1;
        width_ = band_distance / band_buckets_ + 1;
        valid_ = true;
        return true;
    }

    bool valid() const noexcept { return valid_; }

    // Cents per band bucket; 1 means the median is bit-exact.
    Price width() const noexcept {
        assert(valid_);
        return width_;
    }
    bool exact() const noexcept { return width() == 1; }
    Bucket band_buckets() const noexcept {
        assert(valid_);
        return band_buckets_;
    }
    Bucket num_buckets() const noexcept {
        assert(valid_);
        return band_buckets_ + 2 * kTailBuckets;
    }

    // Whether `price` is in the exact band rather than a log tail; a caller should rebuild on false
    // instead of accepting the coarser answer.
    bool in_band(Price price) const noexcept {
        assert(valid_);
        return price >= origin_ && (price - origin_) / width_ < band_buckets_;
    }

    Bucket bucket_of(Price price) const noexcept {
        assert(valid_);
        if (price < origin_) return kTailBuckets - 1 - tail_octave(origin_ - price);
        const Price offset = price - origin_;
        if (offset / width_ < band_buckets_) return kTailBuckets + offset / width_;
        // Right tail: here band_buckets_ * width_ <= offset, so it cannot overflow.
        return kTailBuckets + band_buckets_ + tail_octave(offset - band_buckets_ * width_ + 1);
    }

    // For the bucket of a live price (all the median cursor ever asks for): a band bucket's start
    // and the right tail's edge are then <= that price, so neither can overflow.
    Price bucket_midpoint(Bucket b) const noexcept {
        assert(valid_);
        if (b < kTailBuckets) {
            const Price d = tail_distance(kTailBuckets - 1 - b);
            return (origin_ > d) ? origin_ - d : 0;
        }
        const Bucket i = b - kTailBuckets;
        if (i < band_buckets_) {
            const Price start = origin_ + i * width_;
            // One cent per bucket: start is the real price. Only a multi-cent bucket has a centre
            // to report.
            if (width_ == 1) return start;
            return start + std::min<Price>(width_ / 2, UINT32_MAX - start);
        }
        const Price band_end = origin_ + band_buckets_ * width_;
        return band_end + std::min<Price>(tail_distance(i - band_buckets_), UINT32_MAX - band_end);
    }

    static constexpr Bucket kDefaultMaxBandBuckets = 1'000'000;

  private:
    // One tail bucket per octave of distance from the band edge.
    static Bucket tail_octave(Price distance) noexcept {
        assert(distance >= 1);
        return std::min<Bucket>(std::bit_width(distance) - 1, kTailBuckets - 1);
    }
    static Price tail_distance(Bucket e) noexcept { return Price{1} << e; }  // e < kTailBuckets <= 32

    bool valid_ = false;
    Price origin_ = 0;
    Price width_ = 1;  // cents per band bucket
    Bucket band_buckets_ = 1;
};

}  // namespace ticker::detail
