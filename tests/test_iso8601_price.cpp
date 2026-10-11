#include "doctest.h"
#include "iso8601.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace ticker;

TEST_CASE("parse_iso8601_ns: real Coinbase timestamp") {
    auto ns = parse_iso8601_ns("2026-09-21T09:50:22.531463080Z");
    REQUIRE(ns.has_value());
    // Cross-check against a known epoch-seconds value for 2026-09-21T09:50:22Z
    // computed independently (date -u -d '2026-09-21T09:50:22Z' +%s == 1789984222).
    int64_t expected_ns = 1789984222LL * 1'000'000'000LL + 531463080LL;
    CHECK(*ns == expected_ns);
}

TEST_CASE("parse_iso8601_ns: epoch") {
    auto ns = parse_iso8601_ns("1970-01-01T00:00:00.000000000Z");
    REQUIRE(ns.has_value());
    CHECK(*ns == 0);
}

TEST_CASE("parse_iso8601_ns: no fractional part") {
    auto ns = parse_iso8601_ns("1970-01-01T00:00:01Z");
    REQUIRE(ns.has_value());
    CHECK(*ns == 1'000'000'000LL);
}

TEST_CASE("parse_iso8601_ns: fewer than 9 fractional digits") {
    auto ns = parse_iso8601_ns("1970-01-01T00:00:00.5Z");
    REQUIRE(ns.has_value());
    CHECK(*ns == 500'000'000LL);
}

TEST_CASE("parse_iso8601_ns: leap year Feb 29") {
    auto ns = parse_iso8601_ns("2024-02-29T00:00:00Z");
    REQUIRE(ns.has_value());
    auto ns_next = parse_iso8601_ns("2024-03-01T00:00:00Z");
    REQUIRE(ns_next.has_value());
    CHECK(*ns_next - *ns == 86400LL * 1'000'000'000LL);
}

TEST_CASE("parse_iso8601_ns: rejects malformed timestamps") {
    CHECK_FALSE(parse_iso8601_ns("").has_value());
    CHECK_FALSE(parse_iso8601_ns("2026-09-21T09:50:22.531463080").has_value());  // no Z
    CHECK_FALSE(parse_iso8601_ns("2026/09/21T09:50:22Z").has_value());
    CHECK_FALSE(parse_iso8601_ns("2026-13-21T09:50:22Z").has_value());  // bad month
    CHECK_FALSE(parse_iso8601_ns("2026-09-21T25:50:22Z").has_value());  // bad hour
    CHECK_FALSE(parse_iso8601_ns("garbage").has_value());
}

TEST_CASE("format_iso8601_ns: round-trips real Coinbase timestamps exactly") {
    for (const char* s : {"2026-09-21T09:50:22.531463080Z", "1970-01-01T00:00:00.000000000Z",
                           "2024-02-29T00:00:00.000000000Z", "2026-01-01T23:59:59.999999999Z"}) {
        auto ns = parse_iso8601_ns(s);
        REQUIRE(ns.has_value());
        char buf[40];
        int n = format_iso8601_ns(*ns, buf);
        CHECK(std::string_view(buf, n) == s);
    }
}

TEST_CASE("format_iso8601_ns: handles pre-epoch (negative) nanosecond counts") {
    auto ns = parse_iso8601_ns("1969-12-31T23:59:59.000000000Z");
    REQUIRE(ns.has_value());
    CHECK(*ns < 0);
    char buf[40];
    int n = format_iso8601_ns(*ns, buf);
    CHECK(std::string_view(buf, n) == "1969-12-31T23:59:59.000000000Z");
}

TEST_CASE("parse_iso8601_ns: monotonic across a day boundary") {
    auto a = parse_iso8601_ns("2026-01-01T23:59:59.999999999Z");
    auto b = parse_iso8601_ns("2026-01-02T00:00:00.000000000Z");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*b > *a);
    CHECK(*b - *a == 1);
}

// -- Fast-path (27/30-char, 6/9-fractional-digit) parity tests -----------
// parse_iso8601_ns_fast (src/iso8601.hpp) exists purely as a performance
// optimization inside the public parse_iso8601_ns: it must never return a
// value the general parser (parse_iso8601_ns_general) wouldn't also
// return for the same input, since any fast-path nullopt silently falls
// through to the general parser and becomes the authoritative answer. That
// invariant -- "fast, when it answers at all, answers exactly like
// general would" -- is what these tests check directly against both
// internal functions (ticker::detail::*), rather than only through the
// composed public parse_iso8601_ns, so a regression that made the fast
// path wrong-but-still-non-null wouldn't be masked by the fallback.

namespace {

// Formats a synthetic ISO-8601 timestamp with an exact fractional-digit
// count, for feeding to both the fast and general parsers. Deliberately
// does not reuse format_iso8601_ns (which always emits 9 digits) -- this
// needs to control the digit count itself to hit both fast-path shapes.
std::string make_ts(int64_t year, int month, int day, int hour, int minute, int sec,
                     int64_t frac, int frac_digits) {
    char buf[48];
    int n = std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02d", (long long)year,
                            month, day, hour, minute, sec);
    std::string s(buf, static_cast<size_t>(n));
    if (frac_digits > 0) {
        char fbuf[16];
        std::snprintf(fbuf, sizeof(fbuf), ".%0*lld", frac_digits, (long long)frac);
        s += fbuf;
    }
    s += "Z";
    return s;
}

// Asserts the fast/general parity invariant for one input string: if fast
// answers, general must answer the same value; fast is always allowed to
// defer (return nullopt) regardless of what general does.
void check_parity(const std::string& s) {
    auto fast = detail::parse_iso8601_ns_fast(s);
    auto general = detail::parse_iso8601_ns_general(s);
    if (fast.has_value()) {
        REQUIRE(general.has_value());
        CHECK(*fast == *general);
    }
    // Composed public function must match whichever of the two actually answers.
    auto composed = parse_iso8601_ns(s);
    CHECK(composed == (fast.has_value() ? fast : general));
}

}  // namespace

