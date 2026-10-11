#pragma once
// Flat occupancy histogram: a plain count per bucket plus a hierarchical bitmap of the non-empty
// ones. inc()/dec() are O(1) (a bitmap word is touched only when a count goes 0 <-> 1); finding
// the nearest non-empty bucket above or below is one tzcnt/lzcnt per level, and there are at most
// ceil(log64 N) levels (3 for 65,536 buckets). Meant for MedianCursor, which never needs a prefix
// sum: it steps from the median bucket to its occupied neighbour.
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ticker::detail {

// Bucket index in every histogram. The largest one (range: a capped band plus tails) has about a
// million buckets, so 32 bits is plenty and keeps BucketHandle and the cursor small.
using Bucket = uint32_t;

class FlatOccupancyHistogram {
  public:
    static constexpr Bucket npos = UINT32_MAX;

    explicit FlatOccupancyHistogram(Bucket num_buckets = 1) { clear_and_resize(num_buckets); }

    void inc(Bucket b) noexcept {
        assert(b < n_);
        if (++counts_[b] == 1) set_bit(b);
    }

    void dec(Bucket b) noexcept {
        assert(b < n_ && counts_[b] > 0);
        if (--counts_[b] == 0) clear_bit(b);
    }

    uint32_t count_at(Bucket b) const noexcept { return counts_[b]; }

    // First occupied bucket strictly above `b`, or npos.
    Bucket next_nonempty(Bucket b) const noexcept { return next_from(b + 1); }

    // Last occupied bucket strictly below `b`, or npos.
    Bucket prev_nonempty(Bucket b) const noexcept {
        if (b == 0) return npos;
        Bucket i = b - 1;
        for (unsigned level = 0; level < levels_; ++level) {
            const Bucket w = i >> kWordShift;
            const uint64_t bits = bitmap_[level][w] & (~uint64_t{0} >> (kBitMask - (i & kBitMask)));
            if (bits) {
                i = (w << kWordShift) + kBitMask - std::countl_zero(bits);
                for (unsigned l = level; l > 0; --l) i = (i << kWordShift) + kBitMask - std::countl_zero(bitmap_[l - 1][i]);
                return i;
            }
            if (w == 0) return npos;
            i = w - 1;
        }
        return npos;
    }

    // Lowest occupied bucket, or npos when empty.
    Bucket first_nonempty() const noexcept { return next_from(0); }

    Bucket num_buckets() const noexcept { return n_; }

    // Resizes and zeroes; setup/rebuild path only.
    void clear_and_resize(Bucket num_buckets) {
        n_ = (num_buckets == 0 ? 1 : num_buckets);
        counts_ = std::vector<uint32_t>(n_, 0);
        Bucket words = n_;
        levels_ = 0;
        do {
            words = (words + kBitMask) >> kWordShift;
            bitmap_[levels_++] = std::vector<uint64_t>(words, 0);
        } while (words > 1);
        assert(levels_ <= kMaxLevels);
        for (unsigned l = levels_; l < kMaxLevels; ++l) bitmap_[l] = {};  // release a larger old size's levels
    }

    // Zeroes in place, keeping n_ and capacity; must not allocate.
    void clear() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0);
        for (unsigned l = 0; l < levels_; ++l) std::fill(bitmap_[l].begin(), bitmap_[l].end(), 0);
    }

    size_t bytes() const noexcept {
        size_t b = counts_.capacity() * sizeof(uint32_t);
        for (unsigned l = 0; l < levels_; ++l) b += bitmap_[l].capacity() * sizeof(uint64_t);
        return b;
    }

  private:
    // Each bitmap word covers 64 children: index >> kWordShift is the word,
    // index & kBitMask the bit within it.
    static constexpr unsigned kWordShift = 6;
    static constexpr Bucket kBitMask = 63;
    // 2^32 buckets need ceil(32 / 6) = 6 levels.
    static constexpr unsigned kMaxLevels = 6;

    // First occupied bucket at or above `i`, or npos.
    Bucket next_from(Bucket i) const noexcept {
        for (unsigned level = 0; level < levels_; ++level) {
            const Bucket w = i >> kWordShift;
            if (w >= bitmap_[level].size()) return npos;
            const uint64_t bits = bitmap_[level][w] & (~uint64_t{0} << (i & kBitMask));
            if (bits) {
                i = (w << kWordShift) + std::countr_zero(bits);
                for (unsigned l = level; l > 0; --l) i = (i << kWordShift) + std::countr_zero(bitmap_[l - 1][i]);
                return i;
            }
            i = w + 1;
        }
        return npos;
    }

    // Kept out of line: they run only when a bucket goes empty <-> non-empty. Inlined, GCC peels the
    // level loop into every insert()/erase(), roughly doubling their size, which measurably slowed
    // the feed path.
    [[gnu::noinline]] void set_bit(Bucket i) noexcept {
        for (unsigned level = 0; level < levels_; ++level) {
            uint64_t& word = bitmap_[level][i >> kWordShift];
            const bool word_was_zero = (word == 0);
            word |= uint64_t{1} << (i & kBitMask);
            if (!word_was_zero) return;  // the upper levels already know this word is non-empty
            i >>= kWordShift;
        }
    }

    [[gnu::noinline]] void clear_bit(Bucket i) noexcept {
        for (unsigned level = 0; level < levels_; ++level) {
            uint64_t& word = bitmap_[level][i >> kWordShift];
            word &= ~(uint64_t{1} << (i & kBitMask));
            if (word != 0) return;
            i >>= kWordShift;
        }
    }

    Bucket n_ = 1;
    unsigned levels_ = 0;
    std::vector<uint32_t> counts_;
    std::array<std::vector<uint64_t>, kMaxLevels> bitmap_;  // level 0: one bit per bucket
};

}  // namespace ticker::detail
