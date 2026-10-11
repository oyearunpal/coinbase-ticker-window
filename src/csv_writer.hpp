#pragma once
// Writer thread side: pops StatRecords from the SPSC ring, formats them to
// CSV by hand (no iostream/printf), and writes one file per symbol
// (logs/stats_<SYMBOL>.csv). Prices have 2 decimals; mean and median are
// rounded half-up. Each row's stats already include that row's price.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "fixed_price.hpp"
#include "iso8601.hpp"
#include "probe.hpp"
#include "spsc_ring.hpp"

namespace ticker {

// One accepted tick plus its stats. Trivially copyable, so it goes through
// SpscRing by value.
struct StatRecord {
    int32_t slot;            // SymbolRegistry slot -- the writer maps this to a filename
    int64_t recv_ts_ns;       // when the feed thread received the raw frame (wall clock)
    int64_t event_ts_ns;      // the exchange's own timestamp (from the ticker payload)
    int64_t trade_id;
    Price price;
    Price mean;
    Price median;
    Price low;
    Price high;
    uint32_t count;
};

static_assert(std::is_trivially_copyable_v<StatRecord>, "StatRecord must travel through SpscRing as-is");

// Formats one row including the trailing newline into `out` (>= 256 bytes)
// and returns its length. Uses fresh formatters, so no date caching; meant
// for tests and one-off calls.
int format_csv_row(const StatRecord& r, char* out) noexcept;

// Same, with caller-owned formatters so the date cache persists across rows.
// One formatter per timestamp column; a shared one would thrash.
int format_csv_row(const StatRecord& r, char* out, Iso8601Formatter& recv_fmt, Iso8601Formatter& event_fmt) noexcept;

// One append-mode fd and buffer per symbol. Writer thread only.
class CsvWriter {
  public:
    CsvWriter(const std::string& dir, const std::vector<std::string>& symbol_names);
    ~CsvWriter();

    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    // Buffers one row; flushes that symbol if its buffer passes the
    // threshold. Call flush_all() periodically to bound latency.
    void write(const StatRecord& r);

    void flush_all();

    uint64_t rows_written() const noexcept { return rows_written_; }

  private:
    struct FileBuf {
        int fd = -1;
        std::string buf;
    };

    void flush_one(FileBuf& fb);

    std::vector<FileBuf> files_;  // indexed by slot, one file per symbol
    uint64_t rows_written_ = 0;
    // Kept for the writer's lifetime so the date cache hits.
    Iso8601Formatter recv_fmt_;
    Iso8601Formatter event_fmt_;
};

}  // namespace ticker