TEST_CASE("parse_iso8601_ns_fast: exactly 6 fractional digits (the other real-world shape)") {
    std::string s = "2026-09-21T09:50:22.531463Z";
    CHECK(s.size() == 27);
    auto fast = detail::parse_iso8601_ns_fast(s);
    REQUIRE(fast.has_value());
    auto general = detail::parse_iso8601_ns_general(s);
    REQUIRE(general.has_value());
    CHECK(*fast == *general);
    CHECK(parse_iso8601_ns(s) == fast);
}

TEST_CASE("parse_iso8601_ns_fast: exactly 9 fractional digits hits the fast path") {
    std::string s = "2026-09-21T09:50:22.531463080Z";
    CHECK(s.size() == 30);
    // The public function's own contract test already checks the value;
    // this checks it specifically went through the fast path, not just
    // that *some* path produced the right answer.
    CHECK(detail::parse_iso8601_ns_fast(s).has_value());
    check_parity(s);
}

TEST_CASE("parse_iso8601_ns_fast: random valid timestamps agree with the general parser") {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int64_t> year_d(0, 9999);
    std::uniform_int_distribution<int> month_d(1, 12), day_d(1, 31), hour_d(0, 23), minute_d(0, 59),
        sec_d(0, 60);  // 60 = documented leap-second allowance, same bound both parsers use
    for (int trial = 0; trial < 20000; ++trial) {
        int frac_digits = (trial % 2 == 0) ? 6 : 9;
        int64_t frac_max = 1;
        for (int i = 0; i < frac_digits; ++i) frac_max *= 10;
        std::uniform_int_distribution<int64_t> frac_d(0, frac_max - 1);
        std::string s = make_ts(year_d(rng), month_d(rng), day_d(rng), hour_d(rng), minute_d(rng),
                                 sec_d(rng), frac_d(rng), frac_digits);
        REQUIRE((s.size() == 27 || s.size() == 30));
        auto fast = detail::parse_iso8601_ns_fast(s);
        CHECK(fast.has_value());  // every one of these is fast-path-eligible and valid
        check_parity(s);
    }
}

