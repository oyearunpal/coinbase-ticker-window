#pragma once
// Parses and formats Coinbase's UTC timestamps, e.g.
//   "2026-09-21T09:50:22.531463080Z"
// as int64 nanoseconds since the Unix epoch. Only the "Z" (UTC) form with
// 0-9 fractional digits is supported; no timezone offsets, no allocation.
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "digits.hpp"

namespace ticker {

inline constexpr int64_t kNsPerSec = 1'000'000'000LL;
inline constexpr int64_t kSecsPerDay = 86'400;
inline constexpr int64_t kNsPerDay = kSecsPerDay * kNsPerSec;

// Days since the Unix epoch for a proleptic-Gregorian date. Howard Hinnant's
// days_from_civil (http://howardhinnant.github.io/date_algorithms.html):
// a few integer ops, no timezone dependency. Constants: 146097 = days per
// 400-year era; 719468 = days from 0000-03-01 to 1970-01-01.
inline constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

namespace detail {

// Byte offsets in "YYYY-MM-DDTHH:MM:SS.fffffffffZ".
inline constexpr size_t kFracDot = 19;    // '.' (or 'Z' with no fraction)
inline constexpr size_t kFracStart = 20;  // first fractional digit
inline constexpr size_t kMinLen = 20;     // "YYYY-MM-DDTHH:MM:SSZ"
inline constexpr size_t kLenMicros = 27;  // 6 fractional digits
inline constexpr size_t kLenNanos = 30;   // 9 fractional digits

// General parser: 0-9 fractional digits. Defines the accepted format; the
// fast path below only handles a subset and defers to this for the rest.
inline std::optional<int64_t> parse_iso8601_ns_general(std::string_view s) noexcept {
    // Fixed layout: YYYY-MM-DDTHH:MM:SS[.fraction]Z  (fraction 0-9 digits)
    if (s.size() < kMinLen) return std::nullopt;
    auto digit = [](char c) -> int { return (c >= '0' && c <= '9') ? c - '0' : -1; };

    auto read_n = [&](size_t pos, int n) -> int64_t {
        int64_t v = 0;
        for (int k = 0; k < n; ++k) {
            int dgt = digit(s[pos + k]);
            if (dgt < 0) return -1;
            v = v * 10 + dgt;
        }
        return v;
    };

    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
        return std::nullopt;

    int64_t year = read_n(0, 4);
    int64_t month = read_n(5, 2);
    int64_t day = read_n(8, 2);
    int64_t hour = read_n(11, 2);
    int64_t minute = read_n(14, 2);
    int64_t sec = read_n(17, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 60)
        return std::nullopt;

    size_t i = kFracDot;
    int64_t nanos = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int64_t scale = 100'000'000LL;  // first fractional digit is 1e-1 s = 1e8 ns
        while (i < s.size() && s[i] != 'Z') {
            int dgt = digit(s[i]);
            if (dgt < 0) return std::nullopt;
            if (scale > 0) {
                nanos += dgt * scale;
                scale /= 10;
            }
            // digits beyond nanosecond resolution are truncated (documented)
            ++i;
        }
    }
    if (i >= s.size() || s[i] != 'Z' || i + 1 != s.size()) return std::nullopt;

    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int64_t secs_of_day = hour * 3600 + minute * 60 + sec;
    int64_t total_ns = (days * kSecsPerDay + secs_of_day) * kNsPerSec + nanos;
    return total_ns;
}

// Fast path for the two lengths Coinbase actually sends: 6 or 9 fractional
// digits. Anything else returns nullopt and the general parser handles it,
// so this may be conservative but must never accept a wrong value.
inline std::optional<int64_t> parse_iso8601_ns_fast(std::string_view s) noexcept {
    const size_t n = s.size();
    if (n != kLenMicros && n != kLenNanos) return std::nullopt;

    const char* p = s.data();
    if (p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' || p[16] != ':' ||
        p[kFracDot] != '.' || p[n - 1] != 'Z')
        return std::nullopt;

    // Appends the digit at p[pos] to `out`; false if it is not a digit.
    auto dv = [&](size_t pos, int64_t& out) -> bool {
        int d = p[pos] - '0';
        if (d < 0 || d > 9) return false;
        out = out * 10 + d;
        return true;
    };

    int64_t year = 0;
    if (!dv(0, year) || !dv(1, year) || !dv(2, year) || !dv(3, year)) return std::nullopt;
    int64_t month = 0;
    if (!dv(5, month) || !dv(6, month)) return std::nullopt;
    int64_t day = 0;
    if (!dv(8, day) || !dv(9, day)) return std::nullopt;
    int64_t hour = 0;
    if (!dv(11, hour) || !dv(12, hour)) return std::nullopt;
    int64_t minute = 0;
    if (!dv(14, minute) || !dv(15, minute)) return std::nullopt;
    int64_t sec = 0;
    if (!dv(17, sec) || !dv(18, sec)) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || sec > 60)
        return std::nullopt;

    int64_t frac = 0;
    for (size_t i = kFracStart; i < n - 1; ++i)
        if (!dv(i, frac)) return std::nullopt;
    const int64_t nanos = (n == kLenMicros) ? frac * 1000 : frac;  // micros -> nanos

    const int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t secs_of_day = hour * 3600 + minute * 60 + sec;
    return (days * kSecsPerDay + secs_of_day) * kNsPerSec + nanos;
}

}  // namespace detail

