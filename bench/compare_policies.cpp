// compare_policies: replays a capture through every WindowPolicy side by side (same symbols, same
// window) and reports each policy's deviation from ExactMapWindow, the exact reference. It turns
// the proven error bounds into measured numbers on real market data.
//
//   tracked_histogram            median within PriceBucketing's proven relative bound; mean/low/high
//                                must be exact. Also reports how often the median cursor moved.
//   range_histogram (v4)         per symbol: exact (one cent per bucket) must be zero error; fallback
//                                (no usable 24h band) within the proven bound; a band over the
//                                bucket cap is reported, not gated.
//   range_histogram_no_rebuild   as v4, but an escaping price is an accepted error, so its exact
//                                mode is reported, not gated; rebuild_count() must stay 0.
//   sorted_vector, _freq         bit-exact: any median deviation is a failure.
//   hybrid_histogram (v8)        as v4; exact mode is gated (escapes go to exact tails). Also
//                                reports re-centres and tail usage.
//
// Every window uses NullTracer, as ticker_feed does; this measures accuracy, not latency.
//
// Usage: build/compare_policies <capture> [window_s]
#include "../src/frame_capture.hpp"
#include "../src/policy/exact_map.hpp"
#include "../src/policy/hybrid_histogram.hpp"
#include "../src/policy/range_histogram.hpp"
#include "../src/policy/sorted_vector.hpp"
#include "../src/policy/sorted_vector_freq.hpp"
#include "../src/policy/tracked_histogram.hpp"
#include "../src/sliding_window.hpp"
#include "../src/symbol_registry.hpp"
#include "../src/ticker_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ticker;

namespace {
// Matches PriceBucketingT's own documented bound (see
// price_bucketing.hpp's bucket_midpoint comment): 1/(2*kSubCount - 1).
// Both histogram-based policies share PriceBucketing (SubBits=6), so the
// same bound applies to both.
constexpr double kProvenMedianBound =
    1.0 / (2.0 * static_cast<double>(detail::PriceBucketing::kSubCount) - 1.0);

// Accuracy accounting for one non-reference policy, updated per tick
// against ExactMapWindow's result for the same tick.
struct Accuracy {
    uint64_t mean_mismatches = 0, low_mismatches = 0, high_mismatches = 0;
    double max_rel_err = 0.0, sum_rel_err = 0.0;
    Price max_abs_err = 0;

    void observe(const WindowResult& reference, const WindowResult& got) {
        if (reference.mean != got.mean) ++mean_mismatches;
        if (reference.low != got.low) ++low_mismatches;
        if (reference.high != got.high) ++high_mismatches;
        if (reference.median != 0) {
            const Price diff = (got.median > reference.median) ? got.median - reference.median
                                                               : reference.median - got.median;
            const double rel = static_cast<double>(diff) / static_cast<double>(reference.median);
            max_rel_err = std::max(max_rel_err, rel);
            sum_rel_err += rel;
            max_abs_err = std::max(max_abs_err, diff);
        }
    }

    bool exact_claim_violated() const { return mean_mismatches || low_mismatches || high_mismatches; }
    bool bound_violated() const { return max_rel_err > kProvenMedianBound; }