TEST_CASE("parse_iso8601_ns_fast: random single-byte mutations either both reject or agree") {
    std::mt19937 rng(5678);
    std::uniform_int_distribution<int> pos_d;
    std::uniform_int_distribution<int> byte_d(0, 255);
    for (int trial = 0; trial < 20000; ++trial) {
        int frac_digits = (trial % 2 == 0) ? 6 : 9;
        std::string s = make_ts(2026, 1 + trial % 12, 1 + trial % 28, trial % 24, trial % 60,
                                 trial % 61, trial % 1000, frac_digits);
        pos_d = std::uniform_int_distribution<int>(0, static_cast<int>(s.size()) - 1);
        s[static_cast<size_t>(pos_d(rng))] = static_cast<char>(byte_d(rng));
        check_parity(s);
    }
}

TEST_CASE("parse_iso8601_ns_fast: non-digit byte in every fixed digit position is safely rejected") {
    const std::string base = "2026-09-21T09:50:22.531463080Z";
    REQUIRE(base.size() == 30);
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (c < '0' || c > '9') continue;  // only mutate digit positions
        std::string s = base;
        s[i] = 'x';
        check_parity(s);  // must not produce a wrong value; nullopt-vs-nullopt or matching Some is fine
    }
}

TEST_CASE("parse_iso8601_ns_fast: only exact fraction-digit counts 6 and 9 use the fast path") {
    // Build "2026-09-21T09:50:22." + N digits + "Z" for N = 0..12: total
    // length is 21+N, so this sweeps lengths 21..33 -- the general
    // parser accepts any N (it's genuinely variable-length), but the
    // fast path must only ever engage at N=6 (len 27) or N=9 (len 30),
    // and must defer correctly (matching general exactly) everywhere else.
    const std::string prefix = "2026-09-21T09:50:22.";
    for (int frac_digits = 0; frac_digits <= 12; ++frac_digits) {
        std::string s = (frac_digits == 0) ? "2026-09-21T09:50:22Z"
                                            : prefix + std::string(static_cast<size_t>(frac_digits), '5') + "Z";
        check_parity(s);
        bool should_be_fast_eligible = (frac_digits == 6 || frac_digits == 9);
        CHECK(detail::parse_iso8601_ns_fast(s).has_value() == should_be_fast_eligible);
    }
}

// -- Iso8601Formatter parity tests ----------------------------------------
// Iso8601Formatter caches the "YYYY-MM-DD" prefix keyed on the integer day
// number; these tests check it's byte-for-byte identical to the
// uncached format_iso8601_ns() across day-boundary crossings, cache hits
// and misses, and deliberately non-monotonic call sequences -- nothing
// about the cache is allowed to assume calls arrive in timestamp order.

TEST_CASE("Iso8601Formatter: matches format_iso8601_ns on repeated real timestamps") {
    Iso8601Formatter fmt;
    for (const char* s : {"2026-09-21T09:50:22.531463080Z", "2026-09-21T09:50:23.000000000Z",
                           "2026-09-21T09:50:23.999999999Z", "1970-01-01T00:00:00.000000000Z"}) {
        auto ns = parse_iso8601_ns(s);
        REQUIRE(ns.has_value());
        char expected[40], got[40];
        int n_expected = format_iso8601_ns(*ns, expected);
        int n_got = fmt.format(*ns, got);
        REQUIRE(n_expected == n_got);
        CHECK(std::string_view(expected, static_cast<size_t>(n_expected)) ==
              std::string_view(got, static_cast<size_t>(n_got)));
    }
}

