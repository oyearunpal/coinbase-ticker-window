#include "doctest.h"
#include "../src/ticker_parser.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace ticker;

TEST_CASE("parse_ticker: real captured ETH-USD frame") {
    std::string_view msg =
        R"({"type":"ticker","sequence":103139911985,"product_id":"ETH-USD","price":"2716.18",)"
        R"("open_24h":"2570.14","volume_24h":"103474.66823662","low_24h":"2566.46",)"
        R"("high_24h":"2748.94","volume_30d":"3120410.23012775","best_bid":"2716.17",)"
        R"("best_bid_size":"0.57437756","best_ask":"2716.18","best_ask_size":"0.93639670",)"
        R"("side":"buy","time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,)"
        R"("last_size":"0.36180165"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->product_id == "ETH-USD");
    CHECK(t->price == parse_price("2716.18").value());
    CHECK(t->trade_id == 844419776);
    CHECK(t->event_ts_ns == parse_iso8601_ns("2026-09-21T10:58:36.485337Z").value());
    REQUIRE(t->low_24h().has_value());
    REQUIRE(t->high_24h().has_value());
    CHECK(*t->low_24h() == parse_price("2566.46").value());
    CHECK(*t->high_24h() == parse_price("2748.94").value());
}

TEST_CASE("parse_ticker: real captured BTC-USD frame with a whole-number field (high_24h:85257)") {
    std::string_view msg =
        R"({"type":"ticker","sequence":136469527716,"product_id":"BTC-USD","price":"84399.44",)"
        R"("open_24h":"80283.01","volume_24h":"7054.26893152","low_24h":"80241.84",)"
        R"("high_24h":"85257","volume_30d":"187642.65616403","best_bid":"84396.59",)"
        R"("best_bid_size":"0.00888603","best_ask":"84399.45","best_ask_size":"0.03653149",)"
        R"("side":"sell","time":"2026-09-21T10:58:36.773582Z","trade_id":1095993713,)"
        R"("last_size":"0.00710882"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->product_id == "BTC-USD");
    CHECK(t->trade_id == 1095993713);
    REQUIRE(t->high_24h().has_value());
    CHECK(*t->high_24h() == parse_price("85257").value());
}

TEST_CASE("parse_ticker: low_24h/high_24h missing entirely still accepts the tick (optional field)") {
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","price":"1.0","trade_id":1,"time":"2026-01-01T00:00:00Z"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK_FALSE(t->low_24h().has_value());
    CHECK_FALSE(t->high_24h().has_value());
}

TEST_CASE("parse_ticker: malformed low_24h/high_24h degrades to nullopt rather than rejecting the tick") {
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","price":"1.0","trade_id":1,)"
        R"("low_24h":"not-a-price","high_24h":"also-bad","time":"2026-01-01T00:00:00Z"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());  // the five required fields are all present and valid
    CHECK_FALSE(t->low_24h().has_value());
    CHECK_FALSE(t->high_24h().has_value());
}

TEST_CASE("parse_ticker: low_24h/high_24h order-independent, same as the other fields") {
    std::string_view msg =
        R"({"high_24h":"200.00","time":"2026-01-01T00:00:00.000000Z","trade_id":42,)"
        R"("low_24h":"50.00","price":"9.99","type":"ticker","product_id":"SOL-USD"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    REQUIRE(t->low_24h().has_value());
    REQUIRE(t->high_24h().has_value());
    CHECK(*t->low_24h() == parse_price("50.00").value());
    CHECK(*t->high_24h() == parse_price("200.00").value());
}

TEST_CASE("parse_ticker: rejects the subscriptions ack (not type=ticker)") {
    std::string_view msg =
        R"({"type":"subscriptions","channels":[{"name":"ticker","product_ids":["BTC-USD","ETH-USD"],"account_ids":null}]})";
    CHECK_FALSE(parse_ticker(msg).has_value());
}

