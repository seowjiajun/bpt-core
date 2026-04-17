#include "strategy/dashboard/portfolio_snapshot_publisher.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>
#include <yggdrasil/logging.h>

namespace bpt::strategy::dashboard {

PortfolioSnapshotPublisher::PortfolioSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                                       const std::string& channel,
                                                       int32_t stream_id) {
    if (channel.empty() || stream_id == 0) return;

    const int64_t reg_id = aeron->addPublication(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        pub_ = aeron->findPublication(reg_id);
        if (pub_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (pub_) {
        ygg::log::info("[Strategy] Dashboard snapshot publication ready on stream {}", stream_id);
    } else {
        ygg::log::warn("[Strategy] Dashboard snapshot publication unavailable");
    }
}

void PortfolioSnapshotPublisher::publish_if_due(const strategy::PortfolioState& state, uint64_t now_ns) {
    if (!pub_) return;
    if (now_ns - last_publish_ns_ < kIntervalNs) return;
    if (state.legs.empty() && state.surface_points.empty()) return;

    last_publish_ns_ = now_ns;
    publish(state, now_ns);
}

void PortfolioSnapshotPublisher::publish(const strategy::PortfolioState& state, uint64_t now_ns) {
    nlohmann::json j;
    j["type"] = "portfolio";
    j["ts"] = now_ns;
    j["delta"] = state.portfolio_delta;
    j["gamma"] = state.portfolio_gamma;
    j["vega"] = state.portfolio_vega;
    j["theta"] = state.portfolio_theta;
    j["unrealizedPnl"] = state.total_unrealized_pnl;
    j["realizedPnl"] = state.total_realized_pnl;

    auto& legs = j["legs"] = nlohmann::json::array();
    for (const auto& l : state.legs) {
        legs.push_back({
            {"instrumentId", l.instrument_id},
            {"symbol", l.symbol},
            {"underlying", l.underlying},
            {"expiry", l.expiry_date},
            {"strike", l.strike},
            {"isCall", l.is_call},
            {"isOption", l.is_option},
            {"qty", l.qty},
            {"entryPrice", l.entry_price},
            {"markPrice", l.mark_price},
            {"iv", l.iv},
            {"delta", l.delta},
            {"gamma", l.gamma},
            {"vega", l.vega},
            {"theta", l.theta},
            {"unrealizedPnl", l.unrealized_pnl},
        });
    }

    auto& surf = j["surface"] = nlohmann::json::array();
    for (const auto& sp : state.surface_points) {
        surf.push_back({
            {"instrumentId", sp.instrument_id},
            {"expiry", sp.expiry_date},
            {"strike", sp.strike},
            {"isCall", sp.is_call},
            {"iv", sp.iv},
            {"bidIv", sp.bid_iv},
            {"askIv", sp.ask_iv},
            {"delta", sp.delta},
            {"tte", sp.time_to_expiry},
        });
    }

    auto payload = j.dump();
    aeron::AtomicBuffer buf(reinterpret_cast<uint8_t*>(payload.data()),
                            static_cast<aeron::util::index_t>(payload.size()));
    pub_->offer(buf, 0, static_cast<aeron::util::index_t>(payload.size()));
}

}  // namespace bpt::strategy::dashboard
