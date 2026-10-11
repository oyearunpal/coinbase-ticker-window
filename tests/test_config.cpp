#include "doctest.h"
#include "../src/config.hpp"
#include "../src/app_config.hpp"

using namespace ticker;

TEST_CASE("RawConfig: basic key/value round trip through sections") {
    auto cfg = RawConfig::parse_string(R"(
[feed]
endpoint = "wss://example.com"
duration = 600s

[window]
length = 300s
max_items = 100k
)");
    CHECK(cfg.get_str("feed.endpoint", "") == "wss://example.com");
    CHECK(cfg.get_duration_ns("feed.duration", -1) == 600LL * 1'000'000'000LL);
    CHECK(cfg.get_duration_ns("window.length", -1) == 300LL * 1'000'000'000LL);
    CHECK(cfg.get_size("window.max_items", 0) == 100'000u);
}

TEST_CASE("RawConfig: comments and blank lines are ignored") {
    auto cfg = RawConfig::parse_string(R"(
# a top-level comment
[feed]
# comment inside a section
endpoint = "x"  # inline comment

duration = 5s
)");
    CHECK(cfg.get_str("feed.endpoint", "") == "x");
    CHECK(cfg.get_duration_ns("feed.duration", -1) == 5LL * 1'000'000'000LL);
}

TEST_CASE("RawConfig: string arrays") {
    auto cfg = RawConfig::parse_string(R"(
[feed]
symbols = ["BTC-USD", "ETH-USD", "SOL-USD"]
)");
    auto v = cfg.get_str_array("feed.symbols");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "BTC-USD");
    CHECK(v[1] == "ETH-USD");
    CHECK(v[2] == "SOL-USD");
}

TEST_CASE("RawConfig: duration suffixes ms/s/m/h and bare-number-as-seconds") {
    auto cfg = RawConfig::parse_string(R"(
[x]
a = 200ms
b = 5s
c = 2m
d = 1h
e = 0
)");
    CHECK(cfg.get_duration_ns("x.a", -1) == 200LL * 1'000'000LL);
    CHECK(cfg.get_duration_ns("x.b", -1) == 5LL * 1'000'000'000LL);
    CHECK(cfg.get_duration_ns("x.c", -1) == 120LL * 1'000'000'000LL);
    CHECK(cfg.get_duration_ns("x.d", -1) == 3600LL * 1'000'000'000LL);
    CHECK(cfg.get_duration_ns("x.e", -1) == 0);
}

TEST_CASE("RawConfig: size suffixes k/m") {
    auto cfg = RawConfig::parse_string("[x]\na = 100k\nb = 2m\nc = 64\n");
    CHECK(cfg.get_size("x.a", 0) == 100'000u);
    CHECK(cfg.get_size("x.b", 0) == 2'000'000u);
    CHECK(cfg.get_size("x.c", 0) == 64u);
}

TEST_CASE("RawConfig: bool parsing") {
    auto cfg = RawConfig::parse_string("[x]\na = true\nb = false\n");
    CHECK(cfg.get_bool("x.a", false) == true);
    CHECK(cfg.get_bool("x.b", true) == false);
}

TEST_CASE("RawConfig: missing required key throws") {
    auto cfg = RawConfig::parse_string("[x]\na = 1\n");
    CHECK_THROWS_AS(cfg.require_str("x.missing"), ConfigError);
}

TEST_CASE("RawConfig: key outside any section is an error") {
    CHECK_THROWS_AS(RawConfig::parse_string("a = 1\n"), ConfigError);
}

TEST_CASE("RawConfig: CLI override precedence over file value") {
    auto cfg = RawConfig::parse_string("[feed]\nendpoint = \"orig\"\n");
    CHECK(cfg.get_str("feed.endpoint", "") == "orig");
    cfg.apply_override("feed.endpoint=overridden");
    CHECK(cfg.get_str("feed.endpoint", "") == "overridden");
}

TEST_CASE("RawConfig: unknown key is a hard error once checked") {
    auto cfg = RawConfig::parse_string("[feed]\nendpoint = \"x\"\ntypo_key = 1\n");
    cfg.get_str("feed.endpoint", "");  // recognize this one
    CHECK_THROWS_AS(cfg.check_all_keys_recognized(), ConfigError);
}

TEST_CASE("RawConfig: all-recognized-keys passes the check") {
    auto cfg = RawConfig::parse_string("[feed]\nendpoint = \"x\"\n");
    cfg.get_str("feed.endpoint", "");
    CHECK_NOTHROW(cfg.check_all_keys_recognized());
}

TEST_CASE("AppConfig: loads the shipped ticker.conf") {
    auto raw = RawConfig::parse_file("ticker.conf");
    auto cfg = AppConfig::load(raw);
    REQUIRE(cfg.symbols.size() == 3);
    CHECK(cfg.symbols[0] == "BTC-USD");
    CHECK(cfg.window_length_ns == 300LL * 1'000'000'000LL);
    CHECK(cfg.window_policy == "hybrid_histogram");
    CHECK(cfg.policy_id == PolicyId::kHybridHistogram);
}

TEST_CASE("AppConfig: feed.symbols = [\"*\"] is rejected (whole-cent products only)") {
    auto raw = RawConfig::parse_string(R"(
[feed]
symbols = ["*"]
)");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: the removed [universe] section is an unrecognized key") {
    auto raw = RawConfig::parse_string(R"(
[feed]
symbols = ["BTC-USD"]
[universe]
status = "online"
)");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: every real policy name is accepted by window.policy, and resolves to the matching "
          "PolicyId") {
    // Every policy ticker_feed can now dispatch to (see
    // policy/policy_registry.hpp's dispatch_policy()) -- checked as a
    // list so that adding a new policy without updating this test (and
    // app_config.cpp/policy_registry.hpp) fails here rather than being
    // silently untested.
    const std::pair<const char*, PolicyId> cases[] = {
        {"exact_map", PolicyId::kExactMap},
        {"tracked_histogram", PolicyId::kTrackedHistogram},
        {"range_histogram", PolicyId::kRangeHistogram},
        {"range_histogram_no_rebuild", PolicyId::kRangeHistogramNoRebuild},
        {"sorted_vector", PolicyId::kSortedVector},
        {"sorted_vector_freq", PolicyId::kSortedVectorFreq},
        {"hybrid_histogram", PolicyId::kHybridHistogram},
    };
    for (const auto& [policy, id] : cases) {
        CAPTURE(policy);
        auto raw = RawConfig::parse_string(std::string(R"(
[feed]
symbols = ["BTC-USD"]
[window]
policy = ")") + policy + "\"\n");
        auto cfg = AppConfig::load(raw);
        CHECK(cfg.window_policy == policy);
        CHECK(cfg.policy_id == id);
    }
}

TEST_CASE("AppConfig: window.policy defaults to hybrid_histogram at any window length, and \"auto\" "
          "is no longer accepted") {
    for (const char* len : {"60s", "300s", "3600s"}) {
        CAPTURE(len);
        auto raw = RawConfig::parse_string(std::string("[feed]\nsymbols = [\"BTC-USD\"]\n[window]\nlength = ") +
                                           len + "\n");
        CHECK(AppConfig::load(raw).policy_id == PolicyId::kHybridHistogram);
    }
    auto raw = RawConfig::parse_string("[feed]\nsymbols = [\"BTC-USD\"]\n[window]\npolicy = \"auto\"\n");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: an unrecognized window.policy value is rejected") {
    auto raw = RawConfig::parse_string(R"(
[feed]
symbols = ["BTC-USD"]
[window]
policy = "some_future_policy"
)");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: rejects a config with a typo'd key") {
    auto raw = RawConfig::parse_string(R"(
[feed]
symbols = ["BTC-USD"]
durtaion = 10s
)");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: explicit cpu pin mode requires both cores set") {
    auto raw = RawConfig::parse_string(R"(
[feed]
symbols = ["BTC-USD"]
[cpu]
pin = "explicit"
)");
    CHECK_THROWS_AS(AppConfig::load(raw), ConfigError);
}

TEST_CASE("AppConfig: defaults apply when sections are omitted") {
    auto raw = RawConfig::parse_string("[feed]\nsymbols = [\"BTC-USD\"]\n");
    auto cfg = AppConfig::load(raw);
    CHECK(cfg.window_length_ns == 300LL * 1'000'000'000LL);
    CHECK(cfg.cpu_pin == CpuPinMode::kSharedL3);
    CHECK(cfg.output_dir == "logs");
}
