#pragma once
// Blocking TLS WebSocket client for Coinbase's public `ticker` channel (no
// API key needed).
//
// Uses Beast's synchronous API: there is one connection, read by the pinned
// feed thread in a block-process-repeat loop, so an async event loop would
// only add complexity.
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace ticker {

class WsFeed {
  public:
    explicit WsFeed(std::string host, std::string port = "443");
    ~WsFeed();

    WsFeed(const WsFeed&) = delete;
    WsFeed& operator=(const WsFeed&) = delete;

    // TLS + WebSocket handshake (with SNI), then subscribe. Throws on failure.
    void connect_and_subscribe(const std::vector<std::string>& products, const std::string& channel = "ticker");

    // Blocking read of one text frame into `out`. Returns false on a clean
    // close; throws on a transport error.
    bool read_frame(std::string& out);

    void close() noexcept;

  private:
    std::string host_;
    std::string port_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;
    std::optional<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::beast::tcp_stream>>> ws_;
};

}  // namespace ticker
