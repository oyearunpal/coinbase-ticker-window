// Tests for v8, HybridHistogramWindow (src/policy/hybrid_histogram.hpp):
// a bounded histogram core plus two sorted-vector tails, re-centred through
// the same SlidingWindowStats seam v4 uses for its escape rebuilds.
//
// Three distinct things are checked, because v8 has three distinct modes:
//   1. fallback  -- no usable [low, high]: identical behaviour to
//                   v3's global PriceBucketing, so the same PROVEN
//                   bound applies and nothing stronger is claimed.
//   2. exact     -- a core at one cent per bucket: the median must be
//                   LITERALLY bit-exact, tails included, so every check
//                   here is equality against the brute-force oracle, not
//                   a tolerance.
//   3. re-centring -- the core moving under a drifting price, which must
//                   stay bit-exact across every single step, not merely
//                   at the end.
//
// A deliberately tiny instantiation (kTinyCore buckets, kTinyTail tails)
// makes modes 2 and 3 reachable in a handful of ticks; the shipped
// HybridHistogramWindow's 65,536-bucket core would need a capture-sized
// price excursion to exercise the same paths.
#include "doctest.h"
#include "../src/policy/hybrid_histogram.hpp"
#include "../src/sliding_window.hpp"
#include "policy_test_support.hpp"
#include "reference_window.hpp"

#include <optional>
#include <random>
#include <utility>
#include <vector>

using namespace ticker;
using test::fixed_band;
using test::results_equal_exact;

namespace {
constexpr int kTinyCore = 64;  // 64 one-cent buckets -- trivially escapable
constexpr int kTinyTail = 4;   // 4 out-of-core samples per side before a re-centre

using TinyHybrid = HybridHistogramWindowT<kTinyCore, kTinyTail, 6, 26>;
using TinyWindow = SlidingWindowStats<TinyHybrid, NullTracer>;
using ShippedWindow = SlidingWindowStats<HybridHistogramWindow, NullTracer>;

constexpr Price kCent = 1;
constexpr Price kBase = 8'400'000;  // $84,000.00
constexpr int64_t kWindowNs = 300'000'000'000LL;
}  // namespace

TEST_CASE("HybridHistogramWindow: Handle layout matches the other histogram policies'") {
    static_assert(sizeof(HybridHistogramWindow::Handle) == 8);
    static_assert(sizeof(ShippedWindow::Sample) == 16);
}

// -- Mode 1: fallback (no usable market data) ------------------------------

TEST_CASE("HybridHistogramWindow: with no note_market(), runs on the global fallback scheme -- "
          "bounded, with both tails permanently unused") {
    TinyHybrid w(64);
    for (Price v : {5, 1, 9, 3, 7}) w.insert(v * kCent + kBase);
    CHECK_FALSE(w.using_core_bucketing());
    CHECK_FALSE(w.exact());
    CHECK(w.tail_inserts() == 0);
    CHECK(w.recenter_count() == 0);
    CHECK(test::median_ok(w.median(), 5 * kCent + kBase, test::kHistogramMaxRelErr));
    CHECK(w.low() == 1 * kCent + kBase);   // deque-backed: exact regardless of mode
    CHECK(w.high() == 9 * kCent + kBase);
}

TEST_CASE("HybridHistogramWindow: an unusable [low, high] leaves it in fallback rather than failing") {
    TinyHybrid a(64);
    a.note_market(kBase + 10 * kCent, kBase);  // high < low
    CHECK_FALSE(a.using_core_bucketing());
}

// -- Mode 2: exact core ----------------------------------------------------

