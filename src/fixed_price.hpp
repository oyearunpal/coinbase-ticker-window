#pragma once
// Fixed-point price representation for the hot path.
//
// Coinbase quotes prices as decimal strings ("84792.03"). We never convert
// them to double: floating point mean/median arithmetic accumulates rounding
// error and is not reproducible bit-for-bit across runs/platforms, which
// matters when the CSV is meant to be independently re-derivable (see
// tools/verify_csv.py).
//
// Only products quoted in whole cents are supported (quote_increment 0.01:
// BTC-USD, ETH-USD, SOL-USD), so a price is a uint32 number of cents (up to
// $42,949,672.95). One cent is one tick, so no policy needs a tick size.
// Every output value is in cents too. The two computed ones round
// half-up to a cent, so each is off by at most 0.005: the mean
// (sliding_window.hpp's round_mean) and an even-count median whose two
// middles are an odd number of cents apart (avg_price below).
#include <cassert>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

#include "digits.hpp"

namespace ticker {

using Price = uint32_t;                       // whole cents
inline constexpr uint32_t kPriceScale = 100;  // 1e-2

namespace detail {
// Midpoint of two prices, rounded half-up to a cent (off by at most 0.005).
// Shared by every policy's median() on an even-sized window. Written as
// lo + ceil((hi - lo) / 2) so it cannot overflow near UINT32_MAX.
inline constexpr Price avg_price(Price lo, Price hi) noexcept {
    assert(lo <= hi);
    const Price d = hi - lo;
    return lo + d / 2 + (d & 1);
}
}  // namespace detail

// Parses a decimal price string like "84792.03" into cents. Returns nullopt
// on malformed input (not a number, embedded garbage, a sign), on a nonzero
// digit past the cents ("0.001", "1.234"; trailing zeros such as
// "1.10000000" are fine), and on a value above UINT32_MAX cents. No
// allocation, no exceptions.
inline std::optional<Price> parse_price(std::string_view s) noexcept {
    if (s.empty()) [[unlikely]] return std::nullopt;
    size_t i = 0;

    uint64_t int_part = 0;
    size_t digits_before = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        int_part = int_part * 10 + static_cast<uint64_t>(s[i] - '0');
        ++i;
        ++digits_before;
        if (int_part > UINT32_MAX / kPriceScale) [[unlikely]] return std::nullopt;  // overflow guard
    }
    if (digits_before == 0) [[unlikely]] return std::nullopt;

    uint64_t frac_part = 0;
    uint64_t frac_scale = kPriceScale;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (frac_scale > 1) {
                frac_scale /= 10;
                frac_part = frac_part * 10 + static_cast<uint64_t>(s[i] - '0');
            } else if (s[i] != '0') [[unlikely]] {
                return std::nullopt;  // sub-cent price: not a supported product
            }
            ++i;
        }
    }
    if (i != s.size()) [[unlikely]] return std::nullopt;  // trailing garbage

    const uint64_t cents = int_part * kPriceScale + frac_part * frac_scale;
    if (cents > UINT32_MAX) [[unlikely]] return std::nullopt;
    return static_cast<Price>(cents);
}

// Max length of format_price() output: uint32 cents is at most 8 integer
// digits + '.' + 2 decimals = 11 bytes; 16 leaves room.
inline constexpr size_t kMaxPriceChars = 16;

// Formats cents with exactly 2 decimals ("84792.03") into `out` (at least
// kMaxPriceChars bytes) and returns the length. No allocation.
inline int format_price(Price cents, char* out) noexcept {
    char* p = std::to_chars(out, out + kMaxPriceChars, cents / kPriceScale).ptr;
    *p++ = '.';
    digits::put2(p, static_cast<int>(cents % kPriceScale));
    return static_cast<int>(p + 2 - out);
}

}  // namespace ticker