TEST_CASE("parse_ticker: rejects heartbeat and error message types") {
    CHECK_FALSE(parse_ticker(R"({"type":"heartbeat","sequence":1,"last_trade_id":1,"product_id":"BTC-USD","time":"2026-01-01T00:00:00.000000Z"})").has_value());
    CHECK_FALSE(parse_ticker(R"({"type":"error","message":"bad request"})").has_value());
}

TEST_CASE("parse_ticker: rejects truncated JSON") {
    CHECK_FALSE(parse_ticker(R"({"type":"ticker","product_id":"BTC-USD","price":"1.0")").has_value());
    CHECK_FALSE(parse_ticker(R"({"type":"ticker")").has_value());
    CHECK_FALSE(parse_ticker("").has_value());
    CHECK_FALSE(parse_ticker("{").has_value());
}

TEST_CASE("parse_ticker: rejects a ticker message missing trade_id") {
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","price":"100.00","time":"2026-01-01T00:00:00.000000Z"})";
    CHECK_FALSE(parse_ticker(msg).has_value());
}

TEST_CASE("parse_ticker: rejects a ticker message missing price") {
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","trade_id":5,"time":"2026-01-01T00:00:00.000000Z"})";
    CHECK_FALSE(parse_ticker(msg).has_value());
}

TEST_CASE("parse_ticker: rejects malformed price or time even if all fields present") {
    CHECK_FALSE(parse_ticker(R"({"type":"ticker","product_id":"BTC-USD","price":"abc","trade_id":5,"time":"2026-01-01T00:00:00.000000Z"})").has_value());
    CHECK_FALSE(parse_ticker(R"({"type":"ticker","product_id":"BTC-USD","price":"1.0","trade_id":5,"time":"not-a-time"})").has_value());
}

TEST_CASE("parse_ticker: field order does not matter (single pass recognizes keys, not positions)") {
    std::string_view msg =
        R"({"time":"2026-01-01T00:00:00.000000Z","trade_id":42,"price":"9.99","type":"ticker","product_id":"SOL-USD","side":"buy"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->product_id == "SOL-USD");
    CHECK(t->trade_id == 42);
    CHECK(t->price == parse_price("9.99").value());
}

TEST_CASE("parse_ticker: nanosecond-resolution time field (9 fractional digits) still parses") {
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","price":"1.0","trade_id":1,"time":"2026-09-21T09:50:22.531463080Z"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->event_ts_ns == parse_iso8601_ns("2026-09-21T09:50:22.531463080Z").value());
}

TEST_CASE("parse_ticker: negative trade_id is accepted as a bare number (schema allows it in principle)") {
    // Coinbase's trade_id is always positive in practice, but the parser
    // treats it as a generic signed integer rather than special-casing sign.
    std::string_view msg =
        R"({"type":"ticker","product_id":"BTC-USD","price":"1.0","trade_id":-1,"time":"2026-01-01T00:00:00Z"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->trade_id == -1);
}

TEST_CASE("parse_ticker: an unrecognized nested object field is skipped without desyncing the parse") {
    std::string_view msg =
        R"({"type":"ticker","meta":{"nested":{"a":1,"b":[1,2,3]}},"product_id":"BTC-USD",)"
        R"("price":"1.0","trade_id":1,"time":"2026-01-01T00:00:00Z"})";
    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->product_id == "BTC-USD");
}

TEST_CASE("parse_ticker: real captured frames loaded from tests/fixtures also round-trip") {
    std::ifstream f("tests/fixtures/real_ticker_frames.txt");
    REQUIRE(f.is_open());
    std::string line;
    int ticker_count = 0, other_count = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto t = parse_ticker(line);
        if (t.has_value()) {
            ++ticker_count;
            CHECK((t->product_id == "BTC-USD" || t->product_id == "ETH-USD"));
        } else {
            ++other_count;  // the subscriptions ack line
        }
    }
    CHECK(ticker_count == 3);
    CHECK(other_count == 1);
}

// -- scan_fast() vs scan_generic(): must agree whenever scan_fast() accepts -----

