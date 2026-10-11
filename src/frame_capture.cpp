#include "frame_capture.hpp"

#include <stdexcept>

namespace ticker {

FrameCaptureWriter::FrameCaptureWriter(const std::string& path)
    : out_(path, std::ios::binary | std::ios::app) {
    if (!out_) throw std::runtime_error("FrameCaptureWriter: failed to open " + path);
}

FrameCaptureWriter::~FrameCaptureWriter() { flush(); }

void FrameCaptureWriter::write_frame(std::string_view payload, int64_t recv_ts_ns) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    out_.write(reinterpret_cast<const char*>(&recv_ts_ns), sizeof(recv_ts_ns));
    out_.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ++frames_written_;
}

void FrameCaptureWriter::flush() { out_.flush(); }

FrameCaptureReader::FrameCaptureReader(const std::string& path) : in_(path, std::ios::binary) {
    if (!in_) throw std::runtime_error("FrameCaptureReader: failed to open " + path);
}

bool FrameCaptureReader::next_frame(std::string& out, int64_t& recv_ts_ns) {
    int64_t ts;
    uint32_t len;
    if (!in_.read(reinterpret_cast<char*>(&ts), sizeof(ts))) return false;
    if (!in_.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
    out.resize(len);
    if (len > 0 && !in_.read(out.data(), len)) return false;
    recv_ts_ns = ts;
    ++frames_read_;
    return true;
}

}  // namespace ticker
