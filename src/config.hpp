#pragma once
// Minimal parser for the TOML-like config format (see ticker.conf):
// sections, `#` comments, quoted strings, string arrays, integers, bools, and
// duration/size suffixes (300s, 1h, 64k). Startup-only, so written for
// clarity rather than allocation count. Unknown keys are an error.
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ticker {

struct ConfigError : std::runtime_error {
    explicit ConfigError(const std::string& msg) : std::runtime_error(msg) {}
};

// Flat "section.key" -> raw string map (arrays kept as one comma-joined
// string). CLI overrides merge into the same map.
class RawConfig {
  public:
    static RawConfig parse_file(const std::string& path);
    static RawConfig parse_string(const std::string& text, const std::string& source_name = "<string>");

    // "section.key=value" (from --section.key=value).
    void apply_override(const std::string& assignment);

    void set(const std::string& full_key, const std::string& value) { values_[full_key] = value; }
    bool has(const std::string& full_key) const { return values_.count(full_key) != 0; }

    std::string require_str(const std::string& full_key) const;
    std::string get_str(const std::string& full_key, const std::string& def) const;

    int64_t require_int(const std::string& full_key) const;
    int64_t get_int(const std::string& full_key, int64_t def) const;

    bool get_bool(const std::string& full_key, bool def) const;

    // Duration strings like "300s", "1h", "200ms", "0" -> nanoseconds.
    int64_t require_duration_ns(const std::string& full_key) const;
    int64_t get_duration_ns(const std::string& full_key, int64_t def_ns) const;

    // Size strings like "64k", "1024", "100k" -> a plain count.
    size_t require_size(const std::string& full_key) const;
    size_t get_size(const std::string& full_key, size_t def) const;

    std::vector<std::string> get_str_array(const std::string& full_key) const;

    // Throws if any key in the file was never read by the typed loader.
    // Called once after loading, so unread keys are reported as unknown.
    void check_all_keys_recognized() const;
    void mark_recognized(const std::string& full_key) const { recognized_.insert(full_key); }

    const std::map<std::string, std::string>& raw() const { return values_; }

  private:
    static int64_t parse_duration_ns(const std::string& s, const std::string& key);
    static size_t parse_size(const std::string& s, const std::string& key);

    std::map<std::string, std::string> values_;
    std::string source_name_;
    mutable std::set<std::string> recognized_;
};

}  // namespace ticker
