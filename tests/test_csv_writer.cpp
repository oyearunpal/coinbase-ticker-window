#include "doctest.h"
#include "../src/csv_writer.hpp"
#include "../src/fixed_price.hpp"
#include "../src/iso8601.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ticker;
namespace fs = std::filesystem;

TEST_CASE("format_csv_row: matches the documented column order and formatting") {
    StatRecord r{};
    r.slot = 0;
    r.recv_ts_ns = parse_iso8601_ns("2026-09-21T09:50:22.531000000Z").value();
    r.event_ts_ns = parse_iso8601_ns("2026-09-21T09:50:22.500000000Z").value();
    r.trade_id = 1095963415;
    r.price = parse_price("84792.03").value();
    r.mean = parse_price("84790.11").value();
    r.median = parse_price("84791.50").value();
    r.high = parse_price("84805.22").value();
    r.low = parse_price("84770.00").value();
    r.count = 412;

    char buf[256];
    int n = format_csv_row(r, buf);
    std::string row(buf, n);

    CHECK(row ==
          "2026-09-21T09:50:22.531000000Z,2026-09-21T09:50:22.500000000Z,1095963415,"
          "84792.03,84790.11,84791.50,84805.22,84770.00,35.22,412\n");
}

TEST_CASE("format_csv_row: range is exactly high - low") {
    StatRecord r{};
    r.recv_ts_ns = r.event_ts_ns = 0;
    r.trade_id = 1;
    r.price = r.mean = r.median = 100;
    r.high = parse_price("10.5").value();
    r.low = parse_price("3.25").value();
    r.count = 1;
    char buf[256];
    int n = format_csv_row(r, buf);
    std::string row(buf, n);
    CHECK(row.find(",7.25,1\n") != std::string::npos);
}

TEST_CASE("format_csv_row: INT64_MIN trade_id does not trigger UB on negation") {
    StatRecord r{};
    r.trade_id = INT64_MIN;
    r.price = r.mean = r.median = r.high = r.low = 1;
    r.count = 1;
    char buf[256];
    int n = format_csv_row(r, buf);
    std::string row(buf, n);
    CHECK(row.find(",-") != std::string::npos);  // just needs to not crash and be negative
}

TEST_CASE("CsvWriter: writes a header once and appends rows to a per-symbol file") {
    fs::path dir = fs::temp_directory_path() / "ticker_csv_test";
    fs::remove_all(dir);

    {
        CsvWriter w(dir.string(), {"BTC-USD", "ETH-USD"});
        StatRecord r{};
        r.slot = 0;
        r.event_ts_ns = r.recv_ts_ns = 0;
        r.trade_id = 1;
        r.price = r.mean = r.median = r.high = r.low = 100;
        r.count = 1;
        w.write(r);
        r.slot = 1;
        r.trade_id = 2;
        w.write(r);
        CHECK(w.rows_written() == 2);
        w.flush_all();
    }  // destructor also flushes + closes

    std::ifstream btc(dir / "stats_BTC-USD.csv");
    REQUIRE(btc.is_open());
    std::string line1, line2;
    std::getline(btc, line1);
    std::getline(btc, line2);
    CHECK(line1 == "recv_ts,event_ts,trade_id,price,mean,median,high,low,range,count");
    CHECK(line2.find(",1,") != std::string::npos);  // trade_id=1

    std::ifstream eth(dir / "stats_ETH-USD.csv");
    REQUIRE(eth.is_open());
    std::getline(eth, line1);
    std::getline(eth, line2);
    CHECK(line2.find(",2,") != std::string::npos);  // trade_id=2

    fs::remove_all(dir);
}

TEST_CASE("CsvWriter: reopening an existing file appends rather than re-writing the header") {
    fs::path dir = fs::temp_directory_path() / "ticker_csv_test_reopen";
    fs::remove_all(dir);

    StatRecord r{};
    r.slot = 0;
    r.trade_id = 1;
    r.price = r.mean = r.median = r.high = r.low = 1;
    r.count = 1;

    {
        CsvWriter w(dir.string(), {"BTC-USD"});
        w.write(r);
        w.flush_all();
    }
    {
        r.trade_id = 2;
        CsvWriter w(dir.string(), {"BTC-USD"});  // reopen
        w.write(r);
        w.flush_all();
    }

    std::ifstream f(dir / "stats_BTC-USD.csv");
    REQUIRE(f.is_open());
    int header_count = 0, row_count = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("recv_ts", 0) == 0) ++header_count;
        else ++row_count;
    }
    CHECK(header_count == 1);  // header written only on the first open, not on reopen
    CHECK(row_count == 2);

    fs::remove_all(dir);
}