    void print(const char* name, uint64_t ticks) const {
        printf("-- %s vs exact_map --\n", name);
        printf("  mean/low/high mismatches: %llu / %llu / %llu (must all be 0 -- claimed exact)\n",
               static_cast<unsigned long long>(mean_mismatches), static_cast<unsigned long long>(low_mismatches),
               static_cast<unsigned long long>(high_mismatches));
        printf("  median relative error: max=%.6f%% (%.4fx the proven %.6f%% bound) mean=%.6f%% max_abs=%u cents\n",
               max_rel_err * 100.0, max_rel_err / kProvenMedianBound, kProvenMedianBound * 100.0,
               ticks > 0 ? sum_rel_err / static_cast<double>(ticks) * 100.0 : 0.0,
               max_abs_err);
    }
};

// RangeHistogramWindow's accuracy claim is per-symbol, not one global
// bound (see this file's header comment), so ticks are split into three
// disjoint accounting buckets depending on what THAT tick's own window
// currently is: exact-mode (using_range_bucketing() && exact()),
// fallback-mode (!using_range_bucketing(), i.e. the global PriceBucketing
// scheme v3 also uses), and capped-range (using_range_bucketing() &&
// !exact() -- a band too wide for the cent-per-bucket cap). Split by
// CURRENT state rather than fixed per-symbol, because a symbol really can
// move between modes mid-capture: v8 re-derives its bucket width at every
// re-centre, so a symbol whose live span outgrows the core cap leaves
// exact mode and can return to it later, and these counters follow it
// tick by tick rather than pinning it to whatever it was first.
//
// Reused unchanged for v8 (HybridHistogramWindow), whose three modes are
// the same three: an exact one-cent-per-bucket core, the same global
// fallback for a symbol with no usable market data, and a capped core
// whose buckets had to widen to cover a live span bigger than the cap.
//
// `strict_exact` distinguishes v4 from v5 (RangeHistogramNoRebuildWindow):
// for v4, "exact-mode" ticks are provably zero-error, because
// would_escape() rebuilds BEFORE a price can land outside the exact band
// -- a nonzero error there is a real bug, hard-failed via
// exact_median_violated(). For v5, would_escape() always returns false,
// so an escaping price on an "exact-mode" (width()==1) symbol lands
// in the coarse tail bucket instead of triggering a rebuild -- that
// symbol's band is still exact-resolution, but THIS tick's own median can
// legitimately deviate. That deviation is the accepted error v5 trades
// for skipping the rebuild, not a bug, so strict_exact=false makes
// exact_median_violated() report informationally rather than gate.
struct RangeAccuracy {
    explicit RangeAccuracy(bool strict_exact = true) : strict_exact_(strict_exact) {}

    Accuracy exact_acc, fallback_acc, capped_acc;
    uint64_t exact_ticks = 0, fallback_ticks = 0, capped_ticks = 0;

    void observe(const WindowResult& reference, const WindowResult& got, bool using_range, bool exact) {
        if (using_range && exact) {
            exact_acc.observe(reference, got);
            ++exact_ticks;
        } else if (!using_range) {
            fallback_acc.observe(reference, got);
            ++fallback_ticks;
        } else {
            capped_acc.observe(reference, got);
            ++capped_ticks;
        }
    }

    // Only the two buckets with a PROVEN bound are gates; capped_acc has
    // none (see this file's header comment), so it can never fail this
    // check regardless of what it observes. mean/low/high are NEVER
    // traded away by EnableRebuild=false (the monotonic-deque/sum logic
    // doesn't depend on bucketing at all), so this stays a hard gate for
    // both v4 and v5 regardless of strict_exact.
    bool exact_claim_violated() const {
        return exact_acc.exact_claim_violated() || fallback_acc.exact_claim_violated() ||
               capped_acc.exact_claim_violated();
    }
    // v4 (strict_exact=true): exact_acc's median must be LITERALLY zero
    // error -- that is the whole claim being tested, not "within some
    // bound", so any deviation is a real bug. v5 (strict_exact=false):
    // never gates here -- see this struct's header comment.
    bool exact_median_violated() const { return strict_exact_ && exact_acc.max_abs_err != 0; }
    bool fallback_bound_violated() const { return fallback_acc.bound_violated(); }

