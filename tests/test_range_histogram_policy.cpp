// RangeHistogramWindow-specific tests: band establishment, escapes, clear(), policy_bytes() and a
// direct cross-check against ExactMapWindow. The shared WindowPolicy contract (exact and fallback
// modes) is in test_policy_contract.cpp.
#include "doctest.h"
#include "policy_test_support.hpp"
#include "../src/policy/exact_map.hpp"
#include "../src/policy/price_bucketing.hpp"
#include "../src/policy/range_histogram.hpp"
#include "../src/sliding_window.hpp"

#include <deque>
#include <random>
#include <vector>

using namespace ticker;

namespace {
// A realistic BTC-USD-shaped band in cents: origin $84,000, span comfortably
// inside the default 1M-bucket cap so establishing it always lands in the
// EXACT case.
constexpr Price kCent = 1;
constexpr Price kBase = 8'400'000;  // $84,000.00
constexpr Price kBandLow = kBase;
constexpr Price kBandHigh = kBase + 500'000;  // +$5,000 span
}  // namespace

TEST_CASE("RangeHistogramWindow: Handle layout matches TrackedHistogramWindow's") {
    static_assert(sizeof(RangeHistogramWindow::Handle) == 8);
    using Window = SlidingWindowStats<RangeHistogramWindow, NullTracer>;
    static_assert(sizeof(Window::Sample) == 16);
}

// -- Fallback mode (note_market() never called): same accuracy contract
// as v3, since it uses the identical global PriceBucketing scheme --

TEST_CASE("RangeHistogramWindow: fallback mode (no note_market) behaves like TrackedHistogramWindow "
          "-- within the proven bound, not exact") {
    RangeHistogramWindow w(16);
    CHECK_FALSE(w.using_range_bucketing());
    for (Price v : {5, 1, 9, 3, 7}) w.insert(v);
    CHECK(test::median_ok(w.median(), 5, test::kHistogramMaxRelErr));
    CHECK(w.low() == 1);
    CHECK(w.high() == 9);
    CHECK_FALSE(w.exact());
}

TEST_CASE("RangeHistogramWindow: a caller-supplied invalid range (e.g. the MOG-USD case -- "
          "low_24h/high_24h that failed to parse, so the caller has nothing valid to pass) "
          "falls back gracefully rather than corrupting state") {
    RangeHistogramWindow w(16);
    w.note_market(/*low=*/100, /*high=*/50);  // high < low: RangeBucketing::init() rejects this
    CHECK_FALSE(w.using_range_bucketing());
    for (Price v : {5, 1, 9, 3, 7}) w.insert(v);
    CHECK(test::median_ok(w.median(), 5, test::kHistogramMaxRelErr));
    CHECK(w.low() == 1);
    CHECK(w.high() == 9);
}

// -- Range (exact) mode: the headline claim ---------------------------------

TEST_CASE("RangeHistogramWindow: note_market with a real-world-shaped band establishes EXACT "
          "bucketing (one cent per bucket), not merely bounded") {
    RangeHistogramWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    CHECK(w.using_range_bucketing());
    CHECK(w.exact());
}

// -- note_market() establishment rules --------------------------------------

TEST_CASE("RangeHistogramWindow: note_market() is a no-op once any sample has already been "
          "inserted -- a range that arrives mid-window doesn't retroactively rebucket "
          "(no rebuild in this phase)") {
    RangeHistogramWindow w(16);
    w.insert(kBase);  // fallback mode, since note_market() hasn't run yet
    CHECK_FALSE(w.using_range_bucketing());
    w.note_market(kBandLow, kBandHigh);  // too late: size_ != 0
    CHECK_FALSE(w.using_range_bucketing());
}

TEST_CASE("RangeHistogramWindow: a second note_market() call is a no-op even before any insert "
          "(first call wins)") {
    RangeHistogramWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    REQUIRE(w.using_range_bucketing());
    const uint64_t bytes_before = w.policy_bytes();
    w.note_market(/*low=*/0, /*high=*/1);  // a very different, tiny range
    CHECK(w.policy_bytes() == bytes_before);  // unchanged -- second call ignored
}

// -- Escape handling: a price outside the established band -----------------

TEST_CASE("RangeHistogramWindow: a price outside the established band is still handled without "
          "crashing or corrupting size/low/high, and is counted via escape_count()") {
    RangeHistogramWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    REQUIRE(w.exact());
    CHECK(w.escape_count() == 0);

    for (Price v : {1, 3, 5}) w.insert(kBandLow + v * kCent);
    // Far outside the padded band (band spans ~$5,000; this is $50,000 above it).
    auto escaped = w.insert(kBandHigh + 5'000'000);
    CHECK(w.escape_count() == 1);
    CHECK(w.size() == 4);
    CHECK(w.high() == kBandHigh + 5'000'000);  // low/high stay exact regardless (monotonic deques)
    w.erase(escaped);
    CHECK(w.size() == 3);
}

