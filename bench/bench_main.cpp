// ticker_bench: replays a capture through the same pipeline as ticker_feed (parse -> symbol
// lookup -> window update -> SPSC ring -> CSV row formatting) and times it with the CPU's cycle
// counter.
//
// Only one set of probes is timed per run, so no timed scope ever contains another one and adds
// its cost to it:
//   --mode=feed    only feed_total: a received tick -> its row pushed onto the writer queue.
//                  Frames that are not ticks (skipped by the parser) are not counted. Run once
//                  per policy; this is the headline number.
//   --mode=stages  the policy-independent stages, each timed on its own: parse_total,
//                  symbol_lookup, record_build, ring_push (feed thread), ring_pop, format_row
//                  (writer thread). feed_total, the parse sub-probes and the window probes are off.
//
// Each pass replays the capture from empty windows. Windows are reset(), not rebuilt, so the
// discarded warmup pass leaves the rings grown and the timed passes see steady state. The writer
// formats every CSV row but does not write it to disk, so file I/O stays out of the numbers.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

#include "../src/config.hpp"
#include "../src/cpu_affinity.hpp"
#include "../src/csv_writer.hpp"
#include "../src/frame_capture.hpp"
#include "../src/iso8601.hpp"
#include "../src/latency_hist.hpp"
#include "../src/policy/policy_registry.hpp"
#include "../src/probe.hpp"
#include "../src/sliding_window.hpp"
#include "../src/spsc_ring.hpp"
#include "../src/symbol_registry.hpp"
#include "../src/ticker_parser.hpp"

using namespace ticker;

namespace {

constexpr size_t kRingCapacity = 1 << 14;
using Ring = SpscRing<StatRecord, kRingCapacity>;

constexpr uint64_t bit(ProbeId id) { return uint64_t{1} << std::to_underlying(id); }

constexpr uint64_t kFeedProbes = bit(ProbeId::FeedTotal);
constexpr uint64_t kStageProbes = bit(ProbeId::ParseTotal) | bit(ProbeId::SymbolLookup) | bit(ProbeId::RecordBuild) |
                                  bit(ProbeId::RingPush) | bit(ProbeId::RingPop) | bit(ProbeId::FormatRow);

// Cycles -> ns, measured against steady_clock over 20 ms.
double calibrate_ns_per_cycle() {
    using clock = std::chrono::steady_clock;
    const auto wall_start = clock::now();
    const uint64_t tsc_start = read_timestamp_counter();
    while (clock::now() - wall_start < std::chrono::milliseconds(20)) {
    }
    const uint64_t tsc_end = read_timestamp_counter();
    const double wall_ns = std::chrono::duration<double, std::nano>(clock::now() - wall_start).count();
    return wall_ns / static_cast<double>(tsc_end - tsc_start);
}

// Smallest non-zero step between back-to-back counter reads, in ns: the finest difference any
// single measurement can show.
double timer_resolution_ns(double ns_per_cycle) {
    uint64_t min_step = UINT64_MAX;
    uint64_t prev = read_timestamp_counter();
    for (int i = 0; i < 1'000'000; ++i) {
        const uint64_t now = read_timestamp_counter();
        if (now != prev) min_step = std::min(min_step, now - prev);
        prev = now;
    }
    return static_cast<double>(min_step) * ns_per_cycle;
}

// Times only the probes in Mask; every other enter() is an empty scope that compiles away (the id
// is a constant at each call site). Histograms exist only for timed probes, so an untimed probe
// costs neither time nor memory.
template <uint64_t Mask>
class ProbeSetTracer {
  public:
    explicit ProbeSetTracer(double ns_per_cycle) : ns_per_cycle_(ns_per_cycle) {
        for (size_t i = 0; i < kProbeCount; ++i) {
            if (Mask & (uint64_t{1} << i)) hist_[i] = std::make_unique<LatencyHist>();
        }
    }

    static constexpr bool timed(ProbeId id) noexcept { return (Mask & bit(id)) != 0; }