    void print(const char* name, uint64_t ticks) const {
        printf("-- %s vs exact_map --\n", name);
        printf("  ticks by mode: exact=%llu fallback=%llu capped=%llu (of %llu total)\n",
               static_cast<unsigned long long>(exact_ticks), static_cast<unsigned long long>(fallback_ticks),
               static_cast<unsigned long long>(capped_ticks), static_cast<unsigned long long>(ticks));
        if (exact_ticks > 0) {
            printf("  exact-mode mean/low/high mismatches: %llu / %llu / %llu (must all be 0)\n",
                   static_cast<unsigned long long>(exact_acc.mean_mismatches),
                   static_cast<unsigned long long>(exact_acc.low_mismatches),
                   static_cast<unsigned long long>(exact_acc.high_mismatches));
            if (strict_exact_) {
                printf("  exact-mode median max_abs_err: %u cents (must be exactly 0 -- this IS the claim)\n",
                       exact_acc.max_abs_err);
            } else {
                printf("  exact-mode median relative error: max=%.6f%% mean=%.6f%% max_abs=%u cents "
                       "(informational -- this is the accepted error from skipping the rebuild on an escape; "
                       "see this file's header comment)\n",
                       exact_acc.max_rel_err * 100.0,
                       exact_ticks > 0 ? exact_acc.sum_rel_err / static_cast<double>(exact_ticks) * 100.0 : 0.0,
                       exact_acc.max_abs_err);
            }
        }
        if (fallback_ticks > 0) fallback_acc.print((std::string(name) + " (fallback-mode ticks)").c_str(), fallback_ticks);
        if (capped_ticks > 0) {
            printf("  capped-range-mode median relative error: max=%.6f%% mean=%.6f%% (informational only -- "
                   "no proven bound applies to this mode; see the policy's own header for what a capped "
                   "bucket means there)\n",
                   capped_acc.max_rel_err * 100.0,
                   capped_ticks > 0 ? capped_acc.sum_rel_err / static_cast<double>(capped_ticks) * 100.0 : 0.0);
        }
    }

  private:
    bool strict_exact_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capture> [window_s=300]\n", argv[0]);
        return 1;
    }
    const std::string capture_path = argv[1];
    int64_t window_s = 300;
    if (argc >= 3) window_s = std::stoll(argv[2]);
    const int64_t window_ns = window_s * 1'000'000'000LL;

    SymbolRegistry registry;
    std::vector<std::unique_ptr<SlidingWindowStats<ExactMapWindow, NullTracer>>> exact_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<TrackedHistogramWindow, NullTracer>>> tracked_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<RangeHistogramWindow, NullTracer>>> range_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<RangeHistogramNoRebuildWindow, NullTracer>>> range_v5_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<SortedVectorWindow, NullTracer>>> sortvec_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<SortedVectorFreqWindow, NullTracer>>> sortfreq_windows;
    std::vector<std::unique_ptr<SlidingWindowStats<HybridHistogramWindow, NullTracer>>> hybrid_windows;
    NullTracer tracer;  // same tracer type ticker_feed itself uses -- zero rdtscp, this isn't a latency test

    FrameCaptureReader reader(capture_path);
    std::string raw;
    int64_t recv_ts_ns;
    uint64_t ticks = 0;
    Accuracy tracked_acc;
    RangeAccuracy range_acc(/*strict_exact=*/true);
    RangeAccuracy range_v5_acc(/*strict_exact=*/false);
    Accuracy sortvec_acc, sortfreq_acc;
    RangeAccuracy hybrid_acc(/*strict_exact=*/true);
    uint64_t total_fallbacks = 0, total_rebuilds = 0, total_escapes = 0;
    uint64_t total_rebuilds_v5 = 0, total_escapes_v5 = 0;
    uint64_t total_recenters = 0, total_tail_inserts = 0, total_hybrid_fallbacks = 0;
    size_t max_tail_high_water = 0;
    uint64_t total_scan_steps = 0;
    uint64_t total_sortfreq_fallbacks = 0;
    // v7's memory bet, read off the END state of every window (the fullest
    // each one gets in this single pass): total live samples vs total
    // distinct live prices. See sorted_vector_freq.hpp's header.
    uint64_t end_samples = 0, end_distinct = 0;