TEST_CASE("HybridHistogramWindow: note_market() caps the core at MaxCoreBuckets even when the "
          "symbol's 24h band is far wider -- the memory claim, checked directly") {
    TinyHybrid w(64);
    w.note_market(kBase, kBase + 100'000 * kCent);  // a 100,000-cent band
    CHECK(w.using_core_bucketing());
    CHECK(w.core_buckets() == kTinyCore);  // capped, NOT 100,000-odd
    CHECK(w.exact());                      // still one cent per bucket
}

TEST_CASE("HybridHistogramWindow: a narrow 24h band is sized to the band, not to the cap") {
    HybridHistogramWindowT<4096, kTinyTail, 6, 26> w(64);
    // A 9-cent span pads by the band_pad() floor (100 cents) each side: 9 + 200 + 1 levels.
    w.note_market(1000, 1000 + 9 * kCent);
    CHECK(w.using_core_bucketing());
    CHECK(w.core_buckets() == 9 + 2 * detail::kMinBandPad + 1);
}

TEST_CASE("HybridHistogramWindow: an established core gives a bit-exact median, and the core is "
          "positioned by the first price rather than at note_market() time") {
    TinyHybrid w(64);
    w.note_market(kBase, kBase + 100'000 * kCent);
    CHECK(w.core_buckets() == kTinyCore);

    for (Price v : {5, 1, 9, 3, 7}) w.insert(kBase + v * kCent);
    CHECK(w.exact());
    CHECK(w.tail_inserts() == 0);  // all five land in a core centred on the first (5-cent) price
    CHECK(w.median() == kBase + 5 * kCent);
    CHECK(w.low() == kBase + 1 * kCent);
    CHECK(w.high() == kBase + 9 * kCent);
}

TEST_CASE("HybridHistogramWindow: prices outside the core land in the tails and are answered "
          "EXACTLY -- no re-centre until a tail fills") {
    TinyHybrid w(64);
    w.note_market(kBase, kBase + 100'000 * kCent);
    w.insert(kBase);  // centres the core on kBase

    // Far outside the core on both sides, but only a couple per side --
    // under kTinyTail, so no re-centre is triggered (this is the policy
    // calling insert() directly, with no seam wired; would_escape() is
    // the caller's business, and it is checked separately below).
    w.insert(kBase + 10'000 * kCent);
    w.insert(kBase - 10'000 * kCent);
    CHECK(w.tail_inserts() == 2);
    CHECK(w.recenter_count() == 0);
    CHECK(w.size() == 3);
    CHECK(w.median() == kBase);  // the core element is still the middle one
    CHECK(w.low() == kBase - 10'000 * kCent);
    CHECK(w.high() == kBase + 10'000 * kCent);
}

TEST_CASE("HybridHistogramWindow: a median that falls INSIDE a tail is resolved from the tail, exactly") {
    TinyHybrid w(64);
    w.note_market(kBase, kBase + 100'000 * kCent);
    w.insert(kBase);  // core centred here; one core sample

    // Four samples far above -- ranks 1..4 are all right-tail, so the
    // median of these five (rank 2) is a tail element, not a core bucket.
    for (int i = 1; i <= 4; ++i) w.insert(kBase + (1000 + i) * kCent);
    REQUIRE(w.size() == 5);
    CHECK(w.tail_inserts() == 4);
    CHECK(w.median() == kBase + 1002 * kCent);  // rank 2 of [core, 1001, 1002, 1003, 1004]
}

TEST_CASE("HybridHistogramWindow: mixed core/tail occupancy stays bit-exact against a brute-force "
          "oracle across a long interleaved insert/erase fuzz") {
    // Policy-level FIFO fuzz with a deliberately escapable core: prices
    // wander well beyond the 64-cent core, so every path (core hit, left tail,
    // right tail, median in each of the three) is exercised. No seam is
    // wired here, so no re-centring happens and the tails grow past
    // kTinyTail -- which is exactly the unwired-caller contract, and the
    // answers must still be exact.
    std::mt19937 rng(20250922);
    std::uniform_int_distribution<Price> step(0, 80);  // a step of -40..+40 cents
    TinyHybrid w(4096);
    w.note_market(kBase, kBase + 100'000 * kCent);

    std::deque<TinyHybrid::Handle> fifo;
    std::deque<Price> shadow;
    Price cur = kBase;
    for (int i = 0; i < 3000; ++i) {
        if (fifo.size() > 50 || (!fifo.empty() && (rng() % 3) == 0)) {
            w.erase(fifo.front());
            fifo.pop_front();
            shadow.pop_front();
        }
        cur = cur + step(rng) - 40;
        if (cur < kBase - 5'000 * kCent) cur = kBase - 5'000 * kCent;
        fifo.push_back(w.insert(cur));
        shadow.push_back(cur);

        std::vector<Price> snap(shadow.begin(), shadow.end());
        std::sort(snap.begin(), snap.end());
        const size_t n = snap.size();
        const Price expect = (n % 2 == 1) ? snap[n / 2]
                                          : static_cast<Price>((uint64_t{snap[n / 2 - 1]} + snap[n / 2] + 1) / 2);
        REQUIRE(w.size() == n);
        REQUIRE(w.median() == expect);
        REQUIRE(w.low() == snap.front());
        REQUIRE(w.high() == snap.back());
    }
    CHECK(w.tail_inserts() > 0);  // the tails really were exercised
}

// -- Mode 3: re-centring through the SlidingWindowStats seam ---------------

TEST_CASE("HybridHistogramWindow seam: on_tick's low_24h/high_24h establishes the core, and a "
          "window that stays put never re-centres") {
    NullTracer t;
    TinyWindow w(kWindowNs, 64, t);
    test::ReferenceWindow oracle(kWindowNs);
    for (Price v : {5, 1, 9, 3, 7}) {
        const Price p = kBase + v * kCent;
        REQUIRE(results_equal_exact(w.on_tick(1000, p, fixed_band(kBase, kBase + 100'000 * kCent)),
                                     oracle.on_tick(1000, p)));
    }
    CHECK(w.policy().using_core_bucketing());
    CHECK(w.policy().exact());
    CHECK(w.policy().recenter_count() == 0);
    CHECK(w.policy().tail_inserts() == 0);
}

TEST_CASE("HybridHistogramWindow seam: a tail fills to MaxTailSize before a re-centre fires -- "
          "small excursions cost a tail insert, not a whole-window replay") {
    NullTracer t;
    TinyWindow w(kWindowNs, 64, t);
    const Price lo24 = kBase, hi24 = kBase + 100'000 * kCent;
    w.on_tick(1000, kBase, fixed_band(lo24, hi24));  // centres the core on kBase

    // Each of these is far outside the core. The first kTinyTail land in
    // the right tail with no re-centre; the next one overflows it.
    for (int i = 1; i <= kTinyTail; ++i) {
        w.on_tick(1000 + i, kBase + (5'000 + i) * kCent, fixed_band(lo24, hi24));
        REQUIRE(w.policy().recenter_count() == 0);
    }
    CHECK(w.policy().tail_inserts() == static_cast<uint64_t>(kTinyTail));
    CHECK(w.policy().max_tail_size() == static_cast<size_t>(kTinyTail));

    w.on_tick(2000, kBase + 6'000 * kCent, fixed_band(lo24, hi24));
    CHECK(w.policy().recenter_count() == 1);
    // After a re-centre the core covers the whole live span, so both
    // tails come back empty -- that is what makes the replay worth doing.
    CHECK(w.policy().max_tail_size() == static_cast<size_t>(kTinyTail));  // high-water, not current
}

TEST_CASE("HybridHistogramWindow seam: a sustained monotonic trend forces many re-centres and "
          "stays bit-exact against the brute-force oracle at EVERY step") {
    // The regime v8 is designed for: the absolute price walks far beyond
    // any one core's coverage (1,200 cents here, ~19 core widths), while
    // the LIVE span at any instant stays small (~10 samples x 3 cents)
    // because old samples age out. That is exactly what makes a bounded,
    // re-centred core viable where v4's whole-band histogram is not --
    // and it is the case that must stay bit-exact, tick by tick, across
    // every core move.
    NullTracer t;
    TinyWindow w(kWindowNs, 256, t);
    test::ReferenceWindow oracle(kWindowNs);
    const Price lo24 = kBase, hi24 = kBase + 100'000 * kCent;

    int64_t ts = 1000;
    for (Price i = 0; i < 400; ++i) {
        ts += 30'000'000'000LL;  // 30s apart: ~10 samples live in a 300s window
        const Price p = kBase + i * 3 * kCent;
        REQUIRE(results_equal_exact(w.on_tick(ts, p, fixed_band(lo24, hi24)), oracle.on_tick(ts, p)));
    }
    CHECK(w.policy().recenter_count() > 5);  // the mechanism really fired, repeatedly
    CHECK(w.policy().exact());               // and never had to widen its buckets to do it
}

TEST_CASE("HybridHistogramWindow seam: a live span wider than the core cap falls back to multi-cent "
          "buckets -- bounded, not exact, and honestly reported by exact()") {
    NullTracer t;
    TinyWindow w(kWindowNs, 256, t);
    const Price lo24 = kBase, hi24 = kBase + 1'000'000 * kCent;

    // Drive the live span far past the 64-bucket core, with a long window
    // so nothing ages out: a re-centre must widen its buckets to cover it.
    int64_t ts = 1000;
    for (Price i = 0; i < 400; ++i) {
        ts += 100'000'000LL;
        w.on_tick(ts, kBase + i * 50 * kCent, fixed_band(lo24, hi24));
    }
    CHECK(w.policy().recenter_count() > 0);
    CHECK(w.policy().using_core_bucketing());
    CHECK_FALSE(w.policy().exact());  // width_ > 1: reported, not hidden
    CHECK(w.policy().core_buckets() == kTinyCore);  // the bucket COUNT never grows past the cap
    // mean/low/high are never part of the tradeoff, whatever the geometry.
    const auto r = w.on_tick(ts + 1, kBase);
    CHECK(r.low == kBase);
}

TEST_CASE("HybridHistogramWindow seam: the shipped 65,536-bucket configuration holds a realistic "
          "window entirely in its core, bit-exact, with no re-centring at all") {
    NullTracer t;
    ShippedWindow w(kWindowNs, 2048, t);
    test::ReferenceWindow oracle(kWindowNs);
    std::mt19937 rng(5150);
    std::uniform_int_distribution<Price> step(0, 60);  // a step of -30..+30 cents

    Price cur = kBase;
    int64_t ts = 0;
    for (int i = 0; i < 6000; ++i) {
        ts += 50'000'000LL;
        cur = cur + step(rng) - 30;  // a random walk of a few hundred cents, like a real 5-minute window
        REQUIRE(results_equal_exact(w.on_tick(ts, cur, fixed_band(kBase - 50'000 * kCent, kBase + 50'000 * kCent)),
                                     oracle.on_tick(ts, cur)));
    }
    CHECK(w.policy().exact());
    CHECK(w.policy().recenter_count() == 0);
    CHECK(w.policy().tail_inserts() == 0);
    // 65,536 counts x 4 bytes plus the occupancy bitmap (1,024 + 16 + 1
    // words) -- bounded regardless of how wide the symbol's 24h band is,
    // which is v8's entire reason for existing.
    CHECK(w.policy().policy_bytes() <=
          65536 * sizeof(int) + (1024 + 16 + 1) * sizeof(uint64_t) + 2 * 64 * sizeof(Price));
}

TEST_CASE("HybridHistogramWindow: clear() keeps the core geometry, so a replayed pass doesn't "
          "re-derive it from scratch") {
    NullTracer t;
    TinyWindow w(kWindowNs, 64, t);
    for (int i = 0; i < 10; ++i) w.on_tick(1000 + i, kBase + i * kCent, fixed_band(kBase, kBase + 100'000 * kCent));
    REQUIRE(w.policy().using_core_bucketing());
    const size_t buckets = w.policy().core_buckets();

    w.reset();
    CHECK(w.size() == 0);
    CHECK(w.policy().using_core_bucketing());
    CHECK(w.policy().core_buckets() == buckets);
    CHECK(w.policy().exact());
    const auto r = w.on_tick(1000, kBase + 3 * kCent, fixed_band(kBase, kBase + 100'000 * kCent));
    CHECK(r.count == 1);
    CHECK(r.median == kBase + 3 * kCent);
}
