#include "backtester/results/results_collector.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <boost/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <bpt_common/logging.h>

namespace fs = std::filesystem;

namespace bpt::backtester::results {

using matching::OrderSide;

// ── Construction ──────────────────────────────────────────────────────────────

std::string ResultsCollector::compose_run_id(const RunMetadata& m,
                                             const std::string& start_tag,
                                             const std::string& end_tag) {
    // Layout: {strategy}_{git7}_{params8}_{start}_{end}
    // Any missing component is dropped so a run identified only by its
    // window still yields a clean "{start}_{end}" path. Truncations
    // (7/8 chars) match git's short-SHA convention and balance
    // readability vs collision risk.
    std::string out;
    auto append = [&](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += '_';
        out += s;
    };
    append(m.strategy_name);
    append(m.git_sha.size() > 7 ? m.git_sha.substr(0, 7) : m.git_sha);
    append(m.params_hash.size() > 8 ? m.params_hash.substr(0, 8) : m.params_hash);
    append(start_tag);
    append(end_tag);
    return out.empty() ? std::string{"run"} : out;
}

ResultsCollector::ResultsCollector(double starting_capital, std::string output_dir,
                                   RunMetadata metadata)
    : starting_capital_(starting_capital),
      output_dir_(std::move(output_dir)),
      metadata_(std::move(metadata)),
      wallclock_start_ns_(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count())) {
    // Seed the equity curve with the starting point (ts=0 means pre-simulation).
    equity_curve_.push_back({0, starting_capital_});
}

// ── Position accounting ───────────────────────────────────────────────────────

// Returns realized PnL from any closed portion of the fill.
double ResultsCollector::apply_fill(Position& pos, double fill_qty, double fill_price, OrderSide side) {
    double realized = 0.0;

    if (side == OrderSide::BUY) {
        if (pos.net_qty >= 0.0) {
            // Adding to a long (or opening from flat).
            double new_qty = pos.net_qty + fill_qty;
            pos.avg_cost = (pos.net_qty * pos.avg_cost + fill_qty * fill_price) / new_qty;
            pos.net_qty = new_qty;
        } else {
            // Closing a short.
            double close_qty = std::min(fill_qty, -pos.net_qty);
            realized = close_qty * (pos.avg_cost - fill_price);
            pos.net_qty += close_qty;
            double remaining = fill_qty - close_qty;
            if (remaining > 1e-12) {
                // Flipped from short to long.
                pos.avg_cost = fill_price;
                pos.net_qty = remaining;
            }
        }
    } else {  // SELL
        if (pos.net_qty <= 0.0) {
            // Adding to a short (or opening from flat).
            double new_short = -pos.net_qty + fill_qty;
            pos.avg_cost = (-pos.net_qty * pos.avg_cost + fill_qty * fill_price) / new_short;
            pos.net_qty = -new_short;
        } else {
            // Closing a long.
            double close_qty = std::min(fill_qty, pos.net_qty);
            realized = close_qty * (fill_price - pos.avg_cost);
            pos.net_qty -= close_qty;
            double remaining = fill_qty - close_qty;
            if (remaining > 1e-12) {
                // Flipped from long to short.
                pos.avg_cost = fill_price;
                pos.net_qty = -remaining;
            }
        }
    }

    pos.realized_pnl += realized;
    return realized;
}

double ResultsCollector::total_realized_pnl() const {
    double sum = 0.0;
    for (const auto& [k, p] : positions_)
        sum += p.realized_pnl;
    return sum;
}

double ResultsCollector::total_unrealized_pnl() const {
    double sum = 0.0;
    for (const auto& [k, p] : positions_) {
        if (std::abs(p.net_qty) < 1e-12)
            continue;
        auto mid_it = mid_prices_.find(k);
        if (mid_it == mid_prices_.end())
            continue;
        sum += p.net_qty * (mid_it->second - p.avg_cost);
    }
    return sum;
}

double ResultsCollector::current_equity() const {
    return starting_capital_ + total_realized_pnl() + total_unrealized_pnl();
}

// ── Event handlers ────────────────────────────────────────────────────────────

