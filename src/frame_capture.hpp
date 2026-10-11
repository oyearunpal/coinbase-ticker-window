#pragma once
// Records raw WebSocket frames with their receive timestamps, and replays
// them through the same parse/window/CSV pipeline (skipping only the socket).
// Replaying a capture gives byte-identical CSV, so configs can be A/B'd on
// the same input.
//
// File format, repeated to EOF, no header:
//   [int64_t recv_ts_ns][uint32_t len][len bytes of frame]
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace ticker {

class FrameCaptureWriter {
  public:
    explicit FrameCaptureWriter(const std::string& path);
    ~FrameCaptureWriter();

    void write_frame(std::string_view payload, int64_t recv_ts_ns);
    void flush();
    uint64_t frames_written() const noexcept { return frames_written_; }

  private:
    std::ofstream out_;
    uint64_t frames_written_ = 0;
};

class FrameCaptureReader {
  public:
    explicit FrameCaptureReader(const std::string& path);

    // Reads the next frame into `out`, setting `recv_ts_ns` to its
    // captured receive timestamp. Returns false at EOF.
    bool next_frame(std::string& out, int64_t& recv_ts_ns);

    uint64_t frames_read() const noexcept { return frames_read_; }

  private:
    std::ifstream in_;
    uint64_t frames_read_ = 0;
};

}  // namespace ticker
