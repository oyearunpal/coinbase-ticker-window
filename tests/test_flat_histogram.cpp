// Tests for flat_histogram.hpp (FlatOccupancyHistogram) and MedianCursor. The bitmap is checked
// against a plain scan; the histogram policies are checked against a brute-force median on
// streams built to stress the cursor: sparse and bimodal prices, long erase runs between queries,
// and repeated empty -> non-empty cycles.
#include "doctest.h"
#include "../src/policy/flat_histogram.hpp"
#include "../src/policy/hybrid_histogram.hpp"
#include "../src/policy/range_histogram.hpp"
#include "../src/policy/tracked_histogram.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <vector>

using namespace ticker;
using detail::Bucket;
using detail::FlatOccupancyHistogram;

namespace {
Bucket scan_next(const std::vector<uint32_t>& c, Bucket b) {
    for (Bucket i = b + 1; i < c.size(); ++i) {
        if (c[i] > 0) return i;
    }
    return FlatOccupancyHistogram::npos;
}
Bucket scan_prev(const std::vector<uint32_t>& c, Bucket b) {
    for (Bucket i = b; i-- > 0;) {
        if (c[i] > 0) return i;
    }
    return FlatOccupancyHistogram::npos;
}
}  // namespace

TEST_CASE("FlatOccupancyHistogram: next/prev_nonempty match a scan across word and level boundaries") {
    // 64*64*3 + 5 buckets -> three bitmap levels, with the interesting indices straddling the
    // 64-bit word (63/64) and 4096-bit summary-word (4095/4096) boundaries.
    constexpr Bucket kN = 64 * 64 * 3 + 5;
    FlatOccupancyHistogram h(kN);
    std::vector<uint32_t> ref(kN, 0);
    CHECK(h.first_nonempty() == FlatOccupancyHistogram::npos);
    for (Bucket b : {Bucket{0}, Bucket{63}, Bucket{64}, Bucket{4095}, Bucket{4096}, Bucket{8191}, kN - 1}) {
        h.inc(b);
        h.inc(b);
        ref[b] += 2;
    }
    for (Bucket b : {Bucket{0}, Bucket{1}, Bucket{62}, Bucket{63}, Bucket{64}, Bucket{65}, Bucket{4094}, Bucket{4095},
                     Bucket{4096}, Bucket{4097}, Bucket{8190}, Bucket{8191}, Bucket{8192}, kN - 2, kN - 1}) {
        CAPTURE(b);
        CHECK(h.next_nonempty(b) == scan_next(ref, b));
        CHECK(h.prev_nonempty(b) == scan_prev(ref, b));
        CHECK(h.count_at(b) == ref[b]);
    }
    CHECK(h.first_nonempty() == 0);
    h.dec(0);
    h.dec(0);
    ref[0] = 0;
    CHECK(h.first_nonempty() == 63);
    h.clear();
    CHECK(h.first_nonempty() == FlatOccupancyHistogram::npos);
    CHECK(h.count_at(63) == 0);
}

TEST_CASE("FlatOccupancyHistogram: random inc/dec agrees with a scan, several sizes") {
    std::mt19937 rng(77);
    for (Bucket n : {1u, 2u, 63u, 64u, 65u, 1000u, 5000u}) {
        CAPTURE(n);
        FlatOccupancyHistogram h(n);
        std::vector<uint32_t> ref(n, 0);
        std::uniform_int_distribution<Bucket> pick(0, n - 1);
        for (int step = 0; step < 4000; ++step) {
            const Bucket b = pick(rng);
            if (ref[b] > 0 && rng() % 2 == 0) {
                h.dec(b);
                --ref[b];
            } else {
                h.inc(b);
                ++ref[b];
            }
            if (step % 7 == 0) {
                const Bucket q = pick(rng);
                REQUIRE(h.next_nonempty(q) == scan_next(ref, q));
                REQUIRE(h.prev_nonempty(q) == scan_prev(ref, q));
                REQUIRE(h.first_nonempty() == (ref[0] > 0 ? 0 : scan_next(ref, 0)));
            }
        }
    }
}