void ResultsCollector::on_fill(const matching::FillReport& fill) {
    std::string key = fill.exchange + ':' + fill.symbol;
    double realized = apply_fill(positions_[key], fill.last_fill_qty, fill.last_fill_price, fill.side);

    double eq = current_equity();
    equity_curve_.push_back({fill.simulation_ts, eq});

    TradeRow row;
    row.simulation_ts = fill.simulation_ts;
    row.exchange = fill.exchange;
    row.symbol = fill.symbol;
    row.order_id = fill.order_id;
    row.client_order_id = fill.client_order_id;
    row.side = (fill.side == OrderSide::BUY) ? "BUY" : "SELL";
    row.order_type = (fill.order_type == matching::OrderType::MARKET) ? "MARKET" : "LIMIT";
    row.qty = fill.last_fill_qty;
    row.price = fill.last_fill_price;
    row.realized_pnl = realized;
    row.equity = eq;
    const std::size_t trade_idx = trades_.size();
    trades_.push_back(std::move(row));

    // Schedule one markout entry per horizon. Resolved later in
    // on_market_event when an event's ts crosses the target.
    for (std::size_t h = 0; h < kMarkoutHorizonsNs.size(); ++h) {
        pending_markouts_.push_back({
            trade_idx,
            h,
            fill.simulation_ts + static_cast<uint64_t>(kMarkoutHorizonsNs[h]),
        });
    }
}

void ResultsCollector::on_market_event(const data::MarketEvent& event) {
    if (event.type != data::MarketEvent::Type::ORDER_BOOK)
        return;
    const auto& ob = std::get<data::OrderBookRecord>(event.payload);
    std::string key = ob.exchange + ':' + ob.symbol;
    double bid = ob.bid_px[0];
    double ask = ob.ask_px[0];
    if (bid > 0.0 && ask > 0.0)
        mid_prices_[key] = (bid + ask) * 0.5;

    // Resolve any pending markouts whose target_ts has been crossed.
    // Single linear pass; entries that resolve are erased, the rest stay
    // for a later event. Using an index into the deque rather than an
    // iterator-based erase to keep the code simple — trade volumes
    // typical for this backtester make this cheap regardless.
    if (pending_markouts_.empty()) return;
    std::size_t write = 0;
    for (std::size_t i = 0; i < pending_markouts_.size(); ++i) {
        const auto& p = pending_markouts_[i];
        if (event.timestamp_ns < p.target_ts_ns) {
            if (write != i) pending_markouts_[write] = p;
            ++write;
            continue;
        }
        TradeRow& trade = trades_[p.trade_idx];
        std::string trade_key = trade.exchange + ':' + trade.symbol;
        auto mid_it = mid_prices_.find(trade_key);
        if (mid_it == mid_prices_.end() || trade.price <= 0.0) {
            // No mid available yet for this symbol — drop the entry; we
            // can't do anything useful with it later (target already passed).
            continue;
        }
        const double mid = mid_it->second;
        double bps = (mid - trade.price) / trade.price * 10000.0;
        if (trade.side == "SELL") bps = -bps;
        trade.markouts_bps[p.horizon_idx] = bps;
        // entry resolved → not copied forward → effectively erased
    }
    pending_markouts_.resize(write);
}

// ── Output metrics ────────────────────────────────────────────────────────────

double ResultsCollector::compute_max_drawdown() const {
    if (trades_.empty())
        return 0.0;
    // Compute drawdown on cumulative realised PnL relative to starting
    // capital — same convention RiskPanel uses live. Earlier impl drew
    // from equity_curve_ which includes unrealized PnL at each fill ts;
    // a heavily-short position with a price spike can push the running
    // equity peak well above what the position can ever realise, making
    // the resulting "drawdown" exceed 100%. Realised-only is bounded
    // and matches what a research narrative actually means by DD.
    double cum_realised = 0.0;
    double peak = 0.0;
    double max_dd = 0.0;
    for (const auto& t : trades_) {
        cum_realised += t.realized_pnl;
        peak = std::max(peak, cum_realised);
        const double dd = (peak - cum_realised) / starting_capital_;
        max_dd = std::max(max_dd, dd);
    }
    return max_dd;
}

