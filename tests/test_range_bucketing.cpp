#include "doctest.h"
#include "../src/policy/range_bucketing.hpp"

#include <algorithm>
#include <random>
#include <vector>

using namespace ticker;
using namespace ticker::detail;

TEST_CASE("RangeBucketing: init rejects an inverted range") {
    RangeBucketing<> rb;
    CHECK_FALSE(rb.init(/*low=*/100, /*high=*/50));
    CHECK_FALSE(rb.valid());
}

TEST_CASE("RangeBucketing: a band that fits under the cap is EXACT -- width()==1") {
    RangeBucketing<> rb;
    // A realistic BTC-USD-shaped band in cents: $80,000.00-$80,500.00, well
    // under the default 1,000,000-bucket cap once padded.
    REQUIRE(rb.init(/*low=*/8'000'000, /*high=*/8'050'000));
    CHECK(rb.exact());
    CHECK(rb.width() == 1);
}

TEST_CASE("RangeBucketing: a band wider than the cap clamps band_buckets and reports width()>1") {
    RangeBucketing<> rb;
    const Bucket cap = 100;  // tiny cap, easy to force past
    // Span of 10,000 cents with only 100 buckets available -- must not be exact.
    REQUIRE(rb.init(/*low=*/0, /*high=*/10'000, cap));
    CHECK_FALSE(rb.exact());
    CHECK(rb.width() > 1);
    CHECK(rb.band_buckets() <= cap);
}

TEST_CASE("RangeBucketing: a single-point range (low==high, e.g. a flat 24h band) still yields a "
          "valid, exact, single-bucket-wide band") {
    RangeBucketing<> rb;
    REQUIRE(rb.init(/*low=*/1000, /*high=*/1000));
    CHECK(rb.exact());
    CHECK(rb.band_buckets() >= 1);
    CHECK(rb.in_band(1000));
}

TEST_CASE("RangeBucketing: a flat or near-flat band still gets a minimum pad, so a 1-cent move "
          "stays in band") {
    RangeBucketing<> rb;
    // Span 0 and 5: (high - low) / 10 is 0, so only the floor keeps these from escaping.
    for (const Price high : {Price{1000}, Price{1005}}) {
        REQUIRE(rb.init(/*low=*/1000, high));
        CHECK(rb.exact());
        CHECK(rb.in_band(1000 - kMinBandPad));
        CHECK(rb.in_band(high + kMinBandPad));
        CHECK_FALSE(rb.in_band(high + kMinBandPad + 1));
    }
    // An expensive product pads by 0.1% of the price rather than the absolute floor.
    REQUIRE(rb.init(/*low=*/10'000'000, /*high=*/10'000'000));
    CHECK(rb.in_band(10'000'000 - 10'000));
    CHECK(rb.in_band(10'000'000 + 10'000));
    CHECK_FALSE(rb.in_band(10'000'000 + 10'001));
}

TEST_CASE("RangeBucketing: padding below zero clamps the origin to 0") {
    RangeBucketing<> rb;
    // A cheap product that moved 10x in 24h: 10% padding of the span reaches below zero.
    REQUIRE(rb.init(/*low=*/10, /*high=*/1000));
    CHECK(rb.in_band(0));
    CHECK(rb.bucket_midpoint(rb.bucket_of(0)) == 0);
}

TEST_CASE("RangeBucketing: bucket_of is monotonically non-decreasing in Price across tails and band") {
    RangeBucketing<8> rb;  // small tail count, easy to exercise fully
    // Span 10,000 padded by 1,010 (0.1% of high) each side -> 12,020; the cap covers its 12,021
    // cents, so this is the EXACT case (checked below rather than assumed).
    REQUIRE(rb.init(/*low=*/1'000'000, /*high=*/1'010'000, /*max_band_buckets=*/20'000));
    REQUIRE(rb.exact());

    std::vector<Price> probes;
    // Deep below, near-below, inside the band, near-above, deep above.
    for (Price p : {0u, 1u, 500'000u, 999'000u, 999'999u}) probes.push_back(p);
    for (Price p = 1'000'000; p <= 1'010'000; p += 137) probes.push_back(p);
    for (Price p : {1'010'001u, 1'020'000u, 2'000'000u, 1'000'000'000u, UINT32_MAX}) probes.push_back(p);

    Bucket last_bucket = 0;
    bool first = true;
    for (Price p : probes) {
        const Bucket b = rb.bucket_of(p);
        CAPTURE(p);
        CAPTURE(b);
        if (!first) CHECK(b >= last_bucket);
        last_bucket = b;
        first = false;
    }
}

TEST_CASE("RangeBucketing: in the EXACT case every in-band price is its own bucket's midpoint, and "
          "consecutive cents land in consecutive buckets") {
    RangeBucketing<> rb;
    const Price low = 1'000'000, high = 1'001'000;
    REQUIRE(rb.init(low, high, /*max_band_buckets=*/1'000'000));
    REQUIRE(rb.exact());

    Bucket prev = 0;
    for (Price p = low; p <= high; ++p) {
        REQUIRE(rb.in_band(p));
        const Bucket b = rb.bucket_of(p);
        CAPTURE(p);
        CHECK(rb.bucket_midpoint(b) == p);
        if (p > low) CHECK(b == prev + 1);
        prev = b;
    }
}

TEST_CASE("RangeBucketing: num_buckets equals band_buckets + 2*kTailBuckets") {
    RangeBucketing<32> rb;
    REQUIRE(rb.init(/*low=*/0, /*high=*/1000, /*max_band_buckets=*/500));
    CHECK(rb.num_buckets() == rb.band_buckets() + 64);
}

TEST_CASE("RangeBucketing: tail buckets never exceed the valid bucket index range, even at the "
          "extremes of Price") {
    RangeBucketing<16> rb;
    REQUIRE(rb.init(/*low=*/1000, /*high=*/2000, /*max_band_buckets=*/100));
    const Bucket max_valid = rb.num_buckets() - 1;

    for (Price p : {0u, 1u, UINT32_MAX / 2, UINT32_MAX - 1, UINT32_MAX}) {
        // Below/above the band by an enormous margin -- must clamp into
        // the tail, not overflow or return an out-of-range index.
        const Bucket b = rb.bucket_of(p);
        CAPTURE(p);
        CHECK(b <= max_valid);
        CHECK(rb.bucket_midpoint(b) <= UINT32_MAX);
    }
}

TEST_CASE("RangeBucketing: a fuzzed sequence of random prices always returns a bucket in range and "
          "bucket_of stays monotonic in Price (property-based check, not just hand-picked points)") {
    RangeBucketing<24> rb;
    REQUIRE(rb.init(/*low=*/500'000, /*high=*/600'000, /*max_band_buckets=*/10'000));
    const Bucket max_valid = rb.num_buckets() - 1;

    std::mt19937_64 rng(123);
    std::uniform_int_distribution<Price> dist(0, 2'000'000);
    std::vector<Price> prices;
    for (int i = 0; i < 2000; ++i) prices.push_back(dist(rng));
    std::sort(prices.begin(), prices.end());

    Bucket last = 0;
    bool first = true;
    for (Price p : prices) {
        const Bucket b = rb.bucket_of(p);
        CHECK(b <= max_valid);
        if (!first) CHECK(b >= last);
        last = b;
        first = false;
    }
}
