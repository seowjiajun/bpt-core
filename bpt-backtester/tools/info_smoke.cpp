// info_smoke — validates HyperliquidInfoServer survives multiple
// consecutive POST /info requests. Was the open blocker for the
// HL backtest pipeline (UAF segfault on second connection).
//
// Usage:
//   info_smoke --snapshot /opt/bpt/data/raw/hyperliquid/2026-04-25/meta.json [--port 18113]

#include "backtester/exchange/hyperliquid_info_server.h"

#include <CLI/CLI.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

bool one_post(uint16_t port, const std::string& body) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
        stream.connect(endpoints);

        http::request<http::string_body> req(http::verb::post, "/info", 11);
        req.set(http::field::host, "127.0.0.1");
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
        http::write(stream, req);

        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);

        std::printf("    status=%u, body_size=%zu, body_head=%.80s\n",
                    static_cast<unsigned>(res.result_int()),
                    res.body().size(),
                    res.body().c_str());

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        return res.result() == http::status::ok;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "    request failed: %s\n", e.what());
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App cli{"HyperliquidInfoServer smoke test"};
    std::string snapshot;
    uint16_t port = 18199;
    double capital = 100000.0;
    cli.add_option("--snapshot", snapshot, "path to meta.json")->required();
    cli.add_option("--port", port, "listen port (default 18199)");
    cli.add_option("--capital", capital, "starting capital");
    CLI11_PARSE(cli, argc, argv);

    bpt::backtester::exchange::HyperliquidInfoServer server(port, snapshot, capital);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int passed = 0, total = 0;

    auto run = [&](const char* label, const std::string& body) {
        ++total;
        std::printf("[%d] %s\n", total, label);
        if (one_post(port, body))
            ++passed;
    };

    run("1st: meta", R"({"type":"meta"})");
    run("2nd: meta (was crash trigger)", R"({"type":"meta"})");
    run("3rd: clearinghouseState", R"({"type":"clearinghouseState","user":"0x0"})");
    run("4th: userFees", R"({"type":"userFees","user":"0x0"})");
    run("5th: metaAndAssetCtxs", R"({"type":"metaAndAssetCtxs"})");
    run("6th: meta again", R"({"type":"meta"})");

    server.stop();
    std::printf("\n%d/%d requests OK\n", passed, total);
    return passed == total ? 0 : 1;
}