    class Scope {
      public:
        Scope(ProbeSetTracer* t, ProbeId id) noexcept : t_(t), id_(id) {
            if (timed(id_)) start_ = read_timestamp_counter();
        }
        ~Scope() {
            if (!timed(id_) || discarded_) return;
            const uint64_t cycles = read_timestamp_counter() - start_;
            t_->hist_[std::to_underlying(id_)]->record(
                static_cast<uint64_t>(static_cast<double>(cycles) * t_->ns_per_cycle_));
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        // Drops this sample: used for a frame that turns out not to be a tick.
        void discard() noexcept { discarded_ = true; }

      private:
        ProbeSetTracer* t_;
        ProbeId id_;
        uint64_t start_ = 0;
        bool discarded_ = false;
    };

    Scope enter(ProbeId id) noexcept { return Scope(this, id); }
    void note(ProbeId id, uint64_t value) noexcept {
        if (timed(id)) hist_[std::to_underlying(id)]->record(value);
    }
    const LatencyHist& histogram(ProbeId id) const noexcept { return *hist_[std::to_underlying(id)]; }

  private:
    double ns_per_cycle_;
    std::array<std::unique_ptr<LatencyHist>, kProbeCount> hist_{};
};

template <typename PolicyT, typename Tracer>
using WindowT = SlidingWindowStats<PolicyT, Tracer>;

struct BenchArgs {
    std::string replay_path;
    int64_t window_ns = 300LL * 1'000'000'000LL;
    std::string pin_mode = "shared_l3";
    int64_t feed_core = -1, writer_core = -1;
    int passes = 3;
    std::string mode = "feed";  // feed | stages
    std::string policy = std::string(kDefaultPolicyName);
    std::string out_csv;  // optional: append one row per run
};

struct PassStats {
    uint64_t ticks = 0, symbols = 0;
};

// The symbol registry and each symbol's window, kept across passes (reset(), not rebuilt).
template <typename PolicyT, typename Tracer>
struct State {
    SymbolRegistry registry;
    std::vector<std::unique_ptr<WindowT<PolicyT, Tracer>>> windows;
};

template <typename PolicyT, typename Tracer>
PassStats run_one_pass(const BenchArgs& args, Tracer& tracer, const PinPlan& pin_plan, State<PolicyT, Tracer>& state) {
    PassStats stats;
    for (auto& w : state.windows) {
        if (w) w->reset();
    }

    auto ring = std::make_unique<Ring>();
    std::atomic<bool> feed_done{false};

    std::thread writer_thread([&] {
        if (pin_plan.pinned) pin_current_thread_to(pin_plan.writer_core);
        StatRecord rec;
        char row[256];
        Iso8601Formatter recv_fmt, event_fmt;  // persist across rows, as in CsvWriter
        bool seen_feed_done = false;           // one extra pop after feed_done: see main.cpp
        for (;;) {
            bool popped;
            {
                // Includes empty polls while the writer waits: "cost of polling", not just of a pop.
                auto scope = tracer.enter(ProbeId::RingPop);
                popped = ring->try_pop(rec);
            }
            if (popped) {
                auto scope = tracer.enter(ProbeId::FormatRow);
                (void)format_csv_row(rec, row, recv_fmt, event_fmt);
            } else if (seen_feed_done) {
                break;
            } else if (feed_done.load(std::memory_order_acquire)) {
                seen_feed_done = true;
            } else {
                std::this_thread::yield();
            }
        }
    });

    if (pin_plan.pinned) pin_current_thread_to(pin_plan.feed_core);

    FrameCaptureReader reader(args.replay_path);
    std::string raw;
    int64_t recv_ts_ns;
    while (reader.next_frame(raw, recv_ts_ns)) {
        // The frame has been received: from here to the ring push is feed_total.
        auto total_scope = tracer.enter(ProbeId::FeedTotal);

        auto t = parse_ticker(raw, tracer);
        if (!t) {
            total_scope.discard();  // not a tick (e.g. a sub-cent product): no row, so no feed_total
            continue;
        }

        int slot;
        {
            auto scope = tracer.enter(ProbeId::SymbolLookup);
            slot = state.registry.find(t->product_id);
            if (slot == SymbolRegistry::kNotFound) {
                slot = state.registry.add(t->product_id);
                state.windows.resize(state.registry.size());
            }
        }
        auto& w = state.windows[static_cast<size_t>(slot)];
        if (!w) w = std::make_unique<WindowT<PolicyT, Tracer>>(args.window_ns, kCapacityHint, tracer);
        // The band lambda only runs for a policy that still wants_market() (see on_tick).
        WindowResult r = w->on_tick(t->event_ts_ns, t->price,
                                    [&]() noexcept { return std::pair{t->low_24h(), t->high_24h()}; });
        ++stats.ticks;

        StatRecord rec;
        {
            auto scope = tracer.enter(ProbeId::RecordBuild);
            rec = StatRecord{static_cast<int32_t>(slot), recv_ts_ns, t->event_ts_ns, t->trade_id, t->price,
                             r.mean, r.median, r.low, r.high, static_cast<uint32_t>(r.count)};
        }
        {
            auto scope = tracer.enter(ProbeId::RingPush);
            while (!ring->try_push(rec)) std::this_thread::yield();
        }
    }

    feed_done.store(true, std::memory_order_release);
    writer_thread.join();
    stats.symbols = state.registry.size();
    return stats;
}

uint64_t peak_rss_kb() {
    struct rusage ru {};
    ::getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return static_cast<uint64_t>(ru.ru_maxrss) / 1024;  // bytes on macOS
#else
    return static_cast<uint64_t>(ru.ru_maxrss);  // kilobytes on Linux
#endif
}

const ProbeId kAllReported[] = {ProbeId::FeedTotal,  ProbeId::ParseTotal, ProbeId::SymbolLookup,
                                ProbeId::RecordBuild, ProbeId::RingPush,   ProbeId::RingPop,
                                ProbeId::FormatRow};

// One row per run: run info, then mean/p50/p90/p99/max for every reported probe (empty when not
// timed in this mode). Writes the header when the file is new or empty.
template <typename Tracer>
void append_csv(const BenchArgs& args, const Tracer& tracer, const PassStats& s, uint64_t rss_kb,
                double resolution_ns) {
    const bool is_new = [&] {
        std::ifstream in(args.out_csv);
        return !in || in.peek() == std::ifstream::traits_type::eof();
    }();
    std::ofstream f(args.out_csv, std::ios::app);
    if (!f) {
        std::cerr << "error: cannot open " << args.out_csv << "\n";
        return;
    }
    if (is_new) {
        f << "policy,mode,capture,window_s,passes,ticks,symbols,peak_rss_kb,timer_resolution_ns";
        for (ProbeId id : kAllReported) {
            const std::string n = probe_name(id);
            f << "," << n << "_mean_ns," << n << "_p50_ns," << n << "_p90_ns," << n << "_p99_ns," << n << "_max_ns";
        }
        f << "\n";
    }
    f << std::fixed << std::setprecision(1);
    f << args.policy << "," << args.mode << "," << args.replay_path << "," << args.window_ns / 1'000'000'000LL << ","
      << args.passes << "," << s.ticks << "," << s.symbols << "," << rss_kb << "," << resolution_ns;
    for (ProbeId id : kAllReported) {
        if (!Tracer::timed(id)) {
            f << ",,,,,";
            continue;
        }
        const LatencyHist& h = tracer.histogram(id);
        f << "," << h.mean() << "," << h.percentile(50) << "," << h.percentile(90) << "," << h.percentile(99) << ","
          << h.max();
    }
    f << "\n";
}

template <typename PolicyT, uint64_t Mask>
int run(const BenchArgs& args, const PinPlan& pin_plan) {
    using Tracer = ProbeSetTracer<Mask>;
    const double ns_per_cycle = calibrate_ns_per_cycle();
    const double resolution_ns = timer_resolution_ns(ns_per_cycle);

    State<PolicyT, Tracer> state;
    {
        Tracer warmup(ns_per_cycle);  // discarded; grows the rings so the timed passes see steady state
        run_one_pass<PolicyT, Tracer>(args, warmup, pin_plan, state);
    }
    Tracer tracer(ns_per_cycle);
    PassStats s;
    for (int p = 0; p < args.passes; ++p) s = run_one_pass<PolicyT, Tracer>(args, tracer, pin_plan, state);
    const uint64_t rss = peak_rss_kb();

    printf("\n=== ticker_bench: policy=%s mode=%s window=%llds capture=%s ===\n", args.policy.c_str(),
           args.mode.c_str(), static_cast<long long>(args.window_ns / 1'000'000'000LL), args.replay_path.c_str());
    printf("ticks=%llu symbols=%llu passes=%d peak_rss=%.1f MB timer_resolution=%.1f ns\n\n",
           static_cast<unsigned long long>(s.ticks), static_cast<unsigned long long>(s.symbols), args.passes,
           static_cast<double>(rss) / 1024.0, resolution_ns);
    printf("%-14s %10s %9s %8s %8s %8s %8s\n", "probe", "count", "mean_ns", "p50_ns", "p90_ns", "p99_ns", "max_ns");
    for (ProbeId id : kAllReported) {
        if (!Tracer::timed(id)) continue;
        const LatencyHist& h = tracer.histogram(id);
        printf("%-14s %10llu %9.1f %8llu %8llu %8llu %8llu\n", probe_name(id),
               static_cast<unsigned long long>(h.count()), h.mean(), static_cast<unsigned long long>(h.percentile(50)),
               static_cast<unsigned long long>(h.percentile(90)), static_cast<unsigned long long>(h.percentile(99)),
               static_cast<unsigned long long>(h.max()));
    }

    if (!args.out_csv.empty()) {
        append_csv(args, tracer, s, rss, resolution_ns);
        printf("appended to %s\n", args.out_csv.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    BenchArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const std::string& prefix) -> std::optional<std::string> {
            if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = val("--replay=")) args.replay_path = *v;
        else if (auto v = val("--window=")) args.window_ns = RawConfig::parse_string("[x]\nv=" + *v).get_duration_ns("x.v", 0);
        else if (auto v = val("--pin=")) args.pin_mode = *v;
        else if (auto v = val("--feed-core=")) args.feed_core = std::stoll(*v);
        else if (auto v = val("--writer-core=")) args.writer_core = std::stoll(*v);
        else if (auto v = val("--passes=")) args.passes = std::stoi(*v);
        else if (auto v = val("--mode=")) args.mode = *v;
        else if (auto v = val("--policy=")) args.policy = *v;
        else if (auto v = val("--out=")) args.out_csv = *v;
        else {
            std::cerr << "error: unknown argument " << a << "\n";
            return 1;
        }
    }
    if (args.replay_path.empty() || args.window_ns <= 0 || args.passes < 1 ||
        (args.mode != "feed" && args.mode != "stages")) {
        std::cerr << "usage: ticker_bench --replay=<capture.bin> [--mode=feed|stages] [--window=300s]\n"
                     "         [--policy=hybrid_histogram|exact_map|tracked_histogram|range_histogram|\n"
                     "         range_histogram_no_rebuild|sorted_vector|sorted_vector_freq] [--passes=3]\n"
                     "         [--pin=shared_l3|smt_siblings|none|explicit --feed-core=N --writer-core=N]\n"
                     "         [--out=results.csv]\n";
        return 1;
    }
    PolicyId policy_id;
    try {
        policy_id = parse_policy_id(args.policy);
    } catch (const std::invalid_argument& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    const PinPlan pin_plan = choose_pin_plan(discover_topology("/sys"), args.pin_mode,
                                             static_cast<int>(args.feed_core), static_cast<int>(args.writer_core));
    printf("pin plan: %s\n", pin_plan.note.c_str());

    if (args.mode == "stages") {
        return dispatch_policy(policy_id, [&]<typename PolicyT>() { return run<PolicyT, kStageProbes>(args, pin_plan); });
    }
    return dispatch_policy(policy_id, [&]<typename PolicyT>() { return run<PolicyT, kFeedProbes>(args, pin_plan); });
}
