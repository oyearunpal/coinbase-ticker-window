// The WindowPolicy contract, checked once for every policy configuration in
// policy_test_support.hpp: median (exact, or within the case's proven bound),
// low, high and size against an independent brute-force answer, at the policy
// level and through SlidingWindowStats. Policy-specific behaviour (cursor
// counters, bands, tails, rebuilds) stays in each policy's own test file.
#include "doctest.h"
#include "policy_test_support.hpp"
#include "reference_window.hpp"

#include <algorithm>
#include <deque>
#include <random>
#include <vector>

using namespace ticker;
using namespace ticker::test;

namespace {

// Checks every query against `live` (the multiset of prices currently in `w`).
template <typename C, typename Container>
void check_against(typename C::Policy& w, const Container& live) {
    REQUIRE(w.size() == live.size());
    REQUIRE(w.empty() == live.empty());
    if (live.empty()) return;
    std::vector<Price> cur(live.begin(), live.end());
    const Price exact = naive_median(cur);
    const Price got = w.median();
    CAPTURE(got);
    CAPTURE(exact);
    REQUIRE(median_ok(got, exact, C::kMedianTol));
    REQUIRE(w.low() == *std::min_element(cur.begin(), cur.end()));
    REQUIRE(w.high() == *std::max_element(cur.begin(), cur.end()));
}

// FIFO insert/evict fuzz over prices kBase + [0, max_offset]. Queries every `query_every` steps,
// so the median trackers also have to catch up after several changes.
template <typename C>
void fifo_fuzz(uint32_t seed, Price max_offset, int trials, int steps, int query_every = 1) {
    CAPTURE(seed);
    std::mt19937 rng(seed);
    for (int trial = 0; trial < trials; ++trial) {
        CAPTURE(trial);
        typename C::Policy w(4096);
        C::setup(w);
        std::deque<typename C::Policy::Handle> handles;
        std::deque<Price> ref;
        std::uniform_int_distribution<Price> offset(0, max_offset);
        for (int step = 0; step < steps; ++step) {
            CAPTURE(step);
            if (ref.size() < 5 || rng() % 3 != 0) {
                const Price v = kBase + offset(rng);
                handles.push_back(w.insert(v));
                ref.push_back(v);
            } else {
                REQUIRE(w.value_of(handles.front()) == ref.front());
                w.erase(handles.front());
                handles.pop_front();
                ref.pop_front();
            }
            if (step % query_every == 0) check_against<C>(w, ref);
        }
    }
}

// Window-level stream through SlidingWindowStats against test::ReferenceWindow.
template <typename C>
void window_stream(uint32_t seed, int64_t window_ns, int ticks, int64_t max_gap_ns, Price max_offset,
                   bool shuffle_ts = false) {
    CAPTURE(seed);
    CAPTURE(window_ns);
    std::mt19937 rng(seed);
    NullTracer t;
    SlidingWindowStats<typename C::Policy, NullTracer> w(window_ns, 256, t);
    ReferenceWindow oracle(window_ns);
    std::uniform_int_distribution<int64_t> gap(0, max_gap_ns);
    std::uniform_int_distribution<Price> offset(0, max_offset);
    int64_t ts = 1'000'000'000;
    for (int tick = 0; tick < ticks; ++tick) {
        CAPTURE(tick);
        ts += gap(rng);
        // shuffle_ts: every 5th event arrives with an older timestamp, which on_tick clamps.
        const int64_t event_ts = (shuffle_ts && tick % 5 == 4) ? ts - max_gap_ns * 3 : ts;
        const Price p = kBase + offset(rng);
        WindowResult got;
        if constexpr (C::kUsesBand) {
            got = w.on_tick(event_ts, p, fixed_band(kBase, kBase + kSpan));
        } else {
            got = w.on_tick(event_ts, p);
        }
        const WindowResult want = oracle.on_tick(event_ts, p);
        CAPTURE(got.median);
        CAPTURE(want.median);
        REQUIRE(results_match(got, want, C::kMedianTol));
    }
}

}  // namespace

// -- Policy level ---------------------------------------------------------------

TEST_CASE_TEMPLATE("Policy contract: single element, then back to empty", C, TICKER_ALL_POLICY_CASES) {
    typename C::Policy w(16);
    C::setup(w);
    // A band case must really be in its exact mode, or a tolerance-0 case would be testing the
    // fallback scheme by luck.
    if constexpr (C::kUsesBand) REQUIRE(w.exact());
    auto h = w.insert(kBase + 100);
    check_against<C>(w, std::vector<Price>{kBase + 100});
    w.erase(h);
    CHECK(w.empty());
    CHECK(w.size() == 0);
}

TEST_CASE_TEMPLATE("Policy contract: odd and even counts", C, TICKER_ALL_POLICY_CASES) {
    typename C::Policy w(16);
    C::setup(w);
    std::vector<Price> live;
    for (Price off : {500, 100, 900, 300, 700}) {
        w.insert(kBase + off);
        live.push_back(kBase + off);
    }
    check_against<C>(w, live);  // odd: middle element
    w.insert(kBase + 1100);
    live.push_back(kBase + 1100);
    check_against<C>(w, live);  // even: mean of the two middles
    w.insert(kBase + 2);        // even-count half cent: (x + y) odd sum rounds up
    live.push_back(kBase + 2);
    w.insert(kBase + 3);
    live.push_back(kBase + 3);
    check_against<C>(w, live);
}

