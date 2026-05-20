#pragma once

/// \file
/// \brief StrategyHarness — single-threaded, deterministic backtest driver.
///
/// StrategyHarness — single-threaded, deterministic backtest driver.
/// Constructs the strategy + InProcess clients + matching engine in
/// one process, reads .wslog files directly, decodes via the existing
/// mdgw venue parsers, drives strategy callbacks synchronously, and
/// collects fills via the existing ResultsCollector.
///
/// Key property: there is no other thread. Every event flows
///
///   wslog record → JSON parse → MdBbo → HarnessMdPublisher
///     → InProcessMdClient::push_bbo → strategy.on_bbo
///       → strategy.send_new_order → InProcessOrderGatewayClient
///         → MatchingEngine.submit_order
///           → fill callback → on_exec_report → strategy.on_exec_report
///
/// down a single stack frame. By the time push_bbo returns, the
/// strategy has fully reacted to that BBO and any fills have been
/// recorded. Two runs against the same input produce bit-identical
/// summaries — that's the whole point of this thing.
///
/// Today: HL only (the venue we have multi-day captures for). Adding
/// OKX/Binance is a matter of branching on the wslog file's path
/// (it carries the venue tag) and constructing the appropriate
/// decoder. Skipped for now to keep the first cut tight.

#include "backtester/harness/harness_md_publisher.h"
#include "backtester/harness/inprocess_order_gateway_client.h"
#include "backtester/matching/matching_engine.h"
#include "backtester/results/results_collector.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_decoder.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "strategy/config/config.h"
#include "strategy/md/inprocess_md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/inprocess_refdata_client.h"
#include "strategy/strategy/i_strategy.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace bpt::backtester::harness {

class StrategyHarness {
public:
    struct Options {
        /// Path to the strategy config (the same one StrategyService would
        /// consume — the harness reads it via the standard config
        /// loader so params are byte-equivalent to a live or
        /// multi-process backtest run).
        std::string strategy_config_path;

        /// Path to the canonical instrument_mapping.<venue>.json the
        /// harness loads into the InProcessRefdataClient cache before
        /// starting the strategy.
        std::string instrument_mapping_path;

        /// One or more .wslog files to replay in timestamp order.
        /// Today the harness assumes all files belong to the same
        /// venue; HL only.
        ///
        /// Mutually exclusive with `canon_paths` — exactly one source
        /// must be populated. wslog is the raw venue path (JSON decode
        /// on the hot path); canon is the derived path (SBE blobs,
        /// no JSON parse).
        std::vector<std::string> wslog_paths;

        /// Alternative source: one or more `.canon` files (produced by
        /// `bpt-canon-replay` or by future archive ingesters). When
        /// non-empty, the harness skips the JSON-decoder pipeline and
        /// feeds the matching engine + strategy directly from SBE bytes.
        std::vector<std::string> canon_paths;

        /// Starting capital for ResultsCollector. Defaults to whatever
        /// the strategy's [results] section holds; CLI override
        /// surfaces here.
        double starting_capital{0.0};

        /// Output dir for summary.json / trades.csv / pnl_curve.csv.
        std::string output_dir;

        /// Run identity — propagated into summary.json + the on-disk
        /// run_id directory name. Same shape as bpt-backtester multi-
        /// process flow.
        std::string strategy_name;
        std::string params_hash;
        std::string git_sha;
    };

    explicit StrategyHarness(Options opts);
    ~StrategyHarness();

    /// \brief Run the replay to completion and write results.
    void run();

private:
    /// \brief Construct InProcess clients, load refdata snapshot, instantiate
    ///        the strategy, and wire every callback.
    ///
    /// After this returns, strategy.start() has been called and it's ready
    /// to receive MD.
    void initialize();

    /// \brief Drive every input file. Picks the wslog or canon variant
    ///        based on which Options field is populated.
    /// \return The last simulated timestamp seen.
    uint64_t replay();

    /// \brief WS_FRAME-driven replay: each frame is JSON-decoded through
    ///        the live HyperliquidMdDecoder and fanned out via HarnessMdPublisher.
    uint64_t replay_wslog();

    /// \brief Canon-driven replay: each canon record's SBE blob is
    ///        decoded back to MdBbo/MdTrade/MdOrderBook and fanned out
    ///        via the same HarnessMdPublisher. Same downstream code path
    ///        as wslog — only the source differs.
    uint64_t replay_canon();

    /// \brief Final flatten + write summary.json, trades.csv, pnl_curve.csv.
    void finalize(uint64_t end_ts_ns);

    Options opts_;

    // Harness-owned components — order matters: strategy is constructed
    // last (depends on clients + order manager), destroyed first.
    matching::MatchingEngine matching_;
    std::unique_ptr<results::ResultsCollector> results_;
    HarnessHandler md_handler_;
    std::unique_ptr<bpt::strategy::md::InProcessMdClient<HarnessHandler>> md_client_;
    std::unique_ptr<bpt::strategy::refdata::InProcessRefdataClient> refdata_client_;
    std::unique_ptr<InProcessOrderGatewayClient> order_gw_;
    std::unique_ptr<bpt::strategy::order::OrderManager> order_mgr_;
    std::unique_ptr<bpt::strategy::strategy::IStrategy> strategy_;

    // HL decoder + the publisher that carries decoded events into the
    // strategy. Constructed alongside the strategy in initialize().
    bpt::md_gateway::adapter::SubscriptionMap hl_subs_;
    std::unique_ptr<HarnessMdPublisher> hl_publisher_;
    std::unique_ptr<bpt::md_gateway::adapter::HyperliquidMdDecoder<HarnessMdPublisher>> hl_decoder_;

    // Mirror of the strategy's subscribed instruments — used by the
    // canon replay path to filter events the wslog HL decoder would
    // have dropped via SubscriptionMap.find_id() == 0. Without this
    // filter, canon (which carries the full venue universe) would feed
    // BBOs for non-subscribed instruments into the strategy and shift
    // its time-based state, breaking wslog/canon parity.
    std::unordered_set<uint64_t> strategy_instrument_ids_;

    // Funding-rate + instrument-stats callbacks the decoder requires;
    // AS strategy doesn't need either (HL funding is on stream 1005 in
    // production but the strategy reads from refdata's FundingRateCache,
    // not directly; stats aren't consumed by AS at all).
    bpt::md_gateway::messaging::FundingRateCallback noop_funding_cb_{
        [](const bpt::md_gateway::messaging::FundingRateUpdate&) {}};
    bpt::md_gateway::messaging::InstrumentStatsCallback noop_stats_cb_{
        [](const bpt::md_gateway::messaging::InstrumentStatsUpdate&) {}};

    // Parsed strategy config — owns the TOML tables the strategy
    // params reference.
    bpt::strategy::config::AppConfig strategy_cfg_;
};

}  // namespace bpt::backtester::harness
