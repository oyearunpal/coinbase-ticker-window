#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace ticker {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strips a `#` comment, but not one that appears inside a quoted string.
std::string strip_comment(const std::string& line) {
    std::string out;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') in_quotes = !in_quotes;
        if (c == '#' && !in_quotes) break;
        out.push_back(c);
    }
    return out;
}

std::string strip_quotes_if_present(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

std::vector<std::string> parse_array(const std::string& body) {
    // body is the text between '[' and ']', e.g.  "BTC-USD", "ETH-USD"
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (char c : body) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        if (c == ',' && !in_quotes) {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

}  // namespace

RawConfig RawConfig::parse_string(const std::string& text, const std::string& source_name) {
    RawConfig cfg;
    cfg.source_name_ = source_name;
    std::string section;
    std::istringstream in(text);
    std::string raw_line;
    int lineno = 0;

    while (std::getline(in, raw_line)) {
        ++lineno;
        std::string line = trim(strip_comment(raw_line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (section.empty()) {
                throw ConfigError(source_name + ":" + std::to_string(lineno) + ": empty section name");
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            throw ConfigError(source_name + ":" + std::to_string(lineno) + ": expected 'key = value', got: " + line);
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key.empty()) {
            throw ConfigError(source_name + ":" + std::to_string(lineno) + ": empty key");
        }
        if (section.empty()) {
            throw ConfigError(source_name + ":" + std::to_string(lineno) + ": key '" + key + "' outside of any [section]");
        }

        std::string full_key = section + "." + key;

        if (!value.empty() && value.front() == '[' && value.back() == ']') {
            auto items = parse_array(value.substr(1, value.size() - 2));
            std::string joined;
            for (size_t k = 0; k < items.size(); ++k) {
                if (k) joined.push_back('\x1f');
                joined += items[k];
            }
            cfg.values_[full_key] = joined;
        } else {
            cfg.values_[full_key] = strip_quotes_if_present(value);
        }
    }
    return cfg;
}

RawConfig RawConfig::parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw ConfigError("cannot open config file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_string(ss.str(), path);
}

void RawConfig::apply_override(const std::string& assignment) {
    size_t eq = assignment.find('=');
    if (eq == std::string::npos) {
        throw ConfigError("malformed --key=value override (no '='): " + assignment);
    }
    std::string key = trim(assignment.substr(0, eq));
    std::string value = trim(assignment.substr(eq + 1));
    if (key.find('.') == std::string::npos) {
        throw ConfigError("override key must be 'section.key', got: " + key);
    }
    values_[key] = strip_quotes_if_present(value);
}

std::string RawConfig::require_str(const std::string& full_key) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end()) throw ConfigError("missing required config key: " + full_key);
    return it->second;
}

std::string RawConfig::get_str(const std::string& full_key, const std::string& def) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    return it == values_.end() ? def : it->second;
}

int64_t RawConfig::require_int(const std::string& full_key) const {
    std::string s = require_str(full_key);
    try {
        return std::stoll(s);
    } catch (...) {
        throw ConfigError("expected an integer for " + full_key + ", got: " + s);
    }
}

int64_t RawConfig::get_int(const std::string& full_key, int64_t def) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end()) return def;
    try {
        return std::stoll(it->second);
    } catch (...) {
        throw ConfigError("expected an integer for " + full_key + ", got: " + it->second);
    }
}

bool RawConfig::get_bool(const std::string& full_key, bool def) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end()) return def;
    if (it->second == "true") return true;
    if (it->second == "false") return false;
    throw ConfigError("expected true/false for " + full_key + ", got: " + it->second);
}

int64_t RawConfig::parse_duration_ns(const std::string& s, const std::string& key) {
    if (s.empty()) throw ConfigError("empty duration value for " + key);
    size_t unit_start = s.size();
    while (unit_start > 0 && std::isalpha(static_cast<unsigned char>(s[unit_start - 1]))) --unit_start;
    std::string num_part = s.substr(0, unit_start);
    std::string unit = s.substr(unit_start);

    double value;
    try {
        value = std::stod(num_part);
    } catch (...) {
        throw ConfigError("expected a duration (e.g. 300s, 1h, 200ms) for " + key + ", got: " + s);
    }

    double ns_per_unit;
    if (unit.empty() || unit == "s") ns_per_unit = 1e9;
    else if (unit == "ms") ns_per_unit = 1e6;
    else if (unit == "us") ns_per_unit = 1e3;
    else if (unit == "ns") ns_per_unit = 1.0;
    else if (unit == "m") ns_per_unit = 60.0 * 1e9;
    else if (unit == "h") ns_per_unit = 3600.0 * 1e9;
    else throw ConfigError("unknown duration unit '" + unit + "' for " + key + " (expected s/ms/us/ns/m/h)");

    return static_cast<int64_t>(value * ns_per_unit);
}

int64_t RawConfig::require_duration_ns(const std::string& full_key) const {
    return parse_duration_ns(require_str(full_key), full_key);
}

int64_t RawConfig::get_duration_ns(const std::string& full_key, int64_t def_ns) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end()) return def_ns;
    return parse_duration_ns(it->second, full_key);
}

size_t RawConfig::parse_size(const std::string& s, const std::string& key) {
    if (s.empty()) throw ConfigError("empty size value for " + key);
    size_t unit_start = s.size();
    while (unit_start > 0 && std::isalpha(static_cast<unsigned char>(s[unit_start - 1]))) --unit_start;
    std::string num_part = s.substr(0, unit_start);
    std::string unit = s.substr(unit_start);

    double base;
    try {
        base = std::stod(num_part);
    } catch (...) {
        throw ConfigError("expected a size (e.g. 100k, 1024) for " + key + ", got: " + s);
    }

    double mult = 1.0;
    if (unit.empty()) mult = 1.0;
    else if (unit == "k" || unit == "K") mult = 1e3;   // decimal, not binary -- documented
    else if (unit == "m" || unit == "M") mult = 1e6;
    else throw ConfigError("unknown size unit '" + unit + "' for " + key + " (expected k/m or none)");

    return static_cast<size_t>(base * mult);
}

size_t RawConfig::require_size(const std::string& full_key) const {
    return parse_size(require_str(full_key), full_key);
}

size_t RawConfig::get_size(const std::string& full_key, size_t def) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end()) return def;
    return parse_size(it->second, full_key);
}

std::vector<std::string> RawConfig::get_str_array(const std::string& full_key) const {
    mark_recognized(full_key);
    auto it = values_.find(full_key);
    if (it == values_.end() || it->second.empty()) return {};
    std::vector<std::string> out;
    std::string cur;
    for (char c : it->second) {
        if (c == '\x1f') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

void RawConfig::check_all_keys_recognized() const {
    std::vector<std::string> unknown;
    for (auto& [k, v] : values_) {
        (void)v;
        if (!recognized_.count(k)) unknown.push_back(k);
    }
    if (!unknown.empty()) {
        std::string msg = "unrecognized config key(s) in " + source_name_ + ":";
        for (auto& k : unknown) msg += " " + k;
        throw ConfigError(msg);
    }
}

}  // namespace ticker