TEST_CASE_TEMPLATE("Policy contract: all duplicates", C, TICKER_ALL_POLICY_CASES) {
    typename C::Policy w(16);
    C::setup(w);
    std::deque<typename C::Policy::Handle> hs;
    std::deque<Price> live;
    for (int i = 0; i < 7; ++i) {
        hs.push_back(w.insert(kBase + 42));
        live.push_back(kBase + 42);
        check_against<C>(w, live);
    }
    while (!hs.empty()) {
        w.erase(hs.front());
        hs.pop_front();
        live.pop_front();
        check_against<C>(w, live);
    }
}

TEST_CASE_TEMPLATE("Policy contract: monotone increasing and decreasing insert orders", C,
                   TICKER_ALL_POLICY_CASES) {
    for (const bool increasing : {true, false}) {
        CAPTURE(increasing);
        typename C::Policy w(64);
        C::setup(w);
        std::vector<Price> live;
        for (Price i = 0; i < 50; ++i) {
            const Price v = kBase + (increasing ? i : 49 - i) * 10;
            w.insert(v);
            live.push_back(v);
            check_against<C>(w, live);
        }
    }
}

TEST_CASE_TEMPLATE("Policy contract: monotonic trend with FIFO eviction (min/max deque worst case)", C,
                   TICKER_ALL_POLICY_CASES) {
    typename C::Policy w(64);
    C::setup(w);
    std::deque<typename C::Policy::Handle> hs;
    std::deque<Price> live;
    for (Price i = 0; i < 2000; ++i) {
        CAPTURE(i);
        hs.push_back(w.insert(kBase + i));
        live.push_back(kBase + i);
        if (live.size() > 50) {
            w.erase(hs.front());
            hs.pop_front();
            live.pop_front();
        }
        check_against<C>(w, live);
    }
}

TEST_CASE_TEMPLATE("Policy contract: FIFO fuzz, duplicate-heavy alphabet", C, TICKER_ALL_POLICY_CASES) {
    fifo_fuzz<C>(/*seed=*/123, /*max_offset=*/4, /*trials=*/30, /*steps=*/2000);
}

TEST_CASE_TEMPLATE("Policy contract: FIFO fuzz, wide distinct alphabet", C, TICKER_ALL_POLICY_CASES) {
    fifo_fuzz<C>(/*seed=*/2024, /*max_offset=*/20'000, /*trials=*/10, /*steps=*/2000);
}

TEST_CASE_TEMPLATE("Policy contract: sparse median() calls stay right after many changes", C,
                   TICKER_ALL_POLICY_CASES) {
    fifo_fuzz<C>(/*seed=*/9001, /*max_offset=*/10, /*trials=*/5, /*steps=*/5000, /*query_every=*/37);
}

TEST_CASE_TEMPLATE("Policy contract: clear() leaves a window that behaves like a fresh one", C,
                   TICKER_ALL_POLICY_CASES) {
    typename C::Policy w(64);
    C::setup(w);
    for (Price i = 0; i < 100; ++i) w.insert(kBase + (i * 37) % 200);
    w.clear();
    CHECK(w.empty());
    CHECK(w.size() == 0);
    std::deque<typename C::Policy::Handle> hs;
    std::deque<Price> live;
    for (Price v : {50, 10, 90, 30, 70, 30}) {
        hs.push_back(w.insert(kBase + v));
        live.push_back(kBase + v);
        check_against<C>(w, live);
    }
    while (!hs.empty()) {
        w.erase(hs.front());
        hs.pop_front();
        live.pop_front();
        check_against<C>(w, live);
    }
}

TEST_CASE_TEMPLATE("Policy contract: random-order erase (not FIFO)", C, TICKER_ARBITRARY_ERASE_CASES) {
    const uint32_t seed = 7;
    CAPTURE(seed);
    std::mt19937 rng(seed);
    for (int trial = 0; trial < 15; ++trial) {
        CAPTURE(trial);
        typename C::Policy w(2000);
        C::setup(w);
        std::vector<std::pair<typename C::Policy::Handle, Price>> live;
        std::uniform_int_distribution<Price> offset(0, 20);
        for (int step = 0; step < 2000; ++step) {
            CAPTURE(step);
            if (live.empty() || rng() % 3 != 0) {
                const Price v = kBase + offset(rng);
                live.push_back({w.insert(v), v});
            } else {
                const size_t idx = rng() % live.size();
                REQUIRE(w.value_of(live[idx].first) == live[idx].second);
                w.erase(live[idx].first);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            std::vector<Price> prices;
            for (const auto& e : live) prices.push_back(e.second);
            check_against<C>(w, prices);
        }
    }
}

// -- Through SlidingWindowStats -------------------------------------------------

TEST_CASE_TEMPLATE("Window contract: 300s and 3600s windows match the reference window", C,
                   TICKER_ALL_POLICY_CASES) {
    for (const int64_t window_ns : {300LL * 1'000'000'000LL, 3600LL * 1'000'000'000LL}) {
        window_stream<C>(/*seed=*/99, window_ns, /*ticks=*/3000, /*max_gap_ns=*/200'000'000,
                         /*max_offset=*/20'000);
    }
}

TEST_CASE_TEMPLATE("Window contract: duplicate-heavy stream matches the reference window", C,
                   TICKER_ALL_POLICY_CASES) {
    window_stream<C>(/*seed=*/7, /*window_ns=*/5000, /*ticks=*/5000, /*max_gap_ns=*/50, /*max_offset=*/3);
}

TEST_CASE_TEMPLATE("Window contract: out-of-order (clamped) timestamps match the reference window", C,
                   TICKER_ALL_POLICY_CASES) {
    window_stream<C>(/*seed=*/4242, /*window_ns=*/2'000'000, /*ticks=*/3000, /*max_gap_ns=*/10'000,
                     /*max_offset=*/200, /*shuffle_ts=*/true);
}
