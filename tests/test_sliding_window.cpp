// SlidingWindowStats itself (eviction, rounding, clamping, reset, ring growth) and Ring. Per-policy
// parity against the reference window is in test_policy_contract.cpp.
#include "doctest.h"
#include "../src/sliding_window.hpp"
#include "../src/policy/exact_map.hpp"
#include "policy_test_support.hpp"
#include "reference_window.hpp"

#include <deque>
#include <random>
#include <vector>

using namespace ticker;
using Window = SlidingWindowStats<ExactMapWindow, NullTracer>;


TEST_CASE("SlidingWindowStats: single sample") {
    NullTracer t;
    Window w(300'000'000'000LL, 16, t);
    auto r = w.on_tick(1'000, 100);
    CHECK(r.count == 1);
    CHECK(r.mean == 100);
    CHECK(r.median == 100);
    CHECK(r.low == 100);
    CHECK(r.high == 100);
}

TEST_CASE("SlidingWindowStats: an even-count half-cent median rounds up to a cent") {
    NullTracer t;
    Window w(300'000'000'000LL, 16, t);
    w.on_tick(1, parse_price("100.01").value());
    auto r = w.on_tick(2, parse_price("100.02").value());
    CHECK(r.median == 10'002u);  // 100.015 -> 100.02
    w.on_tick(3, parse_price("100.03").value());
    r = w.on_tick(4, parse_price("100.05").value());
    CHECK(r.median == 10'003u);  // (100.02 + 100.03) / 2 = 100.025 -> 100.03
}

TEST_CASE("SlidingWindowStats: mean rounds half-up to a cent") {
    NullTracer t;
    Window w(300'000'000'000LL, 16, t);
    w.on_tick(1, 100);  // 1.00
    w.on_tick(2, 100);  // 1.00
    auto r = w.on_tick(3, 101);  // mean 1.00333... -> 1.00
    CHECK(r.mean == 100u);

    Window w2(300'000'000'000LL, 16, t);
    w2.on_tick(1, 100);
    r = w2.on_tick(2, 101);  // mean 1.005 -> 1.01
    CHECK(r.mean == 101u);

    Window w3(300'000'000'000LL, 16, t);
    w3.on_tick(1, 100);
    w3.on_tick(2, 101);
    r = w3.on_tick(3, 101);  // mean 1.00666... -> 1.01
    CHECK(r.mean == 101u);
}

TEST_CASE("SlidingWindowStats: the mean sum cannot wrap at the top of the Price range") {
    NullTracer t;
    Window w(300'000'000'000LL, 4096, t);
    WindowResult r{};
    for (int64_t ts = 1; ts <= 2000; ++ts) r = w.on_tick(ts, UINT32_MAX);
    CHECK(r.mean == UINT32_MAX);
}

TEST_CASE("SlidingWindowStats: eviction exactly on the half-open boundary") {
    NullTracer t;
    const int64_t window_ns = 1000;
    Window w(window_ns, 16, t);
    w.on_tick(0, 10);
    w.on_tick(500, 20);
    // A sample at exactly t - window_ns must be evicted (cutoff = t - window_ns,
    // eviction rule is ts <= cutoff).
    auto r = w.on_tick(1000, 30);  // cutoff = 1000-1000 = 0 -> sample at ts=0 evicted
    CHECK(r.count == 2);           // only the ts=500 and ts=1000 samples remain
    CHECK(r.low == 20);

    // One nanosecond later than the boundary, the sample survives: with
    // cutoff = t - window_ns = 0, a sample at ts=1 is *not* <= cutoff.
    Window w2(window_ns, 16, t);
    w2.on_tick(1, 10);
    w2.on_tick(500, 20);
    auto r2 = w2.on_tick(1000, 30);  // cutoff = 1000-1000 = 0
    CHECK(r2.count == 3);            // nothing evicted: ts=1 > cutoff=0
    CHECK(r2.low == 10);
}

TEST_CASE("SlidingWindowStats: out-of-order event time is clamped non-decreasing") {
    NullTracer t;
    Window w(300'000'000'000LL, 16, t);
    w.on_tick(1000, 10);
    // An earlier timestamp than the last one must not rewind the window.
    auto r = w.on_tick(500, 20);
    CHECK(r.count == 2);  // both samples present; the "effective" ts for the
                            // second sample was clamped to 1000, not 500
    // Confirm the clamp actually took effect: evicting far beyond ts=1000
    // should still evict both, proving the second sample's stored ts is
    // >= 1000, not 500.
    auto r2 = w.on_tick(1000 + 300'000'000'000LL + 1, 30);
    CHECK(r2.count == 1);  // only the newest sample remains
}

TEST_CASE("SlidingWindowStats: reset() makes a reused window behave identically to a fresh one") {
    // This is the property ticker_bench depends on: replaying the same
    // capture through a reset window multiple times must give the exact
    // same per-message results every time, without re-paying allocation
    // costs (that part isn't observable from outside, but correctness is).
    NullTracer t;
    const int64_t window_ns = 5000;
    std::mt19937 rng(55);
    std::uniform_int_distribution<int64_t> gap(0, 50);
    std::uniform_int_distribution<Price> price(0, 10);

    std::vector<std::pair<int64_t, Price>> stream;
    int64_t ts = 0;
    for (int i = 0; i < 500; ++i) {
        ts += gap(rng);
        stream.emplace_back(ts, price(rng));
    }

    Window fresh(window_ns, 64, t);
    std::vector<WindowResult> fresh_results;
    for (auto& [ets, p] : stream) fresh_results.push_back(fresh.on_tick(ets, p));

    Window reused(window_ns, 64, t);
    for (auto& [ets, p] : stream) reused.on_tick(ets, p);  // first pass, state now non-empty
    reused.reset();
    for (size_t i = 0; i < stream.size(); ++i) {
        auto r = reused.on_tick(stream[i].first, stream[i].second);
        REQUIRE(test::results_equal_exact(r, fresh_results[i]));
    }
}

TEST_CASE("SlidingWindowStats: ring grows rather than corrupting state when capacity_hint is too small") {
    NullTracer t;
    Window w(10'000'000'000LL, 4, t);  // tiny hint, will need to grow
    test::ReferenceWindow oracle(10'000'000'000LL);
    for (int64_t i = 0; i < 500; ++i) {
        auto r1 = w.on_tick(i * 1000, 100 + i);
        auto r2 = oracle.on_tick(i * 1000, 100 + i);
        REQUIRE(test::results_equal_exact(r1, r2));
    }
    CHECK(w.ring_grow_events() > 0);
}

// -- Ring<T>::back()/pop_back() -------------------------------------------
// Added for a future WindowPolicy (a quantized-histogram policy whose
// exact-low/high tracking needs a monotonic-deque pattern over a Ring<T>
// distinct from the one SlidingWindowStats itself manages) -- generic
// sliding-window machinery, tested on its own here since it doesn't depend
// on any policy.

TEST_CASE("Ring: back()/pop_back() undo push_back(), front()/pop_front() unaffected") {
    Ring<int> r(4);
    r.push_back(1);
    r.push_back(2);
    r.push_back(3);
    CHECK(r.back() == 3);
    CHECK(r.front() == 1);
    r.pop_back();
    CHECK(r.back() == 2);
    CHECK(r.size() == 2);
    r.push_back(4);
    CHECK(r.back() == 4);
    CHECK(r.front() == 1);  // pop_back() never touches the front end
    r.pop_front();
    CHECK(r.front() == 2);
    CHECK(r.back() == 4);
}

TEST_CASE("Ring: back()/pop_back() correct across the wraparound boundary") {
    Ring<int> r(4);
    // Walk head_ forward past the wrap point first.
    r.push_back(10);
    r.push_back(20);
    r.pop_front();
    r.pop_front();
    // head_ is now at index 2 (of 4) -- the next pushes wrap.
    r.push_back(100);
    r.push_back(200);
    r.push_back(300);  // wraps: occupies index 1
    CHECK(r.back() == 300);
    r.pop_back();
    CHECK(r.back() == 200);
    CHECK(r.front() == 100);
    r.push_back(400);  // wraps again
    CHECK(r.back() == 400);
    CHECK(r.size() == 3);
}

TEST_CASE("Ring: grow() with a wrapped head keeps FIFO order (front()/at()/size())") {
    Ring<int> r(4);
    for (int i = 0; i < 4; ++i) r.push_back(i);
    r.pop_front();
    r.pop_front();
    r.pop_front();      // head_ = 3, holds {3}
    r.push_back(4);
    r.push_back(5);
    r.push_back(6);     // wraps: physical layout is [4,5,6,3], head_ = 3, full
    REQUIRE(r.size() == 4);
    REQUIRE(r.grow_events() == 0);
    r.push_back(7);     // triggers grow() with the live run split in two
    CHECK(r.grow_events() == 1);
    CHECK(r.capacity() == 8);
    REQUIRE(r.size() == 5);
    CHECK(r.front() == 3);
    for (size_t i = 0; i < r.size(); ++i) CHECK(r.at(i) == static_cast<int>(3 + i));
    CHECK(r.back() == 7);
}

TEST_CASE("Ring: back()/pop_back() interleaved with push_back()/pop_front() against a std::deque oracle") {
    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> op_d(0, 2);
    Ring<int> r(4);  // small on purpose: forces real growth + wraparound
    std::deque<int> oracle;
    int next_value = 0;
    for (int trial = 0; trial < 5000; ++trial) {
        int op = oracle.empty() ? 0 : op_d(rng);  // must push if empty
        if (op == 0) {
            int v = next_value++;
            r.push_back(v);
            oracle.push_back(v);
        } else if (op == 1 && !oracle.empty()) {
            REQUIRE(r.front() == oracle.front());
            r.pop_front();
            oracle.pop_front();
        } else if (!oracle.empty()) {
            REQUIRE(r.back() == oracle.back());
            r.pop_back();
            oracle.pop_back();
        }
        REQUIRE(r.size() == oracle.size());
        if (!oracle.empty()) {
            REQUIRE(r.front() == oracle.front());
            REQUIRE(r.back() == oracle.back());
        }
    }
}
