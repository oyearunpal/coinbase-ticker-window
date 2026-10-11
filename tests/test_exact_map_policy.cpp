// ExactMapWindow-specific behaviour. The shared WindowPolicy contract (odd/even/duplicates, fuzz
// against a brute-force median, SlidingWindowStats parity) is in test_policy_contract.cpp.
#include "doctest.h"
#include "../src/policy/exact_map.hpp"

#include <vector>

using namespace ticker;

TEST_CASE("ExactMapWindow: erasing the tracked entry's last sample moves the median correctly") {
    ExactMapWindow w(16);
    auto h1 = w.insert(10);
    auto h2 = w.insert(20);
    auto h3 = w.insert(30);
    CHECK(w.median() == 20);  // tracks the 20 entry
    w.erase(h2);              // tracked entry vanishes; successor (30) takes over
    CHECK(w.median() == 20);  // (10 + 30) / 2
    w.erase(h1);
    CHECK(w.median() == 30);
    auto h4 = w.insert(40);
    CHECK(w.median() == 35);  // tracks 30, the lower middle
    w.erase(h4);
    CHECK(w.median() == 30);
    w.erase(h3);  // tracked entry is also the last key: the pointer must not dangle
    CHECK(w.empty());
    w.insert(7);
    CHECK(w.median() == 7);
    CHECK(w.distinct() == 1);
}

TEST_CASE("ExactMapWindow: equal prices share one map entry") {
    ExactMapWindow w(16);
    std::vector<ExactMapWindow::Handle> hs;
    for (int i = 0; i < 12; ++i) hs.push_back(w.insert(100 + i % 3));
    CHECK(w.size() == 12);
    CHECK(w.distinct() == 3);
    for (int i = 0; i < 3; ++i) w.erase(hs[static_cast<size_t>(i)]);  // one of each price
    CHECK(w.distinct() == 3);
    CHECK(hs[3] == hs[0]);  // same price, same node: the Handle is the entry
}