inline std::optional<int64_t> parse_iso8601_ns(std::string_view s) noexcept {
    if (auto fast = detail::parse_iso8601_ns_fast(s)) [[likely]] return fast;
    return detail::parse_iso8601_ns_general(s);
}

// Inverse of days_from_civil (Hinnant's civil_from_days).
inline constexpr void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);              // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                        // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                                // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                                     // [1, 12]
    y = yr + (m <= 2 ? 1 : 0);
}

// Formats ns-since-epoch as "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" (always 9
// fractional digits). `out` must hold at least 32 bytes; returns bytes written.
inline int format_iso8601_ns(int64_t total_ns, char* out) noexcept {
    int64_t days = total_ns / kNsPerDay;
    int64_t rem_ns = total_ns % kNsPerDay;
    if (rem_ns < 0) { rem_ns += kNsPerDay; days -= 1; }

    int64_t secs_of_day = rem_ns / kNsPerSec;
    int64_t nanos = rem_ns % kNsPerSec;
    int hour = static_cast<int>(secs_of_day / 3600);
    int minute = static_cast<int>((secs_of_day / 60) % 60);
    int sec = static_cast<int>(secs_of_day % 60);

    int64_t y;
    unsigned mo, d;
    civil_from_days(days, y, mo, d);

    char* p = out;
    // Year may be negative or longer than 4 digits; zero-pad to at least 4.
    {
        char buf[8];
        int n = 0;
        bool neg = y < 0;
        uint64_t mag = neg ? static_cast<uint64_t>(-y) : static_cast<uint64_t>(y);
        if (mag == 0) buf[n++] = '0';
        while (mag > 0) { buf[n++] = char('0' + (mag % 10)); mag /= 10; }
        while (n < 4) buf[n++] = '0';
        if (neg) *p++ = '-';
        for (int k = n - 1; k >= 0; --k) *p++ = buf[k];
    }
    *p++ = '-'; digits::put2(p, static_cast<int>(mo)); p += 2;
    *p++ = '-'; digits::put2(p, static_cast<int>(d)); p += 2;
    *p++ = 'T'; digits::put2(p, hour); p += 2;
    *p++ = ':'; digits::put2(p, minute); p += 2;
    *p++ = ':'; digits::put2(p, sec); p += 2;
    *p++ = '.';
    // 9 fraction digits as three 3-digit groups.
    digits::put3(p, static_cast<int>(nanos / 1'000'000)); p += 3;
    digits::put3(p, static_cast<int>((nanos / 1000) % 1000)); p += 3;
    digits::put3(p, static_cast<int>(nanos % 1000)); p += 3;
    *p++ = 'Z';
    return static_cast<int>(p - out);
}

// format_iso8601_ns() with the "YYYY-MM-DD" part cached by day number, since
// consecutive timestamps almost always share a day. An object rather than a
// function-local static so it stays reentrant; the CSV writer uses one per
// timestamp column so the two streams don't evict each other.
class Iso8601Formatter {
  public:
    // Same contract and byte-identical output as format_iso8601_ns()
    // (tested in tests/test_iso8601_price.cpp, including out-of-order calls).
    int format(int64_t total_ns, char* out) noexcept {
        int64_t days = total_ns / kNsPerDay;
        int64_t rem_ns = total_ns % kNsPerDay;
        if (rem_ns < 0) {
            rem_ns += kNsPerDay;
            days -= 1;
        }

        if (!has_cache_ || days != cached_days_) {
            int64_t y;
            unsigned mo, d;
            civil_from_days(days, y, mo, d);
            if (y < 0 || y > 9999) {
                // Cache assumes a 4-digit year; defer to the general formatter.
                return format_iso8601_ns(total_ns, out);
            }
            digits::put2(&cached_date_[0], static_cast<int>(y / 100));
            digits::put2(&cached_date_[2], static_cast<int>(y % 100));
            cached_date_[4] = '-';
            digits::put2(&cached_date_[5], static_cast<int>(mo));
            cached_date_[7] = '-';
            digits::put2(&cached_date_[8], static_cast<int>(d));
            cached_days_ = days;
            has_cache_ = true;
        }

        char* p = out;
        std::memcpy(p, cached_date_, sizeof(cached_date_));
        p += sizeof(cached_date_);
        *p++ = 'T';
        const int64_t secs_of_day = rem_ns / kNsPerSec;
        const int hour = static_cast<int>(secs_of_day / 3600);
        const int minute = static_cast<int>((secs_of_day / 60) % 60);
        const int sec = static_cast<int>(secs_of_day % 60);
        const int64_t nanos = rem_ns % kNsPerSec;
        digits::put2(p, hour); p += 2; *p++ = ':';
        digits::put2(p, minute); p += 2; *p++ = ':';
        digits::put2(p, sec); p += 2; *p++ = '.';
        digits::put3(p, static_cast<int>(nanos / 1'000'000)); p += 3;
        digits::put3(p, static_cast<int>((nanos / 1000) % 1000)); p += 3;
        digits::put3(p, static_cast<int>(nanos % 1000)); p += 3;
        *p++ = 'Z';
        return static_cast<int>(p - out);
    }

  private:
    bool has_cache_ = false;
    int64_t cached_days_ = 0;
    char cached_date_[10];  // "YYYY-MM-DD", not NUL-terminated
};

}  // namespace ticker