TEST_CASE("scan_fast: the real captured frame's exact wire shape hits the fast path") {
    std::string_view msg =
        R"({"type":"ticker","sequence":103139911985,"product_id":"ETH-USD","price":"2716.18",)"
        R"("open_24h":"2570.14","volume_24h":"103474.66823662","low_24h":"2566.46",)"
        R"("high_24h":"2748.94","volume_30d":"3120410.23012775","best_bid":"2716.17",)"
        R"("best_bid_size":"0.57437756","best_ask":"2716.18","best_ask_size":"0.93639670",)"
        R"("side":"buy","time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,)"
        R"("last_size":"0.36180165"})";

    auto raw = detail::scan_fast(msg);
    REQUIRE(raw.has_value());
    CHECK(raw->product_id == "ETH-USD");
    CHECK(raw->low_24h_str == "2566.46");
    CHECK(raw->high_24h_str == "2748.94");

    auto t = parse_ticker(msg);
    REQUIRE(t.has_value());
    CHECK(t->product_id == "ETH-USD");
}

TEST_CASE("scan_fast: reordered keys, an added field, a removed field, and embedded whitespace "
          "all miss the fast path -- but parse_ticker still accepts them via scan_generic") {
    auto expect_generic_only = [](std::string_view msg) {
        CHECK_FALSE(detail::scan_fast(msg).has_value());
        auto t = parse_ticker(msg);
        REQUIRE(t.has_value());
        return t;
    };

    // Reordered: type/product_id swapped relative to the real wire order.
    auto t1 = expect_generic_only(
        R"({"product_id":"ETH-USD","type":"ticker","sequence":103139911985,"price":"2716.18",)"
        R"("open_24h":"2570.14","volume_24h":"103474.66823662","low_24h":"2566.46",)"
        R"("high_24h":"2748.94","volume_30d":"3120410.23012775","best_bid":"2716.17",)"
        R"("best_bid_size":"0.57437756","best_ask":"2716.18","best_ask_size":"0.93639670",)"
        R"("side":"buy","time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,)"
        R"("last_size":"0.36180165"})");
    CHECK(t1->product_id == "ETH-USD");

    // Added field: a brand-new key inserted mid-schema.
    expect_generic_only(
        R"({"type":"ticker","sequence":103139911985,"product_id":"ETH-USD","price":"2716.18",)"
        R"("new_field":"surprise","open_24h":"2570.14","volume_24h":"103474.66823662",)"
        R"("low_24h":"2566.46","high_24h":"2748.94","volume_30d":"3120410.23012775",)"
        R"("best_bid":"2716.17","best_bid_size":"0.57437756","best_ask":"2716.18",)"
        R"("best_ask_size":"0.93639670","side":"buy","time":"2026-09-21T10:58:36.485337Z",)"
        R"("trade_id":844419776,"last_size":"0.36180165"})");

    // Removed field: open_24h dropped entirely.
    expect_generic_only(
        R"({"type":"ticker","sequence":103139911985,"product_id":"ETH-USD","price":"2716.18",)"
        R"("volume_24h":"103474.66823662","low_24h":"2566.46","high_24h":"2748.94",)"
        R"("volume_30d":"3120410.23012775","best_bid":"2716.17","best_bid_size":"0.57437756",)"
        R"("best_ask":"2716.18","best_ask_size":"0.93639670","side":"buy",)"
        R"("time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,"last_size":"0.36180165"})");

    // Embedded whitespace: a space after the very first colon.
    expect_generic_only(
        R"({"type": "ticker","sequence":103139911985,"product_id":"ETH-USD","price":"2716.18",)"
        R"("open_24h":"2570.14","volume_24h":"103474.66823662","low_24h":"2566.46",)"
        R"("high_24h":"2748.94","volume_30d":"3120410.23012775","best_bid":"2716.17",)"
        R"("best_bid_size":"0.57437756","best_ask":"2716.18","best_ask_size":"0.93639670",)"
        R"("side":"buy","time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,)"
        R"("last_size":"0.36180165"})");
}

