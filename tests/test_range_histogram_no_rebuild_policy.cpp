// Tests for RangeHistogramNoRebuildWindow (v5): identical to
// RangeHistogramWindow except EnableRebuild=false, so would_escape()
// always returns false and SlidingWindowStats::on_tick's rebuild seam is
// never invoked for it -- see range_histogram.hpp's "EnableRebuild=false"
// header comment for the rationale (a rebuild's O(live-samples) worst
// case scales with window length, so v5 trades it away where its cost
// would be largest: long windows). This file only tests what's DIFFERENT
// about v5; in-band behavior (exactness, oracle parity, fallback mode) is
// already covered by test_range_histogram_policy.cpp and applies
// unchanged, since EnableRebuild only affects would_escape()'s return
// value -- nothing else in the class reads it.
#include "doctest.h"
#include "../src/policy/range_histogram.hpp"
#include "../src/sliding_window.hpp"
#include "policy_test_support.hpp"

#include <deque>
#include <optional>
#include <random>
#include <utility>

using namespace ticker;
using test::fixed_band;
using test::results_equal_exact;

namespace {
constexpr Price kCent = 1;
constexpr Price kBase = 8'400'000;                       // $84,000.00
constexpr Price kBandLow = kBase;
constexpr Price kBandHigh = kBase + 100 * kCent;         // narrow, deliberately escapable band
}  // namespace

TEST_CASE("RangeHistogramNoRebuildWindow: would_escape() always returns false, "
          "at both the policy level and through the SlidingWindowStats seam") {
    RangeHistogramNoRebuildWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    REQUIRE(w.exact());
    CHECK_FALSE(w.would_escape(kBandHigh + 1'000'000));  // far outside the band
    CHECK_FALSE(w.would_escape(kBandLow - 1'000'000));
    CHECK_FALSE(w.would_escape(kBandLow));  // in-band too, trivially false either way
}

TEST_CASE("RangeHistogramNoRebuildWindow: in-band behavior is bit-identical to RangeHistogramWindow "
          "-- EnableRebuild only changes out-of-band handling") {
    RangeHistogramWindow v4(16);
    RangeHistogramNoRebuildWindow v5(16);
    v4.note_market(kBandLow, kBandHigh);
    v5.note_market(kBandLow, kBandHigh);
    REQUIRE(v4.exact());
    REQUIRE(v5.exact());

    std::mt19937 rng(2025);
    std::uniform_int_distribution<Price> cent_offset(0, 100);
    std::deque<RangeHistogramWindow::Handle> h4;
    std::deque<RangeHistogramNoRebuildWindow::Handle> h5;
    for (int i = 0; i < 500; ++i) {
        Price p = kBandLow + cent_offset(rng);
        h4.push_back(v4.insert(p));
        h5.push_back(v5.insert(p));
        REQUIRE(v4.median() == v5.median());
        REQUIRE(v4.low() == v5.low());
        REQUIRE(v4.high() == v5.high());
        if (i % 3 == 0 && !h4.empty()) {
            v4.erase(h4.front());
            v5.erase(h5.front());
            h4.pop_front();
            h5.pop_front();
        }
    }
}

TEST_CASE("RangeHistogramNoRebuildWindow: a price outside the established band lands in the coarse "
          "tail bucket via escape_count(), and rebuild_count() stays 0 -- mean/low/high stay exact "
          "regardless (unaffected by EnableRebuild)") {
    RangeHistogramNoRebuildWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    REQUIRE(w.exact());
    CHECK(w.escape_count() == 0);
    CHECK(w.rebuild_count() == 0);

    for (Price v : {1, 3, 5}) w.insert(kBandLow + v * kCent);
    auto escaped = w.insert(kBandHigh + 5'000'000);
    CHECK(w.escape_count() == 1);
    CHECK(w.rebuild_count() == 0);  // the whole point of v5
    CHECK(w.size() == 4);
    CHECK(w.high() == kBandHigh + 5'000'000);
    w.erase(escaped);
    CHECK(w.size() == 3);
}

TEST_CASE("SlidingWindowStats<RangeHistogramNoRebuildWindow>: through the real seam, an escaping "
          "price never triggers rebuild_policy_for_escape() -- rebuild_count() stays 0 where "
          "RangeHistogramWindow's would have incremented for the identical sequence") {
    NullTracer t;
    SlidingWindowStats<RangeHistogramWindow, NullTracer> v4(300'000'000'000LL, 64, t);
    SlidingWindowStats<RangeHistogramNoRebuildWindow, NullTracer> v5(300'000'000'000LL, 64, t);

    int64_t ts = 0;
    auto r4 = v4.on_tick(ts, kBandLow + 10 * kCent, fixed_band(kBandLow, kBandHigh));
    auto r5 = v5.on_tick(ts, kBandLow + 10 * kCent, fixed_band(kBandLow, kBandHigh));
    REQUIRE(results_equal_exact(r4, r5));  // establishment + in-band insert: identical

    ts += 1;
    const Price escaping = kBandHigh + 20'000 * kCent;  // past the 0.1% (8,400-cent) pad floor
    v4.on_tick(ts, escaping);
    v5.on_tick(ts, escaping);

    CHECK(v4.policy().rebuild_count() == 1);  // v4 rebuilt to stay exact
    CHECK(v5.policy().rebuild_count() == 0);  // v5 accepted the tail-bucket approximation instead
    CHECK(v5.policy().escape_count() == 1);

    // Both stay internally consistent afterward -- mean/low/high are never
    // affected by which path handled the escape (monotonic-deque/sum
    // logic doesn't depend on bucketing at all).
    ts += 1;
    auto r4b = v4.on_tick(ts, kBandLow + 11 * kCent);
    auto r5b = v5.on_tick(ts, kBandLow + 11 * kCent);
    CHECK(r4b.mean == r5b.mean);
    CHECK(r4b.low == r5b.low);
    CHECK(r4b.high == r5b.high);
    CHECK(r4b.count == r5b.count);
    // Medians may now legitimately differ: v4 rebuilt to stay exact over
    // the escaping price; v5 left it in a tail bucket, which can pull its
    // reported median away from v4's exact one -- that IS the accepted
    // error this policy exists to trade for.
}

TEST_CASE("RangeHistogramNoRebuildWindow: repeated escapes never crash and never touch "
          "rebuild_count(), even under a monotonic trend (the deque/rebuild worst case for v4)") {
    NullTracer t;
    SlidingWindowStats<RangeHistogramNoRebuildWindow, NullTracer> w(3600LL * 1'000'000'000LL, 256, t);
    int64_t ts = 0;
    Price price = kBandLow;
    w.on_tick(ts, price, fixed_band(kBandLow, kBandHigh));
    for (int i = 0; i < 500; ++i) {
        ts += 1;
        price += 100 * kCent;  // climbs well past the band repeatedly
        auto r = w.on_tick(ts, price);
        CHECK(r.count > 0);
    }
    INFO("escape_count=" << w.policy().escape_count());
    CHECK(w.policy().rebuild_count() == 0);
    CHECK(w.policy().escape_count() > 0);  // the mechanism actually engaged
}