double ResultsCollector::compute_sharpe() const {
    // Use fill-to-fill equity changes as the return series.
    if (equity_curve_.size() < 3)
        return 0.0;

    std::vector<double> returns;
    returns.reserve(equity_curve_.size() - 1);
    for (std::size_t i = 1; i < equity_curve_.size(); ++i) {
        double prev = equity_curve_[i - 1].equity;
        if (prev > 1e-12)
            returns.push_back((equity_curve_[i].equity - prev) / prev);
    }

    if (returns.empty())
        return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / static_cast<double>(returns.size());
    double variance = 0.0;
    for (double r : returns)
        variance += (r - mean) * (r - mean);
    variance /= static_cast<double>(returns.size());
    double stddev = std::sqrt(variance);

    if (stddev < 1e-12)
        return 0.0;
    // Scale to annualised using 252 trading days * ~390 fills/day as a rough proxy.
    // The raw Sharpe is left unscaled — caller can interpret as per-fill.
    return mean / stddev;
}

// ── File writing ──────────────────────────────────────────────────────────────

void ResultsCollector::write() const {
    fs::create_directories(output_dir_);

    // ── trades.csv ────────────────────────────────────────────────────────
    {
        std::ofstream f(output_dir_ + "/trades.csv");
        if (!f)
            throw std::runtime_error("Cannot open " + output_dir_ + "/trades.csv");
        f << "simulation_ts,exchange,symbol,order_id,client_order_id,"
             "side,type,qty,price,realized_pnl,equity,"
             "markout_50ms_bps,markout_1s_bps,markout_5s_bps,markout_30s_bps\n";
        for (const auto& r : trades_) {
            f << std::format("{},{},{},{},{},{},{},{:.10g},{:.10g},{:.10g},{:.10g}",
                             r.simulation_ts,
                             r.exchange,
                             r.symbol,
                             r.order_id,
                             r.client_order_id,
                             r.side,
                             r.order_type,
                             r.qty,
                             r.price,
                             r.realized_pnl,
                             r.equity);
            // Empty cell instead of NaN for unresolved horizons — pandas
            // and dashboard parsers both treat it as missing.
            for (double mk : r.markouts_bps) {
                if (mk == kUnresolved)
                    f << ",";
                else
                    f << std::format(",{:.4f}", mk);
            }
            f << "\n";
        }
        bpt::common::log::info("[ResultsCollector] Wrote {}/trades.csv ({} rows)", output_dir_, trades_.size());
    }

    // ── pnl_curve.csv ─────────────────────────────────────────────────────
    {
        std::ofstream f(output_dir_ + "/pnl_curve.csv");
        if (!f)
            throw std::runtime_error("Cannot open " + output_dir_ + "/pnl_curve.csv");
        f << "simulation_ts,equity\n";
        for (const auto& pt : equity_curve_)
            f << std::format("{},{:.10g}\n", pt.simulation_ts, pt.equity);
        bpt::common::log::info("[ResultsCollector] Wrote {}/pnl_curve.csv ({} points)", output_dir_, equity_curve_.size());
    }

    // ── summary.json ──────────────────────────────────────────────────────
    {
        double final_equity = equity_curve_.empty() ? starting_capital_ : equity_curve_.back().equity;
        double total_pnl = final_equity - starting_capital_;
        double return_pct = (starting_capital_ > 1e-12) ? (total_pnl / starting_capital_) * 100.0 : 0.0;
        double max_drawdown = compute_max_drawdown();
        double sharpe = compute_sharpe();

        int win_trades = 0;
        int buy_count = 0;
        int sell_count = 0;
        double buy_notional = 0.0;
        double sell_notional = 0.0;
        for (const auto& r : trades_) {
            if (r.realized_pnl > 0.0)
                ++win_trades;
            const double notional = r.qty * r.price;
            if (r.side == "BUY") {
                ++buy_count;
                buy_notional += notional;
            } else {
                ++sell_count;
                sell_notional += notional;
            }
        }
        double win_rate = trades_.empty() ? 0.0 : static_cast<double>(win_trades) / trades_.size() * 100.0;

        const uint64_t wallclock_end_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const uint64_t wallclock_duration_ms =
            (wallclock_end_ns > wallclock_start_ns_)
                ? (wallclock_end_ns - wallclock_start_ns_) / 1'000'000ULL
                : 0;

        namespace json = boost::json;
        json::object obj;
        obj["starting_capital"] = starting_capital_;
        obj["final_equity"] = final_equity;
        obj["total_pnl"] = total_pnl;
        obj["return_pct"] = return_pct;
        obj["max_drawdown_pct"] = max_drawdown * 100.0;
        obj["sharpe_per_fill"] = sharpe;
        obj["total_fills"] = static_cast<int64_t>(trades_.size());
        obj["win_fills"] = static_cast<int64_t>(win_trades);
        obj["win_rate_pct"] = win_rate;

        // Universal-core metadata + per-side fill aggregates. Older runs
        // without these fields will still parse — frontend treats them as
        // optional.
        obj["buy_count"] = static_cast<int64_t>(buy_count);
        obj["sell_count"] = static_cast<int64_t>(sell_count);
        obj["buy_notional_usd"] = buy_notional;
        obj["sell_notional_usd"] = sell_notional;
        obj["simulation_start"] = metadata_.simulation_start;
        obj["simulation_end"] = metadata_.simulation_end;
        obj["wallclock_duration_ms"] = static_cast<int64_t>(wallclock_duration_ms);
        obj["strategy_name"] = metadata_.strategy_name;
        obj["params_hash"] = metadata_.params_hash;
        obj["git_sha"] = metadata_.git_sha;
        json::array instruments_arr;
        for (const auto& inst : metadata_.instruments)
            instruments_arr.push_back(json::value(inst));
        obj["instruments"] = std::move(instruments_arr);

        // Aggregate markouts. Per horizon: avg over all resolved fills,
        // plus avg restricted to BUY and SELL sides separately so a
        // toxicity asymmetry (e.g. picked off only on BUYs) is visible.
        // Resolved-fill count per horizon is also reported so a low
        // sample size can be spotted.
        json::object markouts_obj;
        for (std::size_t h = 0; h < kMarkoutHorizonsNs.size(); ++h) {
            double sum_all = 0.0; int n_all = 0;
            double sum_buy = 0.0; int n_buy = 0;
            double sum_sell = 0.0; int n_sell = 0;
            for (const auto& t : trades_) {
                const double mk = t.markouts_bps[h];
                if (mk == kUnresolved) continue;
                sum_all += mk; ++n_all;
                if (t.side == "BUY")  { sum_buy  += mk; ++n_buy;  }
                else                  { sum_sell += mk; ++n_sell; }
            }
            json::object horizon_obj;
            horizon_obj["resolved_fills"] = static_cast<int64_t>(n_all);
            horizon_obj["avg_bps"]      = (n_all  > 0) ? sum_all  / n_all  : 0.0;
            horizon_obj["avg_buy_bps"]  = (n_buy  > 0) ? sum_buy  / n_buy  : 0.0;
            horizon_obj["avg_sell_bps"] = (n_sell > 0) ? sum_sell / n_sell : 0.0;
            markouts_obj[kMarkoutHorizonLabels[h]] = std::move(horizon_obj);
        }
        obj["markouts"] = std::move(markouts_obj);

        std::ofstream f(output_dir_ + "/summary.json");
        if (!f)
            throw std::runtime_error("Cannot open " + output_dir_ + "/summary.json");
        f << json::serialize(obj) << '\n';
        bpt::common::log::info("[ResultsCollector] Wrote {}/summary.json", output_dir_);
        bpt::common::log::info(
            "[ResultsCollector] Total PnL: {:.2f}  Return: {:.2f}%  "
            "MaxDD: {:.2f}%  Fills: {}  WinRate: {:.1f}%",
            total_pnl,
            return_pct,
            max_drawdown * 100.0,
            trades_.size(),
            win_rate);
    }
}

}  // namespace bpt::backtester::results
