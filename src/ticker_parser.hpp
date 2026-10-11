#pragma once
// Zero-copy, zero-allocation parser for Coinbase `ticker` messages. Not a
// general JSON parser: it extracts type, product_id, price, trade_id, time
// and (optionally) low_24h/high_24h, and skips every other field.
//
// Example frame:
//   {"type":"ticker","sequence":103139911985,"product_id":"ETH-USD",
//    "price":"2716.18", ... ,"time":"2026-09-21T10:58:36.485337Z",
//    "trade_id":844419776,"last_size":"0.36180165"}
//
// No backslash-escape handling: no field in this schema contains one.
//
// parse_ticker() tries scan_fast() (Coinbase's exact field order, checked
// byte-for-byte) and falls back to scan_generic() (any field order) on any
// mismatch. Both feed the same build_ticker(), and tests/test_parser.cpp
// checks the two scans agree on every fixture frame and every single-byte
// mutation of it, so the fast path can be slower but never wrong.
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "fixed_price.hpp"
#include "iso8601.hpp"
#include "probe.hpp"

namespace ticker {

struct Ticker {
    std::string_view product_id;  // aliases the input buffer
    Price price;
    int64_t trade_id;
    int64_t event_ts_ns;
    // Raw text, aliasing the input buffer; empty if absent. Parsed lazily via
    // low_24h()/high_24h() because only the range/hybrid policies use them,
    // and only until a symbol's band is established.
    std::string_view low_24h_str;
    std::string_view high_24h_str;

    // nullopt if absent or malformed (e.g. scientific notation "1.2e-7").
    std::optional<Price> low_24h() const noexcept { return parse_price(low_24h_str); }
    std::optional<Price> high_24h() const noexcept { return parse_price(high_24h_str); }

