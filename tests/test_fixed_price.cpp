#include "doctest.h"
#include "fixed_price.hpp"

#include <string_view>

using namespace ticker;

TEST_CASE("parse_price: basic decimals, in cents") {
    CHECK(parse_price("84792.03").value() == 8479203u);
    CHECK(parse_price("0.01").value() == 1u);
    CHECK(parse_price("0").value() == 0u);
    CHECK(parse_price("116.06").value() == 11606u);
    CHECK(parse_price("116.6").value() == 11660u);
}

TEST_CASE("parse_price: integers with no fraction") {
    CHECK(parse_price("100").value() == 100u * kPriceScale);
}

TEST_CASE("parse_price: trailing zeros past the cents are accepted") {
    CHECK(parse_price("1.10000000").value() == 110u);
    CHECK(parse_price("84792.030").value() == 8479203u);
}

TEST_CASE("parse_price: a nonzero digit past the cents is rejected, not truncated") {
    CHECK_FALSE(parse_price("0.001").has_value());
    CHECK_FALSE(parse_price("1.234").has_value());
    CHECK_FALSE(parse_price("0.0001").has_value());
    CHECK_FALSE(parse_price("1.00000001").has_value());
}

TEST_CASE("parse_price: the uint32 cents ceiling") {
    CHECK(parse_price("42949672.95").value() == UINT32_MAX);
    CHECK_FALSE(parse_price("42949672.96").has_value());
    CHECK_FALSE(parse_price("42949673").has_value());
    CHECK_FALSE(parse_price("99999999999999999999").has_value());
}

TEST_CASE("parse_price: a sign is malformed (prices are non-negative)") {
    CHECK_FALSE(parse_price("-5.5").has_value());
    CHECK_FALSE(parse_price("+5").has_value());
}

TEST_CASE("parse_price: rejects malformed input") {
    CHECK_FALSE(parse_price("").has_value());
    CHECK_FALSE(parse_price("abc").has_value());
    CHECK_FALSE(parse_price("1.2.3").has_value());
    CHECK_FALSE(parse_price(".5").has_value());
    CHECK_FALSE(parse_price("12a").has_value());
    CHECK_FALSE(parse_price("+-5").has_value());
    CHECK_FALSE(parse_price("1.2e-7").has_value());
}

TEST_CASE("parse_price: trailing dot with no fractional digits is accepted as .0") {
    // Coinbase never emits this shape, but "5." is unambiguous (== 5.0), so
    // the parser accepts it rather than rejecting a case with one clear
    // reading.
    CHECK(parse_price("5.").value() == 5u * kPriceScale);
}

TEST_CASE("avg_price: midpoint rounded half-up to a cent, no overflow at UINT32_MAX") {
    CHECK(detail::avg_price(4, 4) == 4u);
    CHECK(detail::avg_price(4, 5) == 5u);  // 4.5 -> 5
    CHECK(detail::avg_price(4, 6) == 5u);
    CHECK(detail::avg_price(10001, 10002) == 10002u);  // 100.015 -> 100.02
    CHECK(detail::avg_price(0, UINT32_MAX) == UINT32_MAX / 2 + 1);
    CHECK(detail::avg_price(UINT32_MAX - 1, UINT32_MAX) == UINT32_MAX);
    CHECK(detail::avg_price(UINT32_MAX, UINT32_MAX) == UINT32_MAX);
    for (Price a = 0; a < 40; ++a)
        for (Price b = a; b < 40; ++b) CHECK(detail::avg_price(a, b) == (a + b + 1) / 2);
}

TEST_CASE("format_price: always two decimals") {
    char buf[32];
    auto fmt = [&](Price v) { return std::string_view(buf, static_cast<size_t>(format_price(v, buf))); };
    CHECK(fmt(0) == "0.00");
    CHECK(fmt(5) == "0.05");
    CHECK(fmt(100) == "1.00");
    CHECK(fmt(8479203) == "84792.03");
    CHECK(fmt(UINT32_MAX) == "42949672.95");
}

TEST_CASE("parse_price/format_price round trip over typical prices") {
    const char* prices[] = {"84792.03", "3011.44", "116.06", "0.01", "1.00", "42949672.95"};
    for (auto* s : prices) {
        auto p = parse_price(s);
        REQUIRE(p.has_value());
        char buf[32];
        const int n = format_price(*p, buf);
        CHECK(std::string_view(buf, static_cast<size_t>(n)) == s);
    }
}
