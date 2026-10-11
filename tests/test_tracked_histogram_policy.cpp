// TrackedHistogramWindow-specific tests: Handle layout and the median cursor's fallback counter.
// The shared WindowPolicy contract (fuzz within the proven bound, etc.) is in
// test_policy_contract.cpp.
#include "doctest.h"
#include "../src/policy/tracked_histogram.hpp"
#include "../src/sliding_window.hpp"

#include <deque>
#include <random>
#include <vector>

using namespace ticker;

TEST_CASE("TrackedHistogramWindow: Handle layout") {
    static_assert(sizeof(TrackedHistogramWindow::Handle) == 8);
    using Window = SlidingWindowStats<TrackedHistogramWindow, NullTracer>;
    static_assert(sizeof(Window::Sample) == 16);
}

// -- Tracking-specific: fallback_count() behavior --------------------------
// TrackedHistogramWindow's whole point is that median() usually resolves
// without moving the cursor. fallback_count() (a diagnostic hook, not part
// of WindowPolicy) counts the calls that did move it,
// so these tests check the *mechanism*, not just the output.

TEST_CASE("TrackedHistogramWindow: the first insert parks the cursor (no search); a repeated "
          "median() call with no state change never moves it") {
    TrackedHistogramWindow w(16);
    w.insert(100);
    CHECK(w.median() == 100);
    CHECK(w.fallback_count() == 0);  // cursor parked on the first sample's bucket
    w.insert(200);
    w.insert(150);
    Price m1 = w.median();
    const uint64_t moved = w.fallback_count();
    CHECK(moved == 1);  // the median moved from 100's bucket to 150's
    Price m2 = w.median();
    CHECK(m2 == m1);
    CHECK(w.fallback_count() == moved);  // unchanged: nothing was inserted/erased between calls
}

TEST_CASE("TrackedHistogramWindow: inserting/erasing far from the median bucket triggers no "
          "new fallback (O(1) path taken)") {
    TrackedHistogramWindow w(64);
    // Realistic-magnitude, widely separated prices so "far from the
    // median" is unambiguous and doesn't accidentally land in the same
    // bucket as the median itself.
    constexpr Price base = 8'400'000;  // 84000.00 in cents
    std::vector<TrackedHistogramWindow::Handle> mid_handles;
    for (Price i = 0; i < 21; ++i) mid_handles.push_back(w.insert(base + i));  // establishes the median
    w.median();  // all 21 share one bucket, where the first insert parked the cursor
    const uint64_t after_first = w.fallback_count();
    CHECK(after_first == 0);

    // Insert/erase pairs far below the median's bucket -- a whole octave
    // down (half the price), comfortably outside the ~1.56% bucket width
    // at this magnitude, so these unambiguously land in a different
    // bucket and only ever shift count_before_med_, never touching
    // med_bucket_ itself.
    for (int i = 0; i < 5; ++i) {
        auto h = w.insert(base / 2 - i);
        w.median();
        w.erase(h);
        w.median();
    }
    CHECK(w.fallback_count() == after_first);  // the cursor never moved

    for (auto h : mid_handles) w.erase(h);
}

TEST_CASE("TrackedHistogramWindow: fallback_count() stays a small fraction of operation count "
          "on a realistic-magnitude duplicate-heavy stream") {
    // Mirrors the FIFO fuzz test's alphabet below, but specifically
    // measuring how often the O(1) path is *not* taken -- this is the
    // empirical claim (README, measured on real captures: the median's
    // bucket typically holds hundreds to thousands of live samples) made
    // executable as a regression check, not just asserted.
    std::mt19937 rng(2026);
    TrackedHistogramWindow w(2000);
    std::deque<TrackedHistogramWindow::Handle> handles;
    constexpr Price base = 8'400'000;  // 84000.00 in cents
    std::uniform_int_distribution<Price> alphabet(0, 9);  // 10 distinct nearby prices -> a shared bucket
    std::uniform_int_distribution<int> op_choice(0, 2);

    for (int step = 0; step < 5000; ++step) {
        bool do_insert = handles.empty() || op_choice(rng) != 0 || handles.size() < 20;
        if (do_insert) {
            handles.push_back(w.insert(base + alphabet(rng)));
        } else {
            w.erase(handles.front());
            handles.pop_front();
        }
        if (!handles.empty()) w.median();
    }
    // Not a tight bound (this is a real, measured property of this
    // specific input, not a universal guarantee) -- but if the O(1) path
    // were never taken, fallback_count() would be in the thousands, not a
    // handful. A generous ceiling catches a regression that breaks the
    // fast path entirely while tolerating this input's actual behavior.
    INFO("fallback_count=" << w.fallback_count());
    CHECK(w.fallback_count() < 200);
}

