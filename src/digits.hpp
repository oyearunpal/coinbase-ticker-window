#pragma once
// Lookup tables for writing fixed-width, zero-padded 2- and 3-digit fields
// (time fields, cents, nanosecond groups) with one lookup + memcpy each.
// Variable-width numbers use std::to_chars instead.
#include <array>
#include <cstring>

namespace ticker::digits {

namespace detail {

inline constexpr std::array<char, 200> make_two_digit_table() noexcept {
    std::array<char, 200> t{};
    for (int i = 0; i < 100; ++i) {
        t[static_cast<size_t>(i) * 2 + 0] = static_cast<char>('0' + i / 10);
        t[static_cast<size_t>(i) * 2 + 1] = static_cast<char>('0' + i % 10);
    }
    return t;
}

inline constexpr std::array<char, 3000> make_three_digit_table() noexcept {
    std::array<char, 3000> t{};
    for (int i = 0; i < 1000; ++i) {
        t[static_cast<size_t>(i) * 3 + 0] = static_cast<char>('0' + i / 100);
        t[static_cast<size_t>(i) * 3 + 1] = static_cast<char>('0' + (i / 10) % 10);
        t[static_cast<size_t>(i) * 3 + 2] = static_cast<char>('0' + i % 10);
    }
    return t;
}

}  // namespace detail

inline constexpr std::array<char, 200> kTwoDigits = detail::make_two_digit_table();
inline constexpr std::array<char, 3000> kThreeDigits = detail::make_three_digit_table();

// Writes v (0-99) as 2 zero-padded digits to out[0..1].
inline void put2(char* out, int v) noexcept { std::memcpy(out, &kTwoDigits[static_cast<size_t>(v) * 2], 2); }

// Writes the zero-padded 3-digit representation of v (0-999) to out[0..2].
inline void put3(char* out, int v) noexcept { std::memcpy(out, &kThreeDigits[static_cast<size_t>(v) * 3], 3); }

}  // namespace ticker::digits