    while (reader.next_frame(raw, recv_ts_ns)) {
        auto t = parse_ticker(raw, tracer);
        if (!t) continue;

        int slot = registry.find(t->product_id);
        if (slot == SymbolRegistry::kNotFound) {
            slot = registry.add(t->product_id);
            exact_windows.resize(registry.size());
            tracked_windows.resize(registry.size());
            range_windows.resize(registry.size());
            range_v5_windows.resize(registry.size());
            sortvec_windows.resize(registry.size());
            sortfreq_windows.resize(registry.size());
            hybrid_windows.resize(registry.size());
        }
        if (!exact_windows[static_cast<size_t>(slot)]) {
            exact_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<ExactMapWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            tracked_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<TrackedHistogramWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            range_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<RangeHistogramWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            range_v5_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<RangeHistogramNoRebuildWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            sortvec_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<SortedVectorWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            sortfreq_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<SortedVectorFreqWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
            hybrid_windows[static_cast<size_t>(slot)] =
                std::make_unique<SlidingWindowStats<HybridHistogramWindow, NullTracer>>(window_ns, kCapacityHint, tracer);
        }
        const WindowResult re = exact_windows[static_cast<size_t>(slot)]->on_tick(t->event_ts_ns, t->price);
        const WindowResult rt = tracked_windows[static_cast<size_t>(slot)]->on_tick(t->event_ts_ns, t->price);
        auto& rw = range_windows[static_cast<size_t>(slot)];
        const WindowResult rr =
            rw->on_tick(t->event_ts_ns, t->price, [&]() noexcept { return std::pair{t->low_24h(), t->high_24h()}; });
        auto& rw5 = range_v5_windows[static_cast<size_t>(slot)];
        const WindowResult rr5 = rw5->on_tick(t->event_ts_ns, t->price,
                                               [&]() noexcept { return std::pair{t->low_24h(), t->high_24h()}; });
        const WindowResult rsv = sortvec_windows[static_cast<size_t>(slot)]->on_tick(t->event_ts_ns, t->price);
        auto& sf = sortfreq_windows[static_cast<size_t>(slot)];
        const WindowResult rsf = sf->on_tick(t->event_ts_ns, t->price);
        auto& hy = hybrid_windows[static_cast<size_t>(slot)];
        const WindowResult rhy =
            hy->on_tick(t->event_ts_ns, t->price, [&]() noexcept { return std::pair{t->low_24h(), t->high_24h()}; });
        ++ticks;

        tracked_acc.observe(re, rt);
        range_acc.observe(re, rr, rw->policy().using_range_bucketing(), rw->policy().exact());
        range_v5_acc.observe(re, rr5, rw5->policy().using_range_bucketing(), rw5->policy().exact());
        sortvec_acc.observe(re, rsv);
        sortfreq_acc.observe(re, rsf);
        hybrid_acc.observe(re, rhy, hy->policy().using_core_bucketing(), hy->policy().exact());
    }

    for (auto& w : tracked_windows) {
        if (w) total_fallbacks += w->policy().fallback_count();
    }
    for (auto& w : range_windows) {
        if (w) {
            total_rebuilds += w->policy().rebuild_count();
            total_escapes += w->policy().escape_count();
        }
    }
    for (auto& w : range_v5_windows) {
        if (w) {
            total_rebuilds_v5 += w->policy().rebuild_count();
            total_escapes_v5 += w->policy().escape_count();
        }
    }
    for (auto& w : sortfreq_windows) {
        if (w) {
            total_scan_steps += w->policy().median_scan_steps();
            total_sortfreq_fallbacks += w->policy().fallback_count();
            end_samples += w->policy().size();
            end_distinct += w->policy().distinct();
        }
    }
    for (auto& w : hybrid_windows) {
        if (w) {
            total_recenters += w->policy().recenter_count();
            total_tail_inserts += w->policy().tail_inserts();
            total_hybrid_fallbacks += w->policy().fallback_count();
            max_tail_high_water = std::max(max_tail_high_water, w->policy().max_tail_size());
        }
    }