TEST_CASE("FlatOccupancyHistogram: size edge cases and bytes()") {
    FlatOccupancyHistogram zero(0);  // clamped to one bucket
    CHECK(zero.num_buckets() == 1);
    for (int k = 0; k < 3; ++k) zero.inc(0);
    CHECK(zero.count_at(0) == 3);
    CHECK(zero.first_nonempty() == 0);
    CHECK(zero.next_nonempty(0) == FlatOccupancyHistogram::npos);
    CHECK(zero.prev_nonempty(0) == FlatOccupancyHistogram::npos);

    FlatOccupancyHistogram h(100);
    const size_t small = h.bytes();
    CHECK(small >= 100 * sizeof(int));
    h.clear_and_resize(70'000);
    CHECK(h.bytes() > small);
    CHECK(h.first_nonempty() == FlatOccupancyHistogram::npos);
    h.clear_and_resize(100);  // shrinking releases the old allocation
    CHECK(h.bytes() == small);
}

namespace {
constexpr Price kBase = 8'400'000;  // $84,000.00 in cents

Price naive_median(std::deque<Price> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : static_cast<Price>((uint64_t{v[n / 2 - 1]} + v[n / 2] + 1) / 2);
}

// Drives `w` with one FIFO stream and checks it against a brute-force median after most steps:
// exactly when `exact`, else within PriceBucketing's proven bound. low/high/size are always exact.
// `gen(step, rng)` picks each price; some steps erase a run (sometimes to empty) with no query in
// between, so the cursor has to cover several ranks at once.
template <typename W, typename Gen>
void run_against_oracle(W& w, Gen gen, bool exact, int insert_bias, uint32_t seed) {
    std::mt19937 rng(seed);
    std::deque<typename W::Handle> handles;
    std::deque<Price> ref;
    auto erase_one = [&] {
        REQUIRE(w.value_of(handles.front()) == ref.front());
        w.erase(handles.front());
        handles.pop_front();
        ref.pop_front();
    };
    for (int step = 0; step < 6000; ++step) {
        const int roll = static_cast<int>(rng() % 1000);
        if (roll < 8 && !ref.empty()) {
            const size_t keep = (roll < 3) ? 0 : ref.size() / 4;
            while (ref.size() > keep) erase_one();
        } else if (ref.empty() || static_cast<int>(rng() % 100) < insert_bias) {
            const Price p = gen(step, rng);
            handles.push_back(w.insert(p));
            ref.push_back(p);
        } else {
            erase_one();
        }
        if (ref.empty() || rng() % 3 == 0) continue;
        REQUIRE(w.size() == ref.size());
        REQUIRE(w.low() == *std::min_element(ref.begin(), ref.end()));
        REQUIRE(w.high() == *std::max_element(ref.begin(), ref.end()));
        const Price want = naive_median(ref);
        const Price got = w.median();
        if (exact) {
            REQUIRE(got == want);
        } else {
            const double rel = std::abs(static_cast<double>(got) - static_cast<double>(want)) / static_cast<double>(want);
            REQUIRE(rel <= 1.0 / 127.0 + 1e-12);
        }
    }
}

// Sparse: ~40k cents apart at most. Bimodal: two clusters 30k cents apart whose weights swap every
// 600 steps, so the median crosses a long run of empty buckets. Dup: four prices.
Price sparse_price(int, std::mt19937& rng) { return kBase + static_cast<Price>(rng() % 40'000); }
Price bimodal_price(int step, std::mt19937& rng) {
    const bool high_phase = (step / 600) % 2 == 1;
    const bool high = (rng() % 100) < (high_phase ? 85u : 15u);
    return kBase + (high ? 30'000u : 0u) + static_cast<Price>(rng() % 6);
}
Price dup_price(int, std::mt19937& rng) { return kBase + static_cast<Price>(rng() % 4); }
// Wide: any magnitude, so the global log buckets are sparse too.
Price wide_price(int, std::mt19937& rng) { return 1 + static_cast<Price>(rng() % 4'000'000'000u); }

constexpr Price kBandLow = kBase, kBandHigh = kBase + 45'000;
}  // namespace

TEST_CASE("TrackedHistogramWindow: stress streams vs brute force, within the proven bound") {
    for (auto gen : {sparse_price, bimodal_price, dup_price, wide_price}) {
        for (int bias : {50, 55, 70}) {
            TrackedHistogramWindow w(64);
            run_against_oracle(w, gen, /*exact=*/false, bias, 11u + static_cast<uint32_t>(bias));
        }
    }
}

TEST_CASE("RangeHistogramWindow: stress streams vs brute force, exact inside the band") {
    for (auto gen : {sparse_price, bimodal_price, dup_price}) {
        RangeHistogramNoRebuildWindow w(64);  // no seam here; every price is in band anyway
        w.note_market(kBandLow, kBandHigh);
        REQUIRE(w.exact());
        run_against_oracle(w, gen, /*exact=*/true, 55, 21u);
    }
}

TEST_CASE("HybridHistogramWindow: stress streams vs brute force, exact (shipped and tiny core)") {
    // Without the SlidingWindowStats seam nothing re-centres, so the tiny 64-bucket core pushes most
    // samples into the exact tails and the rank arithmetic across tail/core/tail gets exercised.
    using TinyHybrid = HybridHistogramWindowT<64, 4, 6, 26>;
    for (auto gen : {sparse_price, bimodal_price, dup_price}) {
        TinyHybrid tiny(64);
        tiny.note_market(kBandLow, kBandHigh);
        run_against_oracle(tiny, gen, /*exact=*/true, 55, 31u);

        HybridHistogramWindow shipped(64);
        shipped.note_market(kBandLow, kBandHigh);
        run_against_oracle(shipped, gen, /*exact=*/true, 55, 41u);
    }
}
