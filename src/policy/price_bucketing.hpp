#pragma once
// Log-linear bucketing over Price (same scheme as latency_hist.hpp): values below kSubCount get one
// bucket each, above that every octave is split into kSubCount sub-buckets, so a bucket's relative
// width is 1/kSubCount. Used by the tracked histogram, and as the fallback of
// range/hybrid for a symbol with no usable price band.
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>

#include "../fixed_price.hpp"
#include "flat_histogram.hpp"

namespace ticker::detail {

// SubBits: sub-buckets per octave = 2^SubBits (+1 halves the error bound and doubles kNumBuckets).
// MaxOctave: octaves of range covered above the linear region. 32 - SubBits covers every uint32
// Price (the one value 2^32 - 1 lands in the clamped last bucket); anything larger is unreachable
// buckets.
template <int SubBits, int MaxOctave>
class PriceBucketingT {
    static_assert(SubBits >= 1 && SubBits <= 16);
    static_assert(MaxOctave >= 1 && MaxOctave <= 32 - SubBits);

  public:
    static constexpr int kSubBits = SubBits;
    static constexpr Bucket kSubCount = Bucket{1} << kSubBits;
    static constexpr int kMaxOctave = MaxOctave;
    static constexpr Bucket kNumBuckets = kSubCount * (1 + kMaxOctave);

    // O(1). Clamped so it never returns an out-of-range index.
    static Bucket bucket_of(Price price) noexcept {
        const uint64_t x = static_cast<uint64_t>(price) + 1;  // shift so price=0 has a valid bucket
        if (x < kSubCount) return x;
        // top = x >> octave lies in [kSubCount, 2 * kSubCount), so
        // kSubCount + octave * kSubCount + (top - kSubCount) = octave * kSubCount + top.
        const int octave = std::bit_width(x) - 1 - kSubBits;
        const uint64_t b = uint64_t{kSubCount} * octave + (x >> octave);
        return static_cast<Bucket>(std::min<uint64_t>(b, kNumBuckets - 1));
    }

    // Midpoint of bucket b. Max relative error is 1/(2*kSubCount - 1) (1/127 for kSubBits=6), at
    // the second octave where rounding first appears; checked against brute force in
    // tests/test_tracked_histogram_policy.cpp.
    static Price bucket_midpoint(Bucket b) noexcept {
        if (b < kSubCount) return b - 1;  // linear region: width 1, x = b (b >= 1: x = price + 1)
        const int octave = (b - kSubCount) >> kSubBits;
        const uint64_t x_lo = uint64_t{kSubCount + ((b - kSubCount) & (kSubCount - 1))} << octave;
        return static_cast<Price>(x_lo + (uint64_t{1} << octave >> 1) - 1);  // back to the price domain
    }
};

// Shipped configuration: 2^6 = 64 sub-buckets per octave (max error 1/127, ~0.8%)
// and 26 octaves, which with 6 sub-bits covers every uint32 price (6 + 26 = 32).
// Chosen from the accuracy vs memory sweep in bench/bucketing_sweep.cpp.
using PriceBucketing = PriceBucketingT<6, 26>;

}  // namespace ticker::detail