TEST_CASE("scan_fast: a price given as a bare JSON number (not a quoted string) is rejected by "
          "both the fast and the generic path") {
    std::string_view msg =
        R"({"type":"ticker","sequence":103139911985,"product_id":"ETH-USD","price":2716.18,)"
        R"("open_24h":"2570.14","volume_24h":"103474.66823662","low_24h":"2566.46",)"
        R"("high_24h":"2748.94","volume_30d":"3120410.23012775","best_bid":"2716.17",)"
        R"("best_bid_size":"0.57437756","best_ask":"2716.18","best_ask_size":"0.93639670",)"
        R"("side":"buy","time":"2026-09-21T10:58:36.485337Z","trade_id":844419776,)"
        R"("last_size":"0.36180165"})";
    CHECK_FALSE(detail::scan_fast(msg).has_value());
    CHECK_FALSE(parse_ticker(msg).has_value());
}

// -- Exhaustive differential: scan_fast can only be slower than scan_generic, never wrong -------
//
// Drives every real fixture frame (including the non-ticker subscriptions
// ack), every truncation prefix of it, and every single-byte mutation of
// it (at every position, to each of a handful of structurally significant
// replacement characters) through both scan paths, and checks:
//   1. parse_ticker()'s result equals what scan_generic() + build_ticker()
//      alone would produce -- i.e. the fast path never changes the answer.
//   2. whenever scan_fast() itself accepts, scan_generic() also accepts,
//      with byte-for-byte identical RawFields -- the fast path can accept
//      only what the generic path already agrees is exactly this shape.
TEST_CASE("differential: scan_fast agrees with scan_generic on every mutation/truncation of every "
          "real fixture frame") {
    std::ifstream f("tests/fixtures/real_ticker_frames.txt");
    REQUIRE(f.is_open());
    std::vector<std::string> frames;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) frames.push_back(line);
    }
    REQUIRE(frames.size() == 4);

    NullTracer tracer;
    uint64_t compared = 0;

    auto check_one = [&](const std::string& m) {
        std::string_view mv(m);
        auto generic_raw = detail::scan_generic(mv, tracer);
        std::optional<Ticker> expected = generic_raw ? detail::build_ticker(*generic_raw, tracer) : std::nullopt;
        auto actual = parse_ticker(mv);
        REQUIRE(expected == actual);

        auto fast_raw = detail::scan_fast(mv);
        if (fast_raw) {
            REQUIRE(generic_raw.has_value());
            CHECK(*fast_raw == *generic_raw);
        }
        ++compared;
    };

    static constexpr char kReplacements[] = {'"', ',', '}', ':', 'x', '1', ' ', '-'};

    for (const auto& base : frames) {
        check_one(base);  // unmutated

        for (size_t len = 0; len < base.size(); ++len) check_one(base.substr(0, len));

        for (size_t pos = 0; pos < base.size(); ++pos) {
            for (char rep : kReplacements) {
                if (base[pos] == rep) continue;  // not actually a mutation
                std::string m = base;
                m[pos] = rep;
                check_one(m);
            }
        }
    }
    CHECK(compared > 5000);  // sanity: the loops above actually ran a real number of cases
}

TEST_CASE("frame_type: reads the top-level type so control frames are told apart from bad tickers") {
    CHECK(frame_type(R"({"type":"subscriptions","channels":[{"name":"ticker","product_ids":["ETH-USD"]}]})") ==
          "subscriptions");
    CHECK(frame_type(R"({"type":"error","message":"Failed to subscribe","reason":"BAD-USD is not a valid product"})") ==
          "error");
    CHECK(frame_type(R"( { "sequence" : 1, "nested":{"type":"x"}, "type" : "heartbeat" })") == "heartbeat");
    CHECK(frame_type(R"({"type":"ticker","price":)") == "ticker");  // truncated ticker still reports "ticker"
    CHECK_FALSE(frame_type(R"({"sequence":1})").has_value());
    CHECK_FALSE(frame_type("not json").has_value());
    CHECK_FALSE(frame_type("").has_value());
    CHECK_FALSE(frame_type(R"({"type":42})").has_value());
}
