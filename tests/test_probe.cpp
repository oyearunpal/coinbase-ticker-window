#include "doctest.h"
#include "probe.hpp"
#include "latency_hist.hpp"
#include "policy/exact_map.hpp"
#include "sliding_window.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace ticker;

TEST_CASE("LatencyHist: exact (1 ns buckets) through 4094 ns") {
    // x = v + 1 is what gets bucketed; the linear region is x < 2048, and the
    // next octave is still 1 ns wide, so every v below 4095 has its own bucket.
    for (uint64_t v = 0; v < 4095; ++v) {
        size_t b = LatencyHist::bucket_of(v + 1);
        CHECK(LatencyHist::bucket_lower_bound(b) == v);
    }
}

TEST_CASE("LatencyHist: bucket lower bound never overstates the value") {
    for (uint64_t v = 0; v < 5'000'000; v += 977) {
        size_t b = LatencyHist::bucket_of(v + 1);
        CHECK(LatencyHist::bucket_lower_bound(b) <= v);
    }
}

TEST_CASE("LatencyHist: relative bucketing error stays within the documented 1/2048 bound") {
    double max_rel_err = 0;
    for (uint64_t v = 4096; v < (1ULL << 40); v = v + v / 6 + 1) {
        size_t b = LatencyHist::bucket_of(v + 1);
        uint64_t lo = LatencyHist::bucket_lower_bound(b);
        max_rel_err = std::max(max_rel_err, double(v - lo) / double(v));
    }
    CHECK(max_rel_err <= 1.0 / 2048);
}

TEST_CASE("LatencyHist: percentiles track a sorted reference within the bucketing bound") {
    std::mt19937 rng(42);
    std::vector<uint64_t> samples;
    LatencyHist h;
    for (int i = 0; i < 200'000; ++i) {
        uint64_t v = static_cast<uint64_t>(std::exp(std::uniform_real_distribution<double>(0, 16)(rng)));
        samples.push_back(v);
        h.record(v);
    }
    std::sort(samples.begin(), samples.end());

    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * samples.size()));
        if (rank == 0) rank = 1;
        uint64_t ref = samples[rank - 1];
        uint64_t got = h.percentile(p);
        CHECK(got <= ref);  // never overstates
        double rel = ref > 0 ? double(ref - got) / double(ref) : 0.0;
        CHECK(rel <= 1.0 / 2048);
    }
}

TEST_CASE("LatencyHist: min/max/mean/count track exactly regardless of bucketing") {
    LatencyHist h;
    h.record(5);
    h.record(1000);
    h.record(3);
    CHECK(h.count() == 3);
    CHECK(h.min() == 3);
    CHECK(h.max() == 1000);
    CHECK(h.sum() == 1008);
}

TEST_CASE("NullTracer: enter() returns a Scope that is empty (zero runtime state)") {
    NullTracer t;
    static_assert(std::is_empty_v<NullTracer::Scope>, "NullTracer::Scope must be empty for zero-cost");
    {
        auto scope = t.enter(ProbeId::ParseTotal);
        (void)scope;
    }
    CHECK(true);  // reaching here without any recorded state is the point
}

TEST_CASE("TscTracer: records into the correct probe's histogram") {
    TscTracer t;
    CHECK(t.histogram(ProbeId::ParseTotal).count() == 0);
    {
        auto scope = t.enter(ProbeId::ParseTotal);
        (void)scope;
        volatile int x = 0;
        for (int i = 0; i < 1000; ++i) x += i;
        (void)x;
    }
    CHECK(t.histogram(ProbeId::ParseTotal).count() == 1);
    CHECK(t.histogram(ProbeId::SymbolLookup).count() == 0);  // other probes untouched
}

TEST_CASE("TscTracer: note() records counters into the same histogram machinery") {
    TscTracer t;
    t.note(ProbeId::WinEvictCount, 3);
    t.note(ProbeId::WinEvictCount, 7);
    CHECK(t.histogram(ProbeId::WinEvictCount).count() == 2);
    CHECK(t.histogram(ProbeId::WinEvictCount).max() == 7);
}

TEST_CASE("TscTracer: probe-overhead self-calibration lands in a plausible, bounded range") {
    // Deliberately loose bounds -- this is a real hardware measurement
    // (rdtscp / CNTVCT round trips), not a fixed constant, so it's machine-
    // and load-dependent. The point is to catch a calibration that's badly
    // broken (e.g. something in the microsecond range), not to pin an exact
    // nanosecond figure. On aarch64 an empty Scope can round to 0 ns because
    // CNTVCT is coarser than a couple of arithmetic instructions; parent_ns
    // (outer-bracketed) still has to be strictly positive.
    TscTracer t;
    CHECK(t.probe_overhead_self_ns() >= 0.0);
    CHECK(t.probe_overhead_self_ns() < 1000.0);
    CHECK(t.probe_overhead_parent_ns() > 0.0);
    CHECK(t.probe_overhead_parent_ns() < 1000.0);
    // Calibration must not itself pollute any *real* probe's histogram --
    // it only ever writes to ProbeOverhead's own bucket.
    CHECK(t.histogram(ProbeId::ParseTotal).count() == 0);
    CHECK(t.histogram(ProbeId::FeedTotal).count() == 0);
    CHECK(t.histogram(ProbeId::ProbeOverhead).count() > 0);
}

TEST_CASE("SlidingWindowStats+TscTracer: WinEvict (duration) and WinEvictCount (eviction "
          "count) land in separate histograms, never conflated") {
    // Regression test: these two used to share one ProbeId, silently
    // mixing nanosecond durations with eviction counts into one set of
    // percentiles (caught via an oddly-doubled sample count and a
    // nonsensical "mean evictions/tick" during benchmark dry runs).
    TscTracer t;
    SlidingWindowStats<ExactMapWindow, TscTracer> w(1000, 64, t);

    // Insert several samples with no eviction (window never exceeds length),
    // then a burst of samples timed to force real evictions.
    for (int64_t i = 0; i < 10; ++i) w.on_tick(i, 100 + i);
    for (int64_t i = 0; i < 10; ++i) w.on_tick(10'000 + i, 200 + i);  // forces evictions

    const LatencyHist& duration_hist = t.histogram(ProbeId::WinEvict);
    const LatencyHist& count_hist = t.histogram(ProbeId::WinEvictCount);

    // One duration sample and one count sample per on_tick call -- same
    // call count, but they must NOT be the same histogram object/values.
    CHECK(duration_hist.count() == 20);
    CHECK(count_hist.count() == 20);
    // The count histogram must actually reflect eviction counts (small
    // integers, at most ~10 here), not raw nanosecond durations (which
    // would be at least tens of ns even for a no-op scope).
    CHECK(count_hist.max() <= 10);
    // Some ticks must have evicted >0 (the second burst), so it's not
    // trivially all-zero either.
    CHECK(count_hist.sum() > 0);
}