    printf("capture=%s window=%llds ticks=%llu symbols=%zu\n", capture_path.c_str(),
           static_cast<long long>(window_ns / 1'000'000'000LL), static_cast<unsigned long long>(ticks),
           registry.size());
    tracked_acc.print("tracked_histogram", ticks);
    printf("-- tracked_histogram cursor moves --\n");
    printf("  moves=%llu / ticks=%llu (%.4f%% of median() calls stepped the cursor to another bucket)\n",
           static_cast<unsigned long long>(total_fallbacks), static_cast<unsigned long long>(ticks),
           ticks > 0 ? 100.0 * static_cast<double>(total_fallbacks) / static_cast<double>(ticks) : 0.0);
    range_acc.print("range_histogram", ticks);
    printf("-- range_histogram rebuild/escape counts --\n");
    printf("  rebuilds=%llu escapes=%llu (of %llu ticks -- both expected near 0 on real captures)\n",
           static_cast<unsigned long long>(total_rebuilds), static_cast<unsigned long long>(total_escapes),
           static_cast<unsigned long long>(ticks));

    range_v5_acc.print("range_histogram_no_rebuild", ticks);
    printf("-- range_histogram_no_rebuild rebuild/escape counts --\n");
    printf("  rebuilds=%llu (must be 0 -- this policy never rebuilds by design) escapes=%llu (of %llu ticks -- "
           "each one is an accepted error, traded for skipping the rebuild v4 would have done here)\n",
           static_cast<unsigned long long>(total_rebuilds_v5), static_cast<unsigned long long>(total_escapes_v5),
           static_cast<unsigned long long>(ticks));

    sortvec_acc.print("sorted_vector", ticks);
    printf("  (claims bit-exact, like exact_map itself -- max_abs_err above must be exactly 0)\n");
    sortfreq_acc.print("sorted_vector_freq", ticks);
    printf("  (claims bit-exact, like exact_map itself -- max_abs_err above must be exactly 0)\n");
    printf("-- sorted_vector_freq: the two halves of its design tradeoff, measured --\n");
    printf("  duplicate collapse at end of capture: %llu live samples over %llu distinct prices (%.2fx)"
           " -- entries shifted per insert scale with the DISTINCT count\n",
           static_cast<unsigned long long>(end_samples), static_cast<unsigned long long>(end_distinct),
           end_distinct > 0 ? static_cast<double>(end_samples) / static_cast<double>(end_distinct) : 0.0);
    printf("  median walk: %llu entries stepped over %llu median() calls (%.1f entries/call average)"
           " -- the cost of moving the tracked pointer\n",
           static_cast<unsigned long long>(total_scan_steps), static_cast<unsigned long long>(ticks),
           ticks > 0 ? static_cast<double>(total_scan_steps) / static_cast<double>(ticks) : 0.0);
    printf("  fallbacks=%llu / ticks=%llu (%.4f%% of median() calls missed the O(1) pointer;"
           " %.1f entries/fallback average)\n",
           static_cast<unsigned long long>(total_sortfreq_fallbacks), static_cast<unsigned long long>(ticks),
           ticks > 0 ? 100.0 * static_cast<double>(total_sortfreq_fallbacks) / static_cast<double>(ticks) : 0.0,
           total_sortfreq_fallbacks > 0
               ? static_cast<double>(total_scan_steps) / static_cast<double>(total_sortfreq_fallbacks)
               : 0.0);

    hybrid_acc.print("hybrid_histogram", ticks);
    printf("-- hybrid_histogram core tracking --\n");
    printf("  re-centres=%llu (one whole-window replay each) tail_inserts=%llu (%.4f%% of ticks landed outside "
           "the core) max_tail_high_water=%zu (cap 64) median_cursor_misses=%llu\n",
           static_cast<unsigned long long>(total_recenters), static_cast<unsigned long long>(total_tail_inserts),
           ticks > 0 ? 100.0 * static_cast<double>(total_tail_inserts) / static_cast<double>(ticks) : 0.0,
           max_tail_high_water, static_cast<unsigned long long>(total_hybrid_fallbacks));

    const bool failed = tracked_acc.exact_claim_violated() || tracked_acc.bound_violated() ||
                         range_acc.exact_claim_violated() || range_acc.exact_median_violated() ||
                         range_acc.fallback_bound_violated() ||
                         range_v5_acc.exact_claim_violated() || range_v5_acc.fallback_bound_violated() ||
                         total_rebuilds_v5 != 0 ||  // the one hard invariant specific to v5: it must never rebuild
                         // v6/v7 claim exactness on ALL FOUR stats, so both
                         // the mismatch counters and the median error are
                         // hard zero gates -- no bound is involved at all.
                         sortvec_acc.exact_claim_violated() || sortvec_acc.max_abs_err != 0 ||
                         sortfreq_acc.exact_claim_violated() || sortfreq_acc.max_abs_err != 0 ||
                         hybrid_acc.exact_claim_violated() || hybrid_acc.exact_median_violated() ||
                         hybrid_acc.fallback_bound_violated();
    if (failed) fprintf(stderr, "FAIL: an exactness claim or a proven median bound was violated\n");
    return failed ? 1 : 0;
}