TEST_CASE("Iso8601Formatter: cache hit across a day boundary stays correct") {
    Iso8601Formatter fmt;
    auto a = parse_iso8601_ns("2026-01-01T23:59:59.999999999Z");
    auto b = parse_iso8601_ns("2026-01-02T00:00:00.000000000Z");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    char buf_a[40], buf_b[40], ref_a[40], ref_b[40];
    int na = fmt.format(*a, buf_a);   // cache miss: populates 2026-01-01
    int nb = fmt.format(*b, buf_b);   // cache miss again: day changed to 2026-01-02
    int nra = format_iso8601_ns(*a, ref_a);
    int nrb = format_iso8601_ns(*b, ref_b);
    CHECK(std::string_view(buf_a, static_cast<size_t>(na)) == std::string_view(ref_a, static_cast<size_t>(nra)));
    CHECK(std::string_view(buf_b, static_cast<size_t>(nb)) == std::string_view(ref_b, static_cast<size_t>(nrb)));
}

TEST_CASE("Iso8601Formatter: non-monotonic (out-of-order) call sequence -- cache assumes nothing about ordering") {
    std::mt19937 rng(999);
    // Timestamps spanning several days, in a fixed but shuffled order fed
    // to one formatter instance -- if the cache incorrectly assumed
    // monotonic calls (e.g. only checking "did the day advance", not "did
    // it change at all"), a later out-of-order call landing on an
    // earlier-seen day would be formatted wrong.
    std::vector<int64_t> timestamps;
    for (int day = 0; day < 20; ++day) {
        for (int sec_offset : {0, 3600, 43199, 86399}) {
            timestamps.push_back(day * 86400LL * 1'000'000'000LL + sec_offset * 1'000'000'000LL +
                                   (day * 7919) % 1'000'000'000LL);
        }
    }
    std::shuffle(timestamps.begin(), timestamps.end(), rng);

    Iso8601Formatter fmt;
    for (int64_t ns : timestamps) {
        char got[40], expected[40];
        int n_got = fmt.format(ns, got);
        int n_expected = format_iso8601_ns(ns, expected);
        REQUIRE(n_got == n_expected);
        CHECK(std::string_view(got, static_cast<size_t>(n_got)) ==
              std::string_view(expected, static_cast<size_t>(n_expected)));
    }
}

TEST_CASE("Iso8601Formatter: random timestamps, fresh formatter per call (all cache misses)") {
    std::mt19937 rng(111);
    // int64 nanoseconds-since-epoch can only represent roughly +-292
    // years around 1970 (int64_t's range / 1e9 ns/s / ~3.15e7 s/year) --
    // years outside [0, 9999] (the formatter's fast-path assumption) are
    // therefore never actually reachable through this type, so this
    // range is chosen to safely avoid signed-overflow in the test's own
    // literal arithmetic while still spanning positive and pre-epoch
    // (negative-year-adjacent) dates.
    std::uniform_int_distribution<int64_t> ns_d(-280LL * 365 * 86400 * 1'000'000'000LL,
                                                  280LL * 365 * 86400 * 1'000'000'000LL);
    for (int i = 0; i < 5000; ++i) {
        int64_t ns = ns_d(rng);
        Iso8601Formatter fmt;  // fresh instance: every call is a cache miss
        char got[40], expected[40];
        int n_got = fmt.format(ns, got);
        int n_expected = format_iso8601_ns(ns, expected);
        REQUIRE(n_got == n_expected);
        CHECK(std::string_view(got, static_cast<size_t>(n_got)) ==
              std::string_view(expected, static_cast<size_t>(n_expected)));
    }
}

TEST_CASE("parse_iso8601_ns_fast: leap year and sec==60 via the fast path") {
    check_parity("2024-02-29T00:00:00.123456Z");     // leap day, 6 digits
    check_parity("2024-02-29T00:00:00.123456789Z");  // leap day, 9 digits
    check_parity("2026-06-30T23:59:60.000000Z");      // documented leap-second allowance
    check_parity("2000-02-29T12:00:00.999999999Z");   // divisible-by-400 leap year
    check_parity("1900-02-28T12:00:00.999999999Z");   // divisible-by-100-not-400: not a leap year
}
