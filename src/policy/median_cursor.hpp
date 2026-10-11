#pragma once
// O(1)-common-case median bookkeeping shared by the tracked, range and hybrid histograms. While
// `valid`: before <= rank < before + at, where `bucket` is a bucket, `before` the live samples in
// lower buckets and `at` those in `bucket`. on_add()/on_remove() keep that true in O(1). One
// insert/erase moves the median by at most one sample, so when settle() finds the rank outside
// `bucket` it steps to the neighbouring occupied bucket (FlatOccupancyHistogram's
// next_nonempty/prev_nonempty, O(1) each) until it covers it; usually a single step. `at` may
// drop to 0, and settle() steps off the empty bucket. The first on_add() after invalidate() parks
// the cursor on that bucket, so there is no cold-start search. `seeks` counts settle() calls that
// had to move and is deliberately not cleared by invalidate().
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../fixed_price.hpp"
#include "flat_histogram.hpp"

namespace ticker::detail {

// 8-byte Handle shared by the histogram policies: the exact price (for value_of()) plus its
// bucket.
struct BucketHandle {
    Price price;
    Bucket bucket;
};
static_assert(sizeof(BucketHandle) == 8);

struct MedianCursor {
    bool valid = false;
    Bucket bucket = 0;
    size_t before = 0;
    size_t at = 0;
    uint64_t seeks = 0;

    // valid is false only while the histogram is empty (construction, clear(), size back to 0),
    // so starting fresh at `b` is correct.
    void on_add(Bucket b) noexcept {
        if (!valid) {
            valid = true;
            bucket = b;
            before = 0;
            at = 1;
            return;
        }
        if (b < bucket) {
            ++before;
        } else if (b == bucket) {
            ++at;
        }
    }

    void on_remove(Bucket b) noexcept {
        if (!valid) return;
        if (b < bucket) {
            assert(before > 0);
            --before;
        } else if (b == bucket) {
            assert(at > 0);
            --at;
        }
    }

    [[nodiscard]] bool covers(size_t rank) const noexcept { return valid && before <= rank && rank < before + at; }

    // Requires rank < the histogram's total count.
    template <typename Histogram>
    void settle(const Histogram& hist, size_t rank) noexcept {
        assert(valid);
        if (covers(rank)) return;
        ++seeks;
        while (rank < before) {
            bucket = hist.prev_nonempty(bucket);
            assert(bucket != Histogram::npos);
            at = hist.count_at(bucket);
            before -= at;
        }
        while (rank >= before + at) {
            before += at;
            bucket = hist.next_nonempty(bucket);
            assert(bucket != Histogram::npos);
            at = hist.count_at(bucket);
        }
    }

    // Bucket of the sample after the one at `rank` (the even-n upper middle), for a cursor
    // already parked on `rank`: O(1) while inside `bucket`, else the next occupied bucket.
    template <typename Histogram>
    [[nodiscard]] Bucket successor_bucket(const Histogram& hist, size_t rank) const noexcept {
        if (rank - before + 1 < at) return bucket;
        return hist.next_nonempty(bucket);
    }

    void invalidate() noexcept { valid = false; }
};

}  // namespace ticker::detail
