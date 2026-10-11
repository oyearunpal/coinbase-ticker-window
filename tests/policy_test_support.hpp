#pragma once
// Shared by the policy tests: an independent median, the histogram policies' proven error bound,
// result comparison, and one "case" struct per policy configuration for test_policy_contract.cpp.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "../src/fixed_price.hpp"
#include "../src/policy/exact_map.hpp"
#include "../src/policy/hybrid_histogram.hpp"
#include "../src/policy/price_bucketing.hpp"
#include "../src/policy/range_histogram.hpp"
#include "../src/policy/sorted_vector.hpp"
#include "../src/policy/sorted_vector_freq.hpp"
#include "../src/policy/tracked_histogram.hpp"
#include "../src/sliding_window.hpp"
#include "doctest.h"

namespace ticker::test {

// Exact median, an even-count half cent rounded up, computed independently of detail::avg_price.
inline Price naive_median(std::vector<Price> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : static_cast<Price>((uint64_t{v[n / 2 - 1]} + v[n / 2] + 1) / 2);
}

// Proven relative median error of the global PriceBucketing scheme (see price_bucketing.hpp's
// bucket_midpoint), which tracked_histogram and every fallback mode use.
inline constexpr double kHistogramMaxRelErr =
    1.0 / (2.0 * static_cast<double>(detail::PriceBucketing::kSubCount) - 1.0);

// tol == 0: exact equality. Otherwise within tol of `exact`, plus one cent: an even-count median
// averages two values, and the policy and the oracle can round that half cent differently.
inline bool median_ok(Price reported, Price exact, double tol) {
    if (tol == 0.0) return reported == exact;
    const double diff = std::abs(static_cast<double>(reported) - static_cast<double>(exact));
    return diff <= static_cast<double>(exact) * tol + 1.0 + 1e-9;
}

// mean/low/high/count exact; median exact or within median_tol.
inline bool results_match(const WindowResult& a, const WindowResult& b, double median_tol) {
    return a.mean == b.mean && median_ok(a.median, b.median, median_tol) && a.low == b.low && a.high == b.high &&
           a.count == b.count;
}

inline bool results_equal_exact(const WindowResult& a, const WindowResult& b) { return results_match(a, b, 0.0); }

// on_tick() takes a lazy BandFn (see sliding_window.hpp); this wraps an already-known band.
inline auto fixed_band(Price low, Price high) {
    return [low, high]() noexcept { return std::pair<std::optional<Price>, std::optional<Price>>{low, high}; };
}

// -- Policy cases -----------------------------------------------------------
// Every case draws prices from [kBase, kBase + kSpan). Cases with kUsesBand establish that range as
// the 24h band (setup() at policy level, fixed_band on the first tick at window level), so the
// range/hybrid policies run in their exact mode. kArbitraryErase: erase() order need not be FIFO
// (the histogram policies' FifoMinMax low/high assume FIFO eviction).
inline constexpr Price kBase = 8'400'000;  // $84,000.00
inline constexpr Price kSpan = 500'000;    // $5,000

template <typename P, double Tol, bool ArbitraryErase, bool UsesBand>
struct PolicyCase {
    using Policy = P;
    static constexpr double kMedianTol = Tol;
    static constexpr bool kArbitraryErase = ArbitraryErase;
    static constexpr bool kUsesBand = UsesBand;
    static void setup(Policy& p) {
        if constexpr (UsesBand) p.note_market(kBase, kBase + kSpan);
        (void)p;
    }
};

struct ExactMapCase : PolicyCase<ExactMapWindow, 0.0, true, false> {};
struct SortedVectorCase : PolicyCase<SortedVectorWindow, 0.0, true, false> {};
struct SortedVectorFreqCase : PolicyCase<SortedVectorFreqWindow, 0.0, true, false> {};
struct TrackedHistogramCase : PolicyCase<TrackedHistogramWindow, kHistogramMaxRelErr, false, false> {};
struct RangeExactCase : PolicyCase<RangeHistogramWindow, 0.0, false, true> {};
struct RangeFallbackCase : PolicyCase<RangeHistogramWindow, kHistogramMaxRelErr, false, false> {};
struct RangeNoRebuildExactCase : PolicyCase<RangeHistogramNoRebuildWindow, 0.0, false, true> {};
struct HybridCase : PolicyCase<HybridHistogramWindow, 0.0, false, true> {};

}  // namespace ticker::test

TYPE_TO_STRING(ticker::test::ExactMapCase);
TYPE_TO_STRING(ticker::test::SortedVectorCase);
TYPE_TO_STRING(ticker::test::SortedVectorFreqCase);
TYPE_TO_STRING(ticker::test::TrackedHistogramCase);
TYPE_TO_STRING(ticker::test::RangeExactCase);
TYPE_TO_STRING(ticker::test::RangeFallbackCase);
TYPE_TO_STRING(ticker::test::RangeNoRebuildExactCase);
TYPE_TO_STRING(ticker::test::HybridCase);

#define TICKER_ALL_POLICY_CASES                                                                            \
    ticker::test::ExactMapCase, ticker::test::SortedVectorCase, ticker::test::SortedVectorFreqCase,  \
        ticker::test::TrackedHistogramCase, ticker::test::RangeExactCase,                               \
        ticker::test::RangeFallbackCase, ticker::test::RangeNoRebuildExactCase, ticker::test::HybridCase

#define TICKER_ARBITRARY_ERASE_CASES \
    ticker::test::ExactMapCase, ticker::test::SortedVectorCase, ticker::test::SortedVectorFreqCase
