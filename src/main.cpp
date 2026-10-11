// ticker_feed: subscribes to Coinbase's `ticker` channel, keeps a sliding
// mean/median/low/high window per symbol, and writes every tick plus its
// stats to a per-symbol CSV. This file wires it together: config, symbol
// registry, CPU pinning, feed thread (this one) + writer thread, summary.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app_config.hpp"
#include "cpu_affinity.hpp"
#include "csv_writer.hpp"
#include "frame_capture.hpp"
#include "policy/policy_registry.hpp"
#include "sliding_window.hpp"
#include "spsc_ring.hpp"
#include "symbol_registry.hpp"
#include "ticker_parser.hpp"
#include "ws_feed.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};
void handle_sigint(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "wss://host" -> "host" for Beast's resolver (the endpoint has no path).
std::string host_from_endpoint(const std::string& endpoint) {
    auto pos = endpoint.find("://");
    return pos == std::string::npos ? endpoint : endpoint.substr(pos + std::string_view("://").size());
}

// Feed -> writer ring slots (power of two). Replaying the 131-symbol capture
// at full speed never fills it.
constexpr size_t kRingCapacity = 1 << 14;

// Writer thread: sleep when the ring is empty (trades a little CSV latency
// for not burning a core), and check the flush clock every N records.
constexpr auto kWriterIdleSleep = std::chrono::microseconds(50);
constexpr uint64_t kFlushCheckEvery = 64;  // power of two, used as a mask

constexpr std::string_view kConfigFlag = "--config=";

// Feed-thread-only counters, so not atomic.
struct RunStats {
    uint64_t parse_failures = 0;     // frames that are, or claim to be, tickers but don't parse
    uint64_t non_ticker_frames = 0;  // control frames: subscriptions, heartbeat, error, ...
    uint64_t unknown_symbol = 0;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace ticker;

    std::string config_path = "ticker.conf";
    std::vector<std::string> overrides;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.starts_with(kConfigFlag)) {
            config_path = arg.substr(kConfigFlag.size());
        } else if (arg.rfind("--", 0) == 0) {
            overrides.push_back(arg.substr(2));
        }
    }

    AppConfig cfg;
    try {
        RawConfig raw = RawConfig::parse_file(config_path);
        for (auto& o : overrides) raw.apply_override(o);
        cfg = AppConfig::load(raw);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 1;
    }

    // Every run writes into its own directory under output.dir, so CSVs from different runs never mix.
    // `latest` points at the newest one.
    char stamp[32];
    {
        std::time_t t = std::time(nullptr);
        std::tm local{};
        ::localtime_r(&t, &local);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &local);
    }
    const std::string run_dir = std::format("{}/{}_{}", cfg.output_dir, stamp, ::getpid());
    try {
        std::filesystem::create_directories(run_dir);
    } catch (const std::exception& e) {
        std::cerr << "cannot create " << run_dir << ": " << e.what() << "\n";
        return 1;
    }
    {
        const std::filesystem::path latest = std::filesystem::path(cfg.output_dir) / "latest";
        std::error_code ec;
        std::filesystem::remove(latest, ec);
        std::filesystem::create_directory_symlink(std::filesystem::path(run_dir).filename(), latest, ec);
    }
    std::ofstream run_log(run_dir + "/run.log", std::ios::trunc);
    // Startup/shutdown logging only, never on the hot path.
    auto log_line = [&]<typename... Args>(std::format_string<Args...> fmt, Args&&... args) {
        std::string s = std::format(fmt, std::forward<Args>(args)...);
        std::cout << s << "\n";
        run_log << s << "\n";
        run_log.flush();
    };

    log_line("=== ticker_feed starting ===");

    std::signal(SIGINT, handle_sigint);

    const std::vector<std::string>& symbols = cfg.symbols;
    log_line("{}", cfg.to_log_string());
    log_line("run_dir={}", run_dir);

    SymbolRegistry registry(symbols.size());  // pre-sized: no rehash while running
    for (auto& s : symbols) registry.add(s);

    // --- CPU pinning -----------------------------------------------------
    auto topo = discover_topology("/sys");
    PinPlan pin_plan = choose_pin_plan(topo, cpu_pin_mode_name(cfg.cpu_pin), static_cast<int>(cfg.feed_core),
                                        static_cast<int>(cfg.writer_core));
    log_line("cpu pin plan: {}", pin_plan.note);
    if (pin_plan.pinned) {
        bool ok = pin_current_thread_to(pin_plan.feed_core);
        log_line("feed thread pin to cpu{}{}", pin_plan.feed_core, ok ? ": ok" : ": FAILED (continuing unpinned)");
    }

    // --- policy-dispatched run --------------------------------------------
    // The only runtime branch on the policy: everything below is instantiated
    // per policy, so the hot path has no policy dispatch.
    return dispatch_policy(cfg.policy_id, [&]<typename PolicyT>() -> int {
        using Window = SlidingWindowStats<PolicyT, NullTracer>;

        // --- writer thread + shared ring -----------------------------------
        auto ring = std::make_unique<SpscRing<StatRecord, kRingCapacity>>();
        std::atomic<bool> feed_done{false};
        std::atomic<uint64_t> ring_full_spins{0};

        auto writer_fn = [&]() {
            if (pin_plan.pinned) {
                bool ok = pin_current_thread_to(pin_plan.writer_core);
                log_line("writer thread pin to cpu{}{}", pin_plan.writer_core,
                          ok ? ": ok" : ": FAILED (continuing unpinned)");
            }
            CsvWriter writer(run_dir, symbols);
            StatRecord rec;
            // Single-item pop: batch draining was measured slower for this
            // 80-byte record (see docs/benchmarks.md).
            auto last_flush = std::chrono::steady_clock::now();
            // After seeing feed_done, pop once more before exiting: the
            // acquire on feed_done only guarantees the producer's last push is
            // visible to a head_ load made *after* it.
            bool seen_feed_done = false;
            uint64_t records_since_clock_check = 0;
            while (true) {
                bool popped = ring->try_pop(rec);
                if (popped) {
                    writer.write(rec);
                } else if (seen_feed_done) {
                    break;  // one more pop attempt after observing feed_done, still empty -> truly drained
                } else if (feed_done.load(std::memory_order_acquire)) {
                    seen_feed_done = true;  // loop back for the guaranteed-fresh final pop
                } else {
                    std::this_thread::sleep_for(kWriterIdleSleep);
                }
                // Reading the clock after every record slowed backlog draining.
                if (!popped || (++records_since_clock_check & (kFlushCheckEvery - 1)) == 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_flush >= std::chrono::nanoseconds(cfg.flush_interval_ns)) {
                        writer.flush_all();
                        last_flush = now;
                    }
                }
            }
            writer.flush_all();
            log_line("writer: {} rows written", writer.rows_written());
        };
        std::thread writer_thread(writer_fn);

        // --- per-symbol window state (lazy) ---------------------------------
        std::vector<std::unique_ptr<Window>> windows(registry.size());
        NullTracer tracer;

        RunStats stats;
        auto push_record = [&](int slot, int64_t recv_ts_ns, const Ticker& t) {
            if (!windows[slot]) {
                windows[slot] = std::make_unique<Window>(cfg.window_length_ns, kCapacityHint, tracer);
            }
            // Band parsed lazily: only called by policies that use it.
            WindowResult r = windows[slot]->on_tick(t.event_ts_ns, t.price,
                                                       [&]() noexcept { return std::pair{t.low_24h(), t.high_24h()}; });
            StatRecord rec{static_cast<int32_t>(slot), recv_ts_ns, t.event_ts_ns, t.trade_id, t.price,
                            r.mean, r.median, r.low, r.high, static_cast<uint32_t>(r.count)};
            while (!ring->try_push(rec)) {
                ring_full_spins.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        };

        auto handle_frame = [&](std::string_view raw_frame, int64_t recv_ts_ns) {
            auto t = parse_ticker(raw_frame);
            if (!t) [[unlikely]] {
                auto type = frame_type(raw_frame);
                if (!type || *type == "ticker") {
                    ++stats.parse_failures;
                } else {
                    ++stats.non_ticker_frames;
                    if (*type == "error") log_line("feed error frame: {}", raw_frame);
                }
                return;
            }
            int slot = registry.find(t->product_id);
            if (slot == SymbolRegistry::kNotFound) { ++stats.unknown_symbol; return; }
            push_record(slot, recv_ts_ns, *t);
        };

        // --- feed loop: replay or live ----------------------------------------
        auto start = std::chrono::steady_clock::now();
        try {
            if (!cfg.capture_replay.empty()) {
                log_line("replaying captured frames from {}", cfg.capture_replay);
                FrameCaptureReader reader(cfg.capture_replay);
                std::string raw_frame;
                int64_t recv_ts_ns;
                while (!g_stop_requested.load(std::memory_order_relaxed) && reader.next_frame(raw_frame, recv_ts_ns)) {
                    handle_frame(raw_frame, recv_ts_ns);
                }
                log_line("replay finished: {} frames read", reader.frames_read());
            } else {
                log_line("connecting to {} ...", cfg.endpoint);
                WsFeed feed(host_from_endpoint(cfg.endpoint));
                feed.connect_and_subscribe(symbols);
                log_line("subscribed to {} product(s)", symbols.size());

                std::unique_ptr<FrameCaptureWriter> capture;
                if (!cfg.capture_record.empty()) {
                    capture = std::make_unique<FrameCaptureWriter>(cfg.capture_record);
                    log_line("recording raw frames to {}", cfg.capture_record);
                }

                std::string raw_frame;
                while (!g_stop_requested.load(std::memory_order_relaxed)) {
                    if (cfg.duration_ns > 0) {
                        auto elapsed = std::chrono::steady_clock::now() - start;
                        if (elapsed >= std::chrono::nanoseconds(cfg.duration_ns)) break;
                    }
                    if (!feed.read_frame(raw_frame)) break;
                    int64_t recv_ts_ns = now_ns();
                    handle_frame(raw_frame, recv_ts_ns);
                    // After processing, so the disk write doesn't delay the tick.
                    if (capture) capture->write_frame(raw_frame, recv_ts_ns);
                }
                feed.close();
            }
        } catch (const std::exception& e) {
            log_line("feed loop error: {}", e.what());
        }

        feed_done.store(true, std::memory_order_release);
        writer_thread.join();

        // --- shutdown summary --------------------------------------------------
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        uint64_t total_ring_grows = 0;
        for (auto& w : windows) {
            if (w) total_ring_grows += w->ring_grow_events();
        }

        log_line("=== summary ===");
        log_line("elapsed_ms={}", elapsed);
        log_line("parse_failures={}", stats.parse_failures);
        log_line("non_ticker_frames={}", stats.non_ticker_frames);
        log_line("unknown_symbol={}", stats.unknown_symbol);
        log_line("ring_full_spin_events={}", ring_full_spins.load());
        log_line("total_window_ring_grow_events={}", total_ring_grows);
        const int active_symbols =
            static_cast<int>(std::ranges::count_if(windows, [](const auto& w) { return static_cast<bool>(w); }));
        log_line("active_symbols={}/{}", active_symbols, symbols.size());

        return 0;
    });
}