    bool operator==(const Ticker&) const = default;
};

namespace detail {

inline void skip_ws(std::string_view s, size_t& i) noexcept {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// The small helpers below are [[gnu::always_inline]] because GCC -O3 left
// them as out-of-line calls (see docs/benchmarks.md).

// Returns the index of the next '"' at or after `from`, or s.size() if none.
// memchr, not strchr: string_view data is not NUL-terminated.
// Precondition: from <= s.size().
[[gnu::always_inline]] inline size_t find_quote(std::string_view s, size_t from) noexcept {
    assert(from <= s.size());
    const void* p = std::memchr(s.data() + from, '"', s.size() - from);
    return p ? static_cast<size_t>(static_cast<const char*>(p) - s.data()) : s.size();
}

// Parses a quoted string at s[i] == '"' and advances i past the closing quote.
[[gnu::always_inline]] inline std::optional<std::string_view> parse_string_value(std::string_view s, size_t& i) noexcept {
    if (i >= s.size() || s[i] != '"') [[unlikely]] return std::nullopt;
    size_t start = ++i;
    i = find_quote(s, i);
    if (i >= s.size()) [[unlikely]] return std::nullopt;
    std::string_view v(s.data() + start, i - start);
    ++i;  // past closing quote
    return v;
}

// Parses a bare JSON integer (no quotes) at s[i], advances i past it.
[[gnu::always_inline]] inline std::optional<int64_t> parse_number_value(std::string_view s, size_t& i) noexcept {
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == start) [[unlikely]] return std::nullopt;
    int64_t v = 0;
    auto res = std::from_chars(s.data() + start, s.data() + i, v);
    if (res.ec != std::errc{}) [[unlikely]] return std::nullopt;
    return v;
}

// Advances i past a bare JSON integer without converting it, for a value that is only checked
// for shape. Accepts what parse_number_value's scan accepts: an optional '-' and one or more
// digits.
[[gnu::always_inline]] inline bool skip_number(std::string_view s, size_t& i) noexcept {
    if (i < s.size() && s[i] == '-') ++i;
    const size_t digits_start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    return i > digits_start;
}

// Skips one JSON value of any type, including nested objects/arrays, so an
// unexpected schema addition is skipped rather than desyncing the parse.
inline bool skip_value(std::string_view s, size_t& i) noexcept {
    if (i >= s.size()) return false;
    if (s[i] == '"') {
        ++i;
        i = find_quote(s, i);
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    if (s[i] == '{' || s[i] == '[') {
        char open = s[i], close = (open == '{') ? '}' : ']';
        int depth = 1;
        ++i;
        while (i < s.size() && depth > 0) {
            if (s[i] == '"') {
                ++i;
                i = find_quote(s, i);
                if (i >= s.size()) return false;
            } else if (s[i] == open) {
                ++depth;
            } else if (s[i] == close) {
                --depth;
            }
            ++i;
        }
        return depth == 0;
    }
    // number / true / false / null: run to the next structural character
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return true;
}

// Switches on key length first so most keys need at most one or two string
// compares. Case labels are key lengths: 4 = type/time, 5 = price,
// 7 = low_24h, 8 = trade_id/high_24h, 10 = product_id.
enum class FieldKey { Type, ProductId, Price, TradeId, Time, Low24h, High24h, Other };

inline FieldKey classify_key(std::string_view key) noexcept {
    switch (key.size()) {
        case 4:
            if (key == "type") return FieldKey::Type;
            if (key == "time") return FieldKey::Time;
            return FieldKey::Other;
        case 5:
            if (key == "price") return FieldKey::Price;
            return FieldKey::Other;
        case 7:
            if (key == "low_24h") return FieldKey::Low24h;
            return FieldKey::Other;
        case 8:
            if (key == "trade_id") return FieldKey::TradeId;
            if (key == "high_24h") return FieldKey::High24h;
            return FieldKey::Other;  // e.g. "sequence"
        case 10:
            if (key == "product_id") return FieldKey::ProductId;
            return FieldKey::Other;  // e.g. "volume_24h"
        default:
            return FieldKey::Other;
    }
}

// Raw field slices and presence flags produced by either scan. Everything
// after the scan (build_ticker) reads only this, so the two scans can differ
// only in how they locate fields. Empty low/high strings mean "absent".
struct RawFields {
    std::string_view product_id;
    std::string_view price_str;
    std::string_view time_str;
    std::string_view low_24h_str;
    std::string_view high_24h_str;
    int64_t trade_id = 0;
    bool have_type_ticker = false;
    bool have_product = false, have_price = false, have_trade_id = false, have_time = false;

    bool operator==(const RawFields&) const = default;
};

// Order-independent scan: accepts any field order, extra fields and
// whitespace. Used when scan_fast() does not match.
template <typename Tracer>
inline std::optional<RawFields> scan_generic(std::string_view msg, Tracer& tracer) noexcept {
    size_t i = 0;
    skip_ws(msg, i);
    if (i >= msg.size() || msg[i] != '{') return std::nullopt;
    ++i;

    RawFields f;
    for (;;) {
        skip_ws(msg, i);
        if (i >= msg.size()) return std::nullopt;
        if (msg[i] == '}') break;
        if (msg[i] == ',') { ++i; continue; }
        if (msg[i] != '"') return std::nullopt;

        size_t key_start = ++i;
        i = find_quote(msg, i);
        if (i >= msg.size()) return std::nullopt;
        std::string_view key(msg.data() + key_start, i - key_start);
        ++i;
        skip_ws(msg, i);
        if (i >= msg.size() || msg[i] != ':') return std::nullopt;
        ++i;
        skip_ws(msg, i);

        switch (classify_key(key)) {
            case FieldKey::Type: {
                auto scope = tracer.enter(ProbeId::ParseTypecheck);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;
                if (*v != "ticker") return std::nullopt;  // fast reject: heartbeat/subscriptions/error/...
                f.have_type_ticker = true;
                break;
            }
            case FieldKey::ProductId: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;
                f.product_id = *v;
                f.have_product = true;
                break;
            }
            case FieldKey::Price: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;
                f.price_str = *v;
                f.have_price = true;
                break;
            }
            case FieldKey::TradeId: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_number_value(msg, i);
                if (!v) return std::nullopt;
                f.trade_id = *v;
                f.have_trade_id = true;
                break;
            }
            case FieldKey::Time: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;
                f.time_str = *v;
                f.have_time = true;
                break;
            }
            case FieldKey::Low24h: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;  // malformed JSON here still desyncs the parse
                f.low_24h_str = *v;
                break;
            }
            case FieldKey::High24h: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                auto v = parse_string_value(msg, i);
                if (!v) return std::nullopt;
                f.high_24h_str = *v;
                break;
            }
            case FieldKey::Other: {
                auto scope = tracer.enter(ProbeId::ParseScan);
                if (!skip_value(msg, i)) return std::nullopt;
                break;
            }
        }
    }
    return f;
}

