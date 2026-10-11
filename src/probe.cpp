#include "probe.hpp"

#include <chrono>

namespace ticker {

namespace {
// Long enough for a stable cycles/ns ratio, short enough to not slow startup.
constexpr auto kCalibrationPeriod = std::chrono::milliseconds(5);
// Enough samples for a stable median at ~20 ns per scope.
constexpr int kProbeOverheadSamples = 10'000;
// Untimed warm-up scopes (the first counter reads can be slower).
constexpr int kWarmupScopes = 8;
}  // namespace

double TscTracer::calibrate() noexcept {
    using clock = std::chrono::steady_clock;
    // Warm up the TSC read path once (first read can be slower).
    (void)read_timestamp_counter();

    const auto wall_start = clock::now();
    const uint64_t tsc_start = read_timestamp_counter();

    while (clock::now() - wall_start < kCalibrationPeriod) {
        (void)read_timestamp_counter();
    }

    const uint64_t tsc_end = read_timestamp_counter();
    const auto wall_end = clock::now();

    const double wall_ns =
        std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const uint64_t cycles = tsc_end - tsc_start;
    if (cycles == 0) return 1.0;
    return wall_ns / static_cast<double>(cycles);
}

void TscTracer::calibrate_probe_overhead() noexcept {
    // self_ns: median of empty scopes, each recording itself like a real probe.
    for (int i = 0; i < kWarmupScopes; ++i) {
        Scope s(this, ProbeId::ProbeOverhead);
        (void)s;
    }
    for (int i = 0; i < kProbeOverheadSamples; ++i) {
        Scope s(this, ProbeId::ProbeOverhead);
        (void)s;
    }
    probe_overhead_self_ns_ =
        static_cast<double>(hist_[std::to_underlying(ProbeId::ProbeOverhead)].percentile(50));

    // parent_ns: time N empty scopes with raw counter reads, divide by N.
    const uint64_t start = read_timestamp_counter();
    for (int i = 0; i < kProbeOverheadSamples; ++i) {
        Scope s(this, ProbeId::ProbeOverhead);
        (void)s;
    }
    const uint64_t end = read_timestamp_counter();
    const double total_ns = static_cast<double>(end - start) * ns_per_cycle_;
    probe_overhead_parent_ns_ = total_ns / static_cast<double>(kProbeOverheadSamples);
}

}  // namespace ticker
