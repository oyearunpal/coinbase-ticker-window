#pragma once
// Brute-force O(n)-per-tick oracle for the sliding window stats, used only
// by tests to independently verify SlidingWindowStats's incremental
// algorithms (the median-tracking multiset, ring eviction) against a
// straightforward re-derivation from scratch on every tick: keep every
// live sample in a plain deque, evict with the same clamp+cutoff rule, and
// recompute mean/median/low/high by sorting and scanning. Deliberately
// simple and obviously correct rather than fast — that's the whole point
// of an oracle.
#include <algorithm>
#include <cassert>
#include <deque>
#include <vector>

#include "../src/fixed_price.hpp"
#include "../src/sliding_window.hpp"  // for WindowResult

namespace ticker::test {

class ReferenceWindow {
  public:
    explicit ReferenceWindow(int64_t window_ns) : window_ns_(window_ns) {}

    WindowResult on_tick(int64_t event_ts_ns, Price price) {
        if (event_ts_ns < last_ts_) {
            event_ts_ns = last_ts_;
        } else {
            last_ts_ = event_ts_ns;
        }

        const int64_t cutoff = event_ts_ns - window_ns_;
        while (!samples_.empty() && samples_.front().first <= cutoff) {
            samples_.pop_front();
        }
        samples_.emplace_back(event_ts_ns, price);

        std::vector<Price> prices;
        prices.reserve(samples_.size());
        __int128 sum = 0;
        for (auto& [ts, px] : samples_) {
            prices.push_back(px);
            sum += px;
        }
        std::sort(prices.begin(), prices.end());

        const size_t n = prices.size();
        WindowResult r;
        r.count = n;
        r.low = prices.front();
        r.high = prices.back();
        // Both in cents, rounded half-up: floor(x + 1/2).
        if (n % 2 == 1) {
            r.median = prices[n / 2];
        } else {
            r.median = static_cast<Price>((static_cast<__int128>(prices[n / 2 - 1]) + prices[n / 2] + 1) / 2);
        }
        const __int128 n128 = static_cast<__int128>(n);
        r.mean = static_cast<Price>((2 * sum + n128) / (2 * n128));
        return r;
    }

    size_t size() const noexcept { return samples_.size(); }

  private:
    int64_t window_ns_;
    int64_t last_ts_ = INT64_MIN;
    std::deque<std::pair<int64_t, Price>> samples_;
};

}  // namespace ticker::test