// Equality of L bytes, L a compile-time constant. GCC expands memcmp inline only for short
// lengths and calls the library for scan_fast's 12-17 byte keys; this compares 8-byte words
// (the last one overlapping) and ORs the differences, with no call and no branch per word.
template <size_t L>
[[gnu::always_inline]] inline bool bytes_equal(const char* a, const char* b) noexcept {
    constexpr size_t kWord = sizeof(uint64_t);
    if constexpr (L < kWord) {
        return std::memcmp(a, b, L) == 0;
    } else {
        auto load = [](const char* p) noexcept {
            uint64_t v;
            std::memcpy(&v, p, kWord);
            return v;
        };
        uint64_t diff = 0;
        for (size_t k = 0; k + kWord < L; k += kWord) diff |= load(a + k) ^ load(b + k);
        diff |= load(a + L - kWord) ^ load(b + L - kWord);
        return diff == 0;
    }
}

// Matches Coinbase's exact field order byte-for-byte. Any difference (field
// order, extra field, whitespace, a non-"ticker" type) returns nullopt and
// parse_ticker() falls back to scan_generic(). Values are read with the same
// helpers scan_generic() uses. Probed as a single ParseScan scope by the caller.
inline std::optional<RawFields> scan_fast(std::string_view msg) noexcept {
    size_t i = 0;
    // Matches literal `lit` at msg[i..) and advances past it. Every step below
    // leaves i <= msg.size(), so checking the literal fits is enough to stay
    // in bounds. `lit` is an array so its length is a compile-time constant.
    auto expect = [&]<size_t N>(const char (&lit)[N]) noexcept __attribute__((always_inline)) -> bool {
        constexpr size_t len = N - 1;  // drop the literal's NUL
        if (len > msg.size() - i) [[unlikely]] return false;
        if (!bytes_equal<len>(msg.data() + i, lit)) [[unlikely]] return false;
        i += len;
        return true;
    };

    if (!expect(R"({"type":"ticker",)")) return std::nullopt;
    if (!expect(R"("sequence":)")) return std::nullopt;
    // sequence: unused, so only its shape is checked (scan_generic skips it as an unknown field).
    if (!skip_number(msg, i)) [[unlikely]] return std::nullopt;

    if (!expect(R"(,"product_id":)")) return std::nullopt;
    auto product_id = parse_string_value(msg, i);
    if (!product_id) return std::nullopt;

    if (!expect(R"(,"price":)")) return std::nullopt;
    auto price_str = parse_string_value(msg, i);
    if (!price_str) return std::nullopt;

    if (!expect(R"(,"open_24h":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"volume_24h":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"low_24h":)")) return std::nullopt;
    auto low_24h = parse_string_value(msg, i);
    if (!low_24h) return std::nullopt;

    if (!expect(R"(,"high_24h":)")) return std::nullopt;
    auto high_24h = parse_string_value(msg, i);
    if (!high_24h) return std::nullopt;

    if (!expect(R"(,"volume_30d":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"best_bid":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"best_bid_size":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"best_ask":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"best_ask_size":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"side":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect(R"(,"time":)")) return std::nullopt;
    auto time_str = parse_string_value(msg, i);
    if (!time_str) return std::nullopt;

    if (!expect(R"(,"trade_id":)")) return std::nullopt;
    auto trade_id = parse_number_value(msg, i);
    if (!trade_id) return std::nullopt;

    if (!expect(R"(,"last_size":)")) return std::nullopt;
    if (!parse_string_value(msg, i)) return std::nullopt;

    if (!expect("}")) return std::nullopt;
    // No trailing bytes after the closing brace.
    if (i != msg.size()) return std::nullopt;

    RawFields f;
    f.product_id = *product_id;
    f.price_str = *price_str;
    f.low_24h_str = *low_24h;
    f.high_24h_str = *high_24h;
    f.time_str = *time_str;
    f.trade_id = *trade_id;
    f.have_type_ticker = true;
    f.have_product = true;
    f.have_price = true;
    f.have_time = true;
    f.have_trade_id = true;
    return f;
}

// Checks required fields, parses price and time, and assembles the Ticker.
// Shared by both scans.
template <typename Tracer>
inline std::optional<Ticker> build_ticker(const RawFields& f, Tracer& tracer) noexcept {
    if (!f.have_type_ticker || !f.have_product || !f.have_price || !f.have_trade_id || !f.have_time)
        return std::nullopt;

    Price price;
    {
        auto scope = tracer.enter(ProbeId::ParsePrice);
        auto p = parse_price(f.price_str);
        if (!p) return std::nullopt;
        price = *p;
    }

    int64_t event_ts_ns;
    {
        auto scope = tracer.enter(ProbeId::ParseTime);
        auto t = parse_iso8601_ns(f.time_str);
        if (!t) return std::nullopt;
        event_ts_ns = *t;
    }

    return Ticker{f.product_id, price, f.trade_id, event_ts_ns, f.low_24h_str, f.high_24h_str};
}

}  // namespace detail

template <typename Tracer>
inline std::optional<Ticker> parse_ticker(std::string_view msg, Tracer& tracer) noexcept {
    auto scope_total = tracer.enter(ProbeId::ParseTotal);

    std::optional<detail::RawFields> f;
    {
        // One scope for the whole fast scan (scan_generic probes per field).
        auto scope = tracer.enter(ProbeId::ParseScan);
        f = detail::scan_fast(msg);
    }
    if (!f) [[unlikely]] {
        f = detail::scan_generic(msg, tracer);
        if (!f) return std::nullopt;
    }

    return detail::build_ticker(*f, tracer);
}

// Convenience overload for call sites that don't care about instrumentation.
inline std::optional<Ticker> parse_ticker(std::string_view msg) noexcept {
    NullTracer t;
    return parse_ticker(msg, t);
}

// The top-level "type" value of a frame, or nullopt if it has none or isn't
// an object. Lets a caller tell a control frame (subscriptions, heartbeat,
// error) from a malformed ticker after parse_ticker() fails; not on the hot path.
inline std::optional<std::string_view> frame_type(std::string_view msg) noexcept {
    using namespace detail;
    size_t i = 0;
    skip_ws(msg, i);
    if (i >= msg.size() || msg[i] != '{') return std::nullopt;
    ++i;
    while (true) {
        skip_ws(msg, i);
        if (i >= msg.size() || msg[i] == '}') return std::nullopt;
        if (msg[i] == ',') { ++i; continue; }
        if (msg[i] != '"') return std::nullopt;
        size_t key_start = ++i;
        i = find_quote(msg, i);
        if (i >= msg.size()) return std::nullopt;
        std::string_view key(msg.data() + key_start, i - key_start);
        ++i;
        skip_ws(msg, i);
        if (i >= msg.size() || msg[i] != ':') return std::nullopt;
        ++i;
        skip_ws(msg, i);
        if (key == "type") return parse_string_value(msg, i);
        if (!skip_value(msg, i)) return std::nullopt;
    }
}

}  // namespace ticker
