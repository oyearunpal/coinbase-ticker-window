#include "app_config.hpp"

#include <sstream>

namespace ticker {

const char* cpu_pin_mode_name(CpuPinMode m) noexcept {
    switch (m) {
        case CpuPinMode::kSharedL3: return "shared_l3";
        case CpuPinMode::kSmtSiblings: return "smt_siblings";
        case CpuPinMode::kExplicit: return "explicit";
        case CpuPinMode::kNone: return "none";
    }
    return "?";
}

namespace {
CpuPinMode parse_cpu_pin_mode(const std::string& s) {
    if (s == "shared_l3") return CpuPinMode::kSharedL3;
    if (s == "smt_siblings") return CpuPinMode::kSmtSiblings;
    if (s == "explicit") return CpuPinMode::kExplicit;
    if (s == "none") return CpuPinMode::kNone;
    throw ConfigError("cpu.pin must be one of shared_l3/smt_siblings/explicit/none, got: " + s);
}
}  // namespace

AppConfig AppConfig::load(RawConfig& raw) {
    AppConfig c;

    c.endpoint = raw.get_str("feed.endpoint", c.endpoint);
    c.symbols = raw.get_str_array("feed.symbols");
    if (c.symbols.empty()) throw ConfigError("feed.symbols must be a non-empty list");
    for (const auto& sym : c.symbols) {
        if (sym == "*") {
            throw ConfigError("feed.symbols = [\"*\"] is not supported: list whole-cent products explicitly "
                              "(e.g. [\"BTC-USD\", \"ETH-USD\", \"SOL-USD\"])");
        }
    }
    c.duration_ns = raw.get_duration_ns("feed.duration", 0);

    c.window_length_ns = raw.get_duration_ns("window.length", c.window_length_ns);
    if (c.window_length_ns <= 0) throw ConfigError("window.length must be positive");
    c.window_policy = raw.get_str("window.policy", c.window_policy);
    try {
        c.policy_id = parse_policy_id(c.window_policy);
    } catch (const std::invalid_argument& e) {
        throw ConfigError(std::string("window.policy: ") + e.what());
    }

    c.output_dir = raw.get_str("output.dir", c.output_dir);
    c.flush_interval_ns = raw.get_duration_ns("output.flush_interval", c.flush_interval_ns);

    c.cpu_pin = parse_cpu_pin_mode(raw.get_str("cpu.pin", "shared_l3"));
    c.feed_core = raw.get_int("cpu.feed_core", -1);
    c.writer_core = raw.get_int("cpu.writer_core", -1);
    if (c.cpu_pin == CpuPinMode::kExplicit && (c.feed_core < 0 || c.writer_core < 0)) {
        throw ConfigError("cpu.pin = explicit requires non-negative cpu.feed_core and cpu.writer_core");
    }

    c.capture_record = raw.get_str("capture.record", "");
    c.capture_replay = raw.get_str("capture.replay", "");

    raw.check_all_keys_recognized();
    return c;
}

std::string AppConfig::to_log_string() const {
    std::ostringstream ss;
    ss << "feed.endpoint=" << endpoint << "\n";
    ss << "feed.symbols=[";
    for (size_t i = 0; i < symbols.size(); ++i) ss << (i ? "," : "") << symbols[i];
    ss << "]\n";
    ss << "feed.duration_ns=" << duration_ns << "\n";
    ss << "window.length_ns=" << window_length_ns << "\n";
    ss << "window.policy=" << window_policy << "\n";
    ss << "output.dir=" << output_dir << "\n";
    ss << "output.flush_interval_ns=" << flush_interval_ns << "\n";
    ss << "cpu.pin=" << cpu_pin_mode_name(cpu_pin) << "\n";
    ss << "cpu.feed_core=" << feed_core << "\n";
    ss << "cpu.writer_core=" << writer_core << "\n";
    ss << "capture.record=" << capture_record << "\n";
    ss << "capture.replay=" << capture_replay << "\n";
    return ss.str();
}

}  // namespace ticker
