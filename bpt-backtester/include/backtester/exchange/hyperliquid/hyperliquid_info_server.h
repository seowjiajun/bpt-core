#pragma once

/// \file
/// \brief Mock Hyperliquid REST /info server for backtesting.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bpt::backtester::exchange {

class HyperliquidInfoSession;

/// \brief Mock Hyperliquid REST /info server for backtesting.
///
/// Mirrors the read-only subset of HL's REST surface that bpt-refdata and
/// bpt-order-gateway hit at startup:
///
///   POST /info {"type":"meta"}              → universe metadata snapshot
///   POST /info {"type":"metaAndAssetCtxs"}  → universe + per-asset contexts
///   POST /info {"type":"userFees", ...}     → fee schedule (synthesised default)
///
/// Snapshot contents come from /opt/bpt/data/raw/{venue}/{date}/refdata-snapshot.json,
/// captured once at recording time by bpt-refdata's recording mode. Backtester
/// loads the file at startup and serves it back; refdata in backtest mode
/// points its REST client at this server instead of the live venue.
///
/// Plain HTTP (no TLS) — backtest configs set use_tls=false.
class HyperliquidInfoServer {
public:
    /// \brief Construct an info server backed by an on-disk snapshot.
    /// \param snapshot_path     Path to refdata-snapshot.json (the raw HL /info response).
    ///                          If empty or missing on disk, server returns 503 for every
    ///                          request — caller can detect this and fall back / fail loudly.
    /// \param starting_capital  Equity to report from clearinghouseState queries. Static
    ///                          for now (matches BacktesterService's settings.results.starting_capital).
    ///                          When MatchingEngine integration lands, this will be replaced
    ///                          by a live read from ResultsCollector.
    /// \param port              TCP port to listen on.
    HyperliquidInfoServer(uint16_t port, std::string snapshot_path, double starting_capital);
    ~HyperliquidInfoServer();

    void start();
    void stop();

    /// \brief Returns the parsed universe (asset_idx → coin name) from the snapshot.
    ///
    /// Empty if snapshot wasn't loaded successfully. Used by BacktesterService to
    /// populate HyperliquidOrderServer's asset_universe.
    const std::vector<std::string>& asset_universe() const { return asset_universe_; }

private:
    void load_snapshot();
    void do_accept();

    uint16_t port_;
    std::string snapshot_path_;
    double starting_capital_;
    std::string meta_response_body_;           ///< verbatim {"type":"meta"} response.
    std::string meta_ctxs_response_body_;      ///< {"type":"metaAndAssetCtxs"} response (same as meta + ctxs).
    std::string clearinghouse_response_body_;  ///< synthesized clearinghouseState JSON.
    std::vector<std::string> asset_universe_;

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<HyperliquidInfoSession>> sessions_;
    std::thread thread_;
};

}  // namespace bpt::backtester::exchange
