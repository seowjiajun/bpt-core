#include "backtester/results/results_collector.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <boost/json.hpp>
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

ResultsCollector::ResultsCollector(double starting_capital, std::string output_dir)
    : starting_capital_(starting_capital),
      output_dir_(std::move(output_dir)) {
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
    trades_.push_back(std::move(row));
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
}

// ── Output metrics ────────────────────────────────────────────────────────────

double ResultsCollector::compute_max_drawdown() const {
    if (equity_curve_.size() < 2)
        return 0.0;
    double peak = starting_capital_;
    double max_dd = 0.0;
    for (const auto& pt : equity_curve_) {
        peak = std::max(peak, pt.equity);
        double dd = (peak - pt.equity) / peak;
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
             "side,type,qty,price,realized_pnl,equity\n";
        for (const auto& r : trades_) {
            f << std::format("{},{},{},{},{},{},{},{:.10g},{:.10g},{:.10g},{:.10g}\n",
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
        for (const auto& r : trades_)
            if (r.realized_pnl > 0.0)
                ++win_trades;
        double win_rate = trades_.empty() ? 0.0 : static_cast<double>(win_trades) / trades_.size() * 100.0;

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
