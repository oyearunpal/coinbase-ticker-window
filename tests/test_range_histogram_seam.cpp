// Tests for the SlidingWindowStats<RangeHistogramWindow, ...> seam:
// on_tick's optional low_24h/high_24h forwarding into note_market(), and
// the escape -> rebuild_market() -> re-insert-from-ring sequence
// (rebuild_policy_for_escape() in sliding_window.hpp). test_range_histogram_
// policy.cpp already covers the POLICY in isolation (calling insert()/
// note_market() directly); this file is specifically about the glue that
// only exists at the SlidingWindowStats level, because that's the only
// place the live ring (needed to re-insert every sample under a rebuilt,
// wider band) is available.
#include "doctest.h"
#include "../src/policy/range_histogram.hpp"
#include "../src/sliding_window.hpp"
#include "policy_test_support.hpp"
#include "reference_window.hpp"

#include <optional>
#include <random>
#include <utility>

using namespace ticker;
using test::fixed_band;
using test::results_equal_exact;
using RangeWindow = SlidingWindowStats<RangeHistogramWindow, NullTracer>;

namespace {
constexpr Price kCent = 1;
constexpr Price kBase = 8'400'000;                       // $84,000.00
constexpr Price kBandLow = kBase;
constexpr Price kBandHigh = kBase + 100 * kCent;         // a narrow, deliberately escapable band
}  // namespace

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: on_tick's low_24h/high_24h establishes the "
          "range band via note_market(), and stays bit-exact against the brute-force oracle") {
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 16, t);
    test::ReferenceWindow oracle(300'000'000'000LL);

    for (Price v : {5, 1, 9, 3, 7}) {
        Price price = kBandLow + v * kCent;
        auto r1 = w.on_tick(1000, price, fixed_band(kBandLow, kBandHigh));
        auto r2 = oracle.on_tick(1000, price);
        REQUIRE(results_equal_exact(r1, r2));
    }
    CHECK(w.policy().using_range_bucketing());
    CHECK(w.policy().exact());
    CHECK(w.policy().rebuild_count() == 0);  // every price stayed inside the established band
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: without ever being given low_24h/high_24h, "
          "the seam is simply never engaged -- fallback mode, exactly like calling the policy "
          "directly with no note_market()") {
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 16, t);
    auto r = w.on_tick(1000, kBase + 42);
    CHECK(r.count == 1);
    CHECK_FALSE(w.policy().using_range_bucketing());
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: a price outside the established band "
          "triggers exactly one rebuild, and the window stays bit-exact against the brute-force "
          "oracle across it -- the seam's core correctness claim") {
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 64, t);
    test::ReferenceWindow oracle(300'000'000'000LL);
    int64_t ts = 0;

    // Establish a narrow band from the first tick.
    for (Price v : {10, 20, 30}) {
        Price price = kBandLow + v * kCent;
        ts += 1;
        auto r1 = w.on_tick(ts, price, fixed_band(kBandLow, kBandHigh));
        auto r2 = oracle.on_tick(ts, price);
        REQUIRE(results_equal_exact(r1, r2));
    }
    REQUIRE(w.policy().using_range_bucketing());
    REQUIRE(w.policy().rebuild_count() == 0);

    // Now a price far outside [kBandLow, kBandHigh] -- no low_24h/high_24h
    // supplied on this tick (note_market() would be a no-op anyway once
    // established; the escape has to be caught by would_escape(), not by
    // a market-data update).
    ts += 1;
    Price escaping = kBandHigh + 20'000 * kCent;  // past the 0.1% (8,400-cent) pad floor
    auto r1 = w.on_tick(ts, escaping);
    auto r2 = oracle.on_tick(ts, escaping);
    REQUIRE(results_equal_exact(r1, r2));
    CHECK(w.policy().rebuild_count() == 1);
    CHECK(w.policy().exact());  // the rebuilt band is still exact for this price range

    // And the window keeps working correctly afterward, including
    // eviction of the pre-rebuild samples in the original FIFO order.
    for (Price v : {31, 32, 33}) {
        Price price = kBandLow + v * kCent;
        ts += 1;
        auto ra = w.on_tick(ts, price);
        auto rb = oracle.on_tick(ts, price);
        REQUIRE(results_equal_exact(ra, rb));
    }
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: a monotonic trend that repeatedly escapes "
          "the established band still tracks the brute-force oracle exactly, tick by tick, "
          "through multiple rebuilds") {
    NullTracer t;
    RangeWindow w(3600LL * 1'000'000'000LL, 256, t);
    test::ReferenceWindow oracle(3600LL * 1'000'000'000LL);

    int64_t ts = 0;
    Price price = kBandLow;
    // First tick establishes a narrow band; every subsequent price climbs
    // by 100 cents -- enough to escape the band (100-cent span padded by
    // the 0.1% floor, 8,400 cents each side) repeatedly over 500 steps.
    auto r0 = w.on_tick(ts, price, fixed_band(kBandLow, kBandHigh));
    auto o0 = oracle.on_tick(ts, price);
    REQUIRE(results_equal_exact(r0, o0));

    for (int i = 0; i < 500; ++i) {
        ts += 1;
        price += 100 * kCent;
        auto r1 = w.on_tick(ts, price);
        auto r2 = oracle.on_tick(ts, price);
        REQUIRE(results_equal_exact(r1, r2));
    }
    INFO("rebuild_count=" << w.policy().rebuild_count());
    CHECK(w.policy().rebuild_count() > 1);  // the mechanism actually engaged more than once
    CHECK(w.policy().exact());
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: FIFO fuzz with occasional wide-range escapes, "
          "bit-exact against the brute-force oracle at every step") {
    std::mt19937 rng(2024);
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 128, t);
    test::ReferenceWindow oracle(300'000'000'000LL);
    std::uniform_int_distribution<Price> cent_offset(0, 4000);  // kBase +-2000 cents: escapes often
    int64_t ts = 0;

    auto r0 = w.on_tick(ts, kBandLow, fixed_band(kBandLow, kBandHigh));
    auto o0 = oracle.on_tick(ts, kBandLow);
    REQUIRE(results_equal_exact(r0, o0));

    for (int i = 0; i < 2000; ++i) {
        ts += 1;
        const Price price = kBase - 2000 + cent_offset(rng);
        auto r1 = w.on_tick(ts, price);
        auto r2 = oracle.on_tick(ts, price);
        REQUIRE(results_equal_exact(r1, r2));
    }
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: the lazy BandFn overload calls `band()` only "
          "while the policy still wants_market() -- never again once established, even though the "
          "caller keeps offering it every tick") {
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 16, t);

    int band_calls = 0;
    auto band = [&]() noexcept -> std::pair<std::optional<Price>, std::optional<Price>> {
        ++band_calls;
        return {kBandLow, kBandHigh};
    };

    // First tick: wants_market() is still true, so band() is called and
    // the range band gets established from it.
    w.on_tick(1000, kBandLow + 5 * kCent, band);
    CHECK(band_calls == 1);
    CHECK(w.policy().using_range_bucketing());

    // Every subsequent tick: already established (wants_market() is now
    // false), so band() is never invoked again -- this is the whole point
    // of the seam (ticker_parser.hpp's Ticker::low_24h()/high_24h() run
    // parse_price() lazily inside a band() like this one; this checks
    // that laziness is actually exercised, not just available).
    for (int i = 0; i < 10; ++i) {
        w.on_tick(1000 + i + 1, kBandLow + (6 + i) * kCent, band);
    }
    CHECK(band_calls == 1);
}

TEST_CASE("SlidingWindowStats<RangeHistogramWindow>: a band that failed to parse (nullopt) leaves the "
          "policy in fallback mode, and band() is only asked once -- a live sample ends wants_market()") {
    NullTracer t;
    RangeWindow w(300'000'000'000LL, 16, t);

    int band_calls = 0;
    auto band = [&]() noexcept -> std::pair<std::optional<Price>, std::optional<Price>> {
        ++band_calls;
        return {std::nullopt, std::nullopt};
    };

    w.on_tick(1000, kBandLow + 5 * kCent, band);
    w.on_tick(1001, kBandLow + 6 * kCent, band);
    CHECK(band_calls == 1);
    CHECK_FALSE(w.policy().using_range_bucketing());
}
