#include "csv_writer.hpp"

#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "iso8601.hpp"

namespace ticker {

int format_csv_row(const StatRecord& r, char* out) noexcept {
    Iso8601Formatter recv_fmt, event_fmt;
    return format_csv_row(r, out, recv_fmt, event_fmt);
}

int format_csv_row(const StatRecord& r, char* out, Iso8601Formatter& recv_fmt, Iso8601Formatter& event_fmt) noexcept {
    char* p = out;
    p += recv_fmt.format(r.recv_ts_ns, p);
    *p++ = ',';
    p += event_fmt.format(r.event_ts_ns, p);
    *p++ = ',';

    // to_chars handles INT64_MIN safely (trade_id comes from the network).
    // 24 >= the 20 chars of "-9223372036854775808".
    auto put_i64 = [&](int64_t v) { p = std::to_chars(p, p + 24, v).ptr; };

    put_i64(r.trade_id);
    *p++ = ',';
    p += format_price(r.price, p);
    *p++ = ',';
    p += format_price(r.mean, p);
    *p++ = ',';
    p += format_price(r.median, p);
    *p++ = ',';
    p += format_price(r.high, p);
    *p++ = ',';
    p += format_price(r.low, p);
    *p++ = ',';
    p += format_price(r.high - r.low, p);
    *p++ = ',';
    put_i64(static_cast<int64_t>(r.count));
    *p++ = '\n';
    return static_cast<int>(p - out);
}

namespace {
constexpr size_t kFlushThreshold = 32 * 1024;
}  // namespace

CsvWriter::CsvWriter(const std::string& dir, const std::vector<std::string>& symbol_names) {
    ::mkdir(dir.c_str(), 0755);  // ignore EEXIST; a real error surfaces on open() below

    auto open_for = [&](const std::string& path) -> int {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) throw std::runtime_error("CsvWriter: failed to open " + path);
        return fd;
    };

    files_.resize(symbol_names.size());
    for (size_t i = 0; i < symbol_names.size(); ++i) {
        std::string path = dir + "/stats_" + symbol_names[i] + ".csv";
        bool is_new = (::access(path.c_str(), F_OK) != 0);
        files_[i].fd = open_for(path);
        if (is_new) {
            files_[i].buf += "recv_ts,event_ts,trade_id,price,mean,median,high,low,range,count\n";
        }
    }
}

CsvWriter::~CsvWriter() {
    flush_all();
    for (auto& f : files_) if (f.fd >= 0) ::close(f.fd);
}

void CsvWriter::write(const StatRecord& r) {
    char row[256];
    int n = format_csv_row(r, row, recv_fmt_, event_fmt_);
    FileBuf& fb = files_[r.slot];
    fb.buf.append(row, n);
    ++rows_written_;
    if (fb.buf.size() >= kFlushThreshold) flush_one(fb);
}

void CsvWriter::flush_one(FileBuf& fb) {
    if (fb.buf.empty() || fb.fd < 0) return;
    size_t off = 0;
    while (off < fb.buf.size()) {
        ssize_t n = ::write(fb.fd, fb.buf.data() + off, fb.buf.size() - off);
        if (n <= 0) break;  // best-effort; a real disk error will surface on the next open/fsync in practice
        off += static_cast<size_t>(n);
    }
    fb.buf.clear();
}

void CsvWriter::flush_all() {
    for (auto& f : files_) flush_one(f);
}

}  // namespace ticker
