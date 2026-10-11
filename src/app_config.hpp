#pragma once
// Typed config schema built on config.hpp; the one place that lists every
// key. See ticker.conf for the annotated defaults.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "policy/policy_registry.hpp"

namespace ticker {

enum class CpuPinMode { kSharedL3, kSmtSiblings, kExplicit, kNone };

struct AppConfig {
    // [feed]
    std::string endpoint = "wss://ws-feed.exchange.coinbase.com";
    std::vector<std::string> symbols;   // whole-cent products only (see fixed_price.hpp)
    int64_t duration_ns = 0;             // 0 = run until SIGINT

    // [window]
    int64_t window_length_ns = 300LL * 1'000'000'000LL;
    // Median algorithm, by name (see policy/policy_registry.hpp and docs/policies.md).
    std::string window_policy = std::string(kDefaultPolicyName);
    PolicyId policy_id = PolicyId::kHybridHistogram;  // parsed from window_policy in load()

    // [output]
    std::string output_dir = "logs";
    int64_t flush_interval_ns = 200LL * 1'000'000LL;

    // [cpu]
    CpuPinMode cpu_pin = CpuPinMode::kSharedL3;
    int64_t feed_core = -1;    // -1 = auto-select from topology
    int64_t writer_core = -1;

    // [capture]
    std::string capture_record;  // path, or empty
    std::string capture_replay;  // path, or empty

    // Validates and converts `raw`; throws ConfigError on an unknown key,
    // bad value or type mismatch. Defaults live only here.
    static AppConfig load(RawConfig& raw);

    // The resolved config, logged at startup so each run records its settings.
    std::string to_log_string() const;
};

const char* cpu_pin_mode_name(CpuPinMode m) noexcept;

}  // namespace ticker
