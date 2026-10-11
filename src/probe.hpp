#pragma once
// Hot-path instrumentation that costs nothing when disabled.
//
// Each pipeline stage is wrapped in `auto scope = tracer.enter(id);`. The
// tracer is a template parameter: ticker_feed uses NullTracer (empty
// inline no-ops, removed by the compiler) and
// ticker_bench uses TscTracer, which times each scope with the cycle
// counter and records ns into a per-stage LatencyHist. Tracers are
// instances, not a global, so each benchmark run gets fresh histograms.
#include <array>
#include <cstdint>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include "latency_hist.hpp"

namespace ticker {

// Cycle counter: rdtscp on x86_64, CNTVCT on aarch64. Benchmark-only.
inline uint64_t read_timestamp_counter() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t v;
    asm volatile("isb" ::: "memory");
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#error "read_timestamp_counter: unsupported architecture"
#endif
}

enum class ProbeId : uint8_t {
    WsRead,
    ParseTotal,
    ParseTypecheck,
    ParseScan,
    ParsePrice,
    ParseTime,
    SymbolLookup,
    WinEvict,
    WinEvictCount,  // evictions per tick (a count, not a duration)
    WinInsert,
    WinQuery,
    RecordBuild,
    RingPush,
    FeedTotal,
    RingPop,
    FormatRow,
    WriteSyscall,
    Flush,
    EndToEnd,
    ProbeOverhead,  // TscTracer's own cost, measured at construction
    kCount
};

inline constexpr const char* probe_name(ProbeId id) noexcept {
    switch (id) {
        case ProbeId::WsRead: return "ws_read";
        case ProbeId::ParseTotal: return "parse_total";
        case ProbeId::ParseTypecheck: return "parse_typecheck";
        case ProbeId::ParseScan: return "parse_scan";
        case ProbeId::ParsePrice: return "parse_price";
        case ProbeId::ParseTime: return "parse_time";
        case ProbeId::SymbolLookup: return "symbol_lookup";
        case ProbeId::WinEvict: return "win_evict";
        case ProbeId::WinEvictCount: return "win_evict_count";
        case ProbeId::WinInsert: return "win_insert";
        case ProbeId::WinQuery: return "win_query";
        case ProbeId::RecordBuild: return "record_build";
        case ProbeId::RingPush: return "ring_push";
        case ProbeId::FeedTotal: return "feed_total";
        case ProbeId::RingPop: return "ring_pop";
        case ProbeId::FormatRow: return "format_row";
        case ProbeId::WriteSyscall: return "write_syscall";
        case ProbeId::Flush: return "flush";
        case ProbeId::EndToEnd: return "end_to_end";
        case ProbeId::ProbeOverhead: return "probe_overhead";
        // Explicit (no default:) so -Wswitch flags a new ProbeId without a name.
        case ProbeId::kCount: std::unreachable();
    }
    std::unreachable();
}

inline constexpr size_t kProbeCount = std::to_underlying(ProbeId::kCount);

// --- NullTracer: compiles away completely in the shipping binary. -----
struct NullTracer {
    // User-provided destructor so `auto scope = ...` isn't flagged as unused.
    struct Scope {
        ~Scope() noexcept {}
    };
    Scope enter(ProbeId) noexcept { return {}; }
    void note(ProbeId, uint64_t) noexcept {}
};

// --- TscTracer: rdtscp-based stage timing for ticker_bench. --------
class TscTracer {
  public:
    TscTracer() {
        ns_per_cycle_ = calibrate();
        calibrate_probe_overhead();
    }

    class Scope {
      public:
        Scope(TscTracer* t, ProbeId id) noexcept : t_(t), id_(id) {
            start_ = read_timestamp_counter();
        }
        ~Scope() {
            uint64_t end = read_timestamp_counter();
            uint64_t cycles = end - start_;
            uint64_t ns = static_cast<uint64_t>(static_cast<double>(cycles) * t_->ns_per_cycle_);
            t_->hist_[std::to_underlying(id_)].record(ns);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

      private:
        TscTracer* t_;
        ProbeId id_;
        uint64_t start_;
    };

    Scope enter(ProbeId id) noexcept { return Scope(this, id); }

    // Records a value that isn't a duration (e.g. evictions per tick).
    void note(ProbeId id, uint64_t value) noexcept { hist_[std::to_underlying(id)].record(value); }

    const LatencyHist& histogram(ProbeId id) const noexcept { return hist_[std::to_underlying(id)]; }
    double ns_per_cycle() const noexcept { return ns_per_cycle_; }

    // Median cost of an empty scope as reported by itself: the floor under
    // every probe reading.
    double probe_overhead_self_ns() const noexcept { return probe_overhead_self_ns_; }

    // Cost an empty child scope adds to its parent's reading (why a parent
    // reads more than the sum of its children).
    double probe_overhead_parent_ns() const noexcept { return probe_overhead_parent_ns_; }

  private:
    // Cycles -> ns ratio, measured against the wall clock.
    static double calibrate() noexcept;

    // Fills the two probe_overhead values using real Scope objects.
    void calibrate_probe_overhead() noexcept;

    std::array<LatencyHist, kProbeCount> hist_{};
    double ns_per_cycle_ = 1.0;
    double probe_overhead_self_ns_ = 0.0;
    double probe_overhead_parent_ns_ = 0.0;
};

}  // namespace ticker
