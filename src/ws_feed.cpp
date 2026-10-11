#include "ws_feed.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <stdexcept>
#include <string_view>

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace ticker {

WsFeed::WsFeed(std::string host, std::string port)
    : host_(std::move(host)), port_(std::move(port)), ssl_ctx_(ssl::context::tlsv12_client) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

WsFeed::~WsFeed() { close(); }

void WsFeed::connect_and_subscribe(const std::vector<std::string>& products, const std::string& channel) {
    tcp::resolver resolver(ioc_);
    ws_.emplace(ioc_, ssl_ctx_);

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        throw std::runtime_error("WsFeed: SSL SNI setup failed");
    }

    auto const results = resolver.resolve(host_, port_);
    beast::get_lowest_layer(*ws_).connect(results);
    beast::get_lowest_layer(*ws_).expires_never();
    // TCP_NODELAY so Beast's automatic pongs aren't delayed by Nagle/delayed ACK. Set
    // before the TLS handshake so it applies to every write.
    beast::get_lowest_layer(*ws_).socket().set_option(tcp::no_delay(true));

    ws_->next_layer().handshake(ssl::stream_base::client);

    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "coinbase-ticker-window/1.0");
    }));

    ws_->handshake(host_, "/");

    std::string sub;
    constexpr std::string_view kHead = R"({"type":"subscribe","channels":[{"name":")";
    constexpr std::string_view kMid = R"(","product_ids":[)";
    constexpr std::string_view kTail = "]}]}";
    size_t reserve_n = kHead.size() + channel.size() + kMid.size() + kTail.size();
    for (const auto& p : products) reserve_n += p.size() + 3;  // quotes + comma
    sub.reserve(reserve_n);
    sub.append(kHead).append(channel).append(kMid);
    for (size_t i = 0; i < products.size(); ++i) {
        if (i) sub += ',';
        sub += '"';
        sub += products[i];
        sub += '"';
    }
    sub.append(kTail);

    ws_->text(true);
    ws_->write(net::buffer(sub));
}

bool WsFeed::read_frame(std::string& out) {
    if (!ws_) return false;
    boost::system::error_code ec;
    // clear() keeps out's capacity, so once it has grown to the largest
    // frame seen, reads land directly in it with no allocation.
    out.clear();
#ifdef __linux__
    // Re-arm TCP_QUICKACK (the kernel clears it after each ACK) so incoming
    // segments are ACKed immediately. Done before the blocking read so it
    // never delays a received frame. Best effort; result ignored.
    {
        const int one = 1;
        (void)::setsockopt(beast::get_lowest_layer(*ws_).socket().native_handle(), IPPROTO_TCP,
                           TCP_QUICKACK, &one, sizeof one);
    }
#endif
    auto buffer = net::dynamic_buffer(out);
    ws_->read(buffer, ec);
    if (ec == websocket::error::closed) return false;
    if (ec) throw boost::system::system_error(ec);
    return true;
}

void WsFeed::close() noexcept {
    if (!ws_) return;
    boost::system::error_code ec;
    ws_->close(websocket::close_code::normal, ec);
    ws_.reset();
}

}  // namespace ticker