TEST_CASE("RangeHistogramWindow: repeated escapes on the same side keep escape_count() accurate "
          "and never return an out-of-range bucket (verified indirectly: median()/low()/high() "
          "never crash or assert across many escapes)") {
    RangeHistogramWindow w(64);
    w.note_market(kBandLow, kBandHigh);
    std::deque<RangeHistogramWindow::Handle> handles;
    for (Price v = 0; v < 50; ++v) {
        handles.push_back(w.insert(kBandHigh + 1'000'000 + v * kCent));  // all outside the band
    }
    CHECK(w.escape_count() == 50);
    CHECK(w.size() == 50);
    (void)w.median();  // must not crash
    for (auto h : handles) w.erase(h);
    CHECK(w.empty());
}

// -- clear() preserves the established bucket space -------------------------

TEST_CASE("RangeHistogramWindow: clear() keeps the established range bucketing (using_range_bucketing() "
          "and histogram size unchanged), only resetting occupancy -- matching v3's contract "
          "that SlidingWindowStats::reset() relies on for multi-pass replay") {
    RangeHistogramWindow w(16);
    w.note_market(kBandLow, kBandHigh);
    REQUIRE(w.using_range_bucketing());
    const size_t bytes_before = w.policy_bytes();

    auto h = w.insert(kBase + 3 * kCent);
    w.erase(h);
    w.clear();

    CHECK(w.using_range_bucketing());  // still established
    CHECK(w.exact());
    CHECK(w.policy_bytes() == bytes_before);  // same band, same histogram size
    CHECK(w.empty());

    // A fresh insert after clear() behaves identically to the first pass.
    auto h2 = w.insert(kBase + 3 * kCent);
    CHECK(w.median() == kBase + 3 * kCent);
    w.erase(h2);
}

// -- policy_bytes() reflects the actual established size, not the
// fallback-sized initial allocation (policy-level check; the underlying
// FlatOccupancyHistogram-level regression is in test_flat_histogram.cpp) --

TEST_CASE("RangeHistogramWindow: policy_bytes() after establishing a small range band is far "
          "smaller than the ~6.9KB fallback (PriceBucketing::kNumBuckets) allocation") {
    // NOTE: kBandLow/kBandHigh (a ~$5,000 BTC-shaped span) are deliberately
    // NOT used here -- that band needs ~600,000 exact one-cent buckets
    // (typical of a real BTC-USD 24h range), which is LARGER than the 1,728-bucket global fallback,
    // not smaller. Range bucketing trades memory for exactness in that
    // regime. This test instead checks a genuinely small band (a few
    // dollars wide), where the histogram really should shrink relative to
    // the fallback.
    RangeHistogramWindow w(16);
    const size_t fallback_bytes = w.policy_bytes();  // sized to FallbackB::kNumBuckets at construction
    CHECK(fallback_bytes >= RangeHistogramWindow::FallbackB::kNumBuckets * sizeof(int));

    w.note_market(/*low=*/1000, /*high=*/1500);  // ~600 cents total, well under the fallback size
    REQUIRE(w.exact());
    CHECK(w.policy_bytes() < fallback_bytes);
}

// -- Direct cross-check against ExactMapWindow (the true oracle),
// exact mode: bit-identical output at every step, the strongest possible
// check for the "exact, not bounded" claim --

TEST_CASE("RangeHistogramWindow: bit-identical to ExactMapWindow in exact mode, "
          "wide-range prices") {
    std::mt19937 rng(999);
    // Prices spread across the whole band, so exactness is exercised across
    // the full range rather than only near one point.
    std::uniform_int_distribution<Price> cent_offset(0, kBandHigh - kBandLow);
    for (int trial = 0; trial < 20; ++trial) {
        ExactMapWindow we(2000);
        RangeHistogramWindow wr(2000);
        wr.note_market(kBandLow, kBandHigh);
        REQUIRE(wr.exact());
        std::deque<ExactMapWindow::Handle> handles_e;
        std::deque<RangeHistogramWindow::Handle> handles_r;
        std::uniform_int_distribution<int> op_choice(0, 2);

        for (int step = 0; step < 1000; ++step) {
            bool do_insert = handles_e.empty() || op_choice(rng) != 0 || handles_e.size() < 5;
            if (do_insert) {
                Price v = kBandLow + cent_offset(rng);
                handles_e.push_back(we.insert(v));
                handles_r.push_back(wr.insert(v));
            } else {
                we.erase(handles_e.front());
                wr.erase(handles_r.front());
                handles_e.pop_front();
                handles_r.pop_front();
            }
            if (!handles_e.empty()) {
                REQUIRE(we.low() == wr.low());
                REQUIRE(we.high() == wr.high());
                REQUIRE(we.median() == wr.median());  // bit-exact, the whole point of this test
                REQUIRE(we.size() == wr.size());
            }
        }
    }
}
