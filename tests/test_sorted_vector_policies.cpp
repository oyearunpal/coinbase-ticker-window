// v6/v7-specific tests (SortedVectorWindow, SortedVectorFreqWindow) plus the detail::SortedPriceRun
// primitive they share with v8's tails. The shared WindowPolicy contract, including bit-exact fuzz
// against a brute-force median, is in test_policy_contract.cpp.
#include "doctest.h"
#include "../src/policy/sorted_price_run.hpp"
#include "../src/policy/sorted_vector.hpp"
#include "../src/policy/sorted_vector_freq.hpp"
#include "../src/sliding_window.hpp"

#include <deque>
#include <vector>

using namespace ticker;

// -- detail::SortedPriceRun ------------------------------------------------

TEST_CASE("SortedPriceRun: stays sorted through interleaved inserts and erases") {
    detail::SortedPriceRun run(4);
    for (Price p : {50, 10, 90, 30, 70, 10, 90}) run.insert(p);
    REQUIRE(run.size() == 7);
    CHECK(run.front() == 10);
    CHECK(run.back() == 90);
    for (size_t i = 1; i < run.size(); ++i) CHECK(run[i - 1] <= run[i]);

    CHECK(run.erase(10));
    CHECK(run.erase(90));
    CHECK(run.size() == 5);
    CHECK(run.front() == 10);  // the OTHER 10 is still there
    CHECK(run.back() == 90);
    for (size_t i = 1; i < run.size(); ++i) CHECK(run[i - 1] <= run[i]);
}

TEST_CASE("SortedPriceRun: erase of an absent price reports false rather than corrupting the run") {
    detail::SortedPriceRun run;
    run.insert(5);
    CHECK_FALSE(run.erase(6));
    CHECK(run.size() == 1);
    CHECK(run.front() == 5);
}

TEST_CASE("SortedPriceRun: clear() empties without releasing the reservation") {
    detail::SortedPriceRun run(128);
    for (Price p = 0; p < 100; ++p) run.insert(p);
    const size_t reserved = run.bytes();
    run.clear();
    CHECK(run.empty());
    CHECK(run.bytes() == reserved);
    CHECK(run.grow_events() == 0);  // 128 reserved, 100 inserted
}

TEST_CASE("SortedPriceRun: outgrowing the reservation is counted, not silent") {
    detail::SortedPriceRun run(4);
    for (Price p = 0; p < 64; ++p) run.insert(p);
    CHECK(run.size() == 64);
    CHECK(run.grow_events() > 0);
    for (size_t i = 1; i < run.size(); ++i) CHECK(run[i - 1] < run[i]);
}

// -- v6: SortedVectorWindow ------------------------------------------------

TEST_CASE("SortedVectorWindow: Handle is price-only, and the Sample it produces is 16 bytes") {
    static_assert(sizeof(SortedVectorWindow::Handle) == 4);
    using Window = SlidingWindowStats<SortedVectorWindow, NullTracer>;
    static_assert(sizeof(Window::Sample) == 16);
}

TEST_CASE("SortedVectorWindow: clear() keeps the reservation, and the window is reusable after reset()") {
    NullTracer t;
    SlidingWindowStats<SortedVectorWindow, NullTracer> w(1'000'000'000LL, 512, t);
    for (int i = 0; i < 400; ++i) w.on_tick(i * 1'000'000LL, 100 + i);
    const size_t reserved = w.policy().policy_bytes();
    w.reset();
    CHECK(w.size() == 0);
    CHECK(w.policy().policy_bytes() == reserved);
    auto r = w.on_tick(0, 500);
    CHECK(r.count == 1);
    CHECK(r.median == 500);
}

// -- v7: SortedVectorFreqWindow -------------------------------------------

TEST_CASE("SortedVectorFreqWindow: duplicates collapse to one entry -- the memory claim, checked") {
    SortedVectorFreqWindow w(64);
    std::vector<SortedVectorFreqWindow::Handle> hs;
    for (int i = 0; i < 100; ++i) hs.push_back(w.insert(42));
    CHECK(w.size() == 100);
    CHECK(w.distinct() == 1);
    CHECK(w.median() == 42);
    CHECK(w.low() == 42);
    CHECK(w.high() == 42);

    for (int i = 0; i < 99; ++i) w.erase(hs[static_cast<size_t>(i)]);
    CHECK(w.size() == 1);
    CHECK(w.distinct() == 1);
    w.erase(hs[99]);
    CHECK(w.empty());
    CHECK(w.distinct() == 0);
}

TEST_CASE("SortedVectorFreqWindow: even-n median whose two middles sit inside the SAME entry's run") {
    // The branch that distinguishes "successor is inside this entry" from
    // "successor is the next entry" -- both reachable, both checked.
    SortedVectorFreqWindow w(16);
    for (Price v : {10, 20, 20, 30}) w.insert(v);
    CHECK(w.median() == 20);   // ranks 1 and 2 are both the 20-entry
    CHECK(w.distinct() == 3);

    SortedVectorFreqWindow w2(16);
    for (Price v : {10, 10, 30, 30}) w2.insert(v);
    CHECK(w2.median() == 20);  // ranks 1 and 2 straddle two entries: (10+30)/2
}

TEST_CASE("SortedVectorFreqWindow: erasing the tracked entry keeps the median exact, at either end") {
    // Tracked entry in the middle: erasing it leaves the pointer on its successor.
    SortedVectorFreqWindow w(16);
    std::vector<SortedVectorFreqWindow::Handle> hs;
    for (Price v : {10, 20, 30, 40, 50}) hs.push_back(w.insert(v));
    REQUIRE(w.median() == 30);
    w.erase(hs[2]);  // 10 20 40 50, tracked entry 30 is gone
    REQUIRE(w.median() == 30);  // mean of 20 and 40
    w.erase(hs[0]);
    w.erase(hs[1]);  // 40 50
    REQUIRE(w.median() == 45);

    // Tracked entry is the LAST one (it holds most of the samples): erasing it must step the
    // pointer back onto the new last entry rather than off the end.
    SortedVectorFreqWindow w2(16);
    std::vector<SortedVectorFreqWindow::Handle> hs2;
    hs2.push_back(w2.insert(10));
    for (int i = 0; i < 3; ++i) hs2.push_back(w2.insert(20));
    REQUIRE(w2.median() == 20);  // n = 4, both middles (ranks 1 and 2) are 20, the last entry
    w2.erase(hs2[1]);
    w2.erase(hs2[2]);
    w2.erase(hs2[3]);  // only 10 left
    REQUIRE(w2.median() == 10);
    w2.erase(hs2[0]);
    REQUIRE(w2.empty());
    w2.insert(7);  // emptied then refilled: no valid pointer, scan from the front
    REQUIRE(w2.median() == 7);
}

TEST_CASE("SortedVectorFreqWindow: a median miss costs the walk distance, not the whole array") {
    SortedVectorFreqWindow w(1024);
    std::deque<SortedVectorFreqWindow::Handle> fifo;
    for (Price v = 1; v <= 500; ++v) fifo.push_back(w.insert(v));  // 500 distinct prices
    (void)w.median();
    const uint64_t before = w.median_scan_steps();
    // Slide the window up by one price: the median moves to the neighbouring entry.
    w.erase(fifo.front());
    fifo.pop_front();
    fifo.push_back(w.insert(501));
    (void)w.median();
    CHECK(w.median_scan_steps() - before <= 2);
}
