#include "refdata/adapter/deribit/deribit_refdata_adapter.h"

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/types.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <atomic>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <bpt_common/util/tsc_clock.h>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

// Map bpt-refdata InstrumentType to SBE InstrumentType
bpt::messages::InstrumentType::Value to_sbe_inst_type(refdata::InstrumentType t) {
    switch (t) {
        case refdata::InstrumentType::SPOT:
            return bpt::messages::InstrumentType::SPOT;
        case refdata::InstrumentType::PERP:
            return bpt::messages::InstrumentType::PERPETUAL;
        case refdata::InstrumentType::FUTURE:
            return bpt::messages::InstrumentType::FUTURE;
        case refdata::InstrumentType::OPTION:
            return bpt::messages::InstrumentType::OPTION;
        default:
            return bpt::messages::InstrumentType::NULL_VALUE;
    }
}

// Determine instrument type from Deribit kind + settlement_period.
// kind=future with settlement_period=perpetual → PERP
// kind=future otherwise → FUTURE
// kind=option → OPTION
refdata::InstrumentType deribit_to_inst_type(const std::string& kind, const std::string& settlement_period) {
    if (kind == "option")
        return refdata::InstrumentType::OPTION;
    if (kind == "future") {
        if (settlement_period == "perpetual")
            return refdata::InstrumentType::PERP;
        return refdata::InstrumentType::FUTURE;
    }
    if (kind == "spot")
        return refdata::InstrumentType::SPOT;
    return refdata::InstrumentType::UNKNOWN;
}

// Atomic JSON-RPC request id generator.
std::atomic<uint64_t> g_jsonrpc_id{1};

}  // namespace

DeribitRefDataAdapter::DeribitRefDataAdapter(const config::AdapterConfig& cfg,
                                             const ExchangeCredentials& creds,
                                             std::shared_ptr<registry::InstrumentRegistry> registry,
                                             std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : cfg_(cfg),
      registry_(std::move(registry)),
      mapping_(std::move(mapping)),
      client_id_(creds.client_id),
      client_secret_(creds.client_secret) {
    if (client_id_.empty() || client_secret_.empty()) {
        bpt::common::log::warn(
            "[DeribitRefData] Deribit credentials not set — "
            "private endpoints (fee tiers) will not be available; "
            "using per-instrument fees from get_instruments instead.");
    }
}

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 envelope + POST via shared RestClient.
// ---------------------------------------------------------------------------
std::string DeribitRefDataAdapter::post_jsonrpc(const std::string& method,
                                                 const std::string& params_json) const {
    json req_body;
    req_body["jsonrpc"] = "2.0";
    req_body["id"] = g_jsonrpc_id.fetch_add(1, std::memory_order_relaxed);
    req_body["method"] = method;
    req_body["params"] = json::parse(params_json);
    return rest_client_->post("/api/v2", req_body.dump());
}

// ---------------------------------------------------------------------------
// Instrument parser
// ---------------------------------------------------------------------------
void DeribitRefDataAdapter::parse_instruments(const std::string& body, uint64_t collected_ts) {
    auto j = json::parse(body);

    if (j.contains("error")) {
        bpt::common::log::error("[DeribitRefData] get_instruments error: {}", j["error"].value("message", "unknown"));
        return;
    }

    const auto& result = j["result"];
    if (!result.is_array()) {
        bpt::common::log::error("[DeribitRefData] get_instruments result is not an array");
        return;
    }

    int loaded = 0;
    for (const auto& sym : result) {
        bool is_active = sym.value("is_active", false);
        if (!is_active)
            continue;

        std::string instrument_name = sym.value("instrument_name", "");
        if (instrument_name.empty())
            continue;

        std::string kind = sym.value("kind", "");
        std::string settlement_period = sym.value("settlement_period", "");
        std::string base = sym.value("base_currency", "");
        std::string quote = sym.value("quote_currency", "");

        refdata::InstrumentType itype = deribit_to_inst_type(kind, settlement_period);
        if (itype == refdata::InstrumentType::UNKNOWN)
            continue;

        // Instrument mapping lookup — Deribit symbols are unique per type (BTC-PERPETUAL, BTC-28MAR25, etc.)
        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_DERIBIT, instrument_name);
        if (!cid)
            continue;

        refdata::Instrument inst;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_DERIBIT);
        inst.venue = "DERIBIT";
        inst.venue_symbol = instrument_name;
        inst.display_name = instrument_name;
        inst.base = base;
        inst.quote = quote;
        inst.inst_type = itype;
        inst.status = refdata::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;

        // tick_size, min_trade_amount (lot_size), contract_size (contract_multiplier)
        inst.tick_size = sym.value("tick_size", 0.0);
        inst.lot_size = sym.value("min_trade_amount", 0.0);
        inst.contract_multiplier = sym.value("contract_size", 1.0);

        // Expiry for FUTURE and OPTION types (expiration_timestamp is ms)
        if (itype == refdata::InstrumentType::FUTURE || itype == refdata::InstrumentType::OPTION) {
            uint64_t exp_ms = sym.value("expiration_timestamp", static_cast<uint64_t>(0));
            if (exp_ms > 0)
                inst.expiry_timestamp = exp_ms * 1'000'000ULL;  // ms → ns
        }

        // Strike price for OPTIONS
        if (itype == refdata::InstrumentType::OPTION) {
            double strike = sym.value("strike", 0.0);
            if (strike > 0.0)
                inst.strike_price = strike;
        }

        if (registry_->update_if_changed(inst) && on_instrument_delta)
            on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, collected_ts);
        ++loaded;

        // Fees come directly from instrument data (maker_commission, taker_commission).
        // These are decimal fractions (e.g. 0.0003 = 3 bps).
        double maker = sym.value("maker_commission", 0.0);
        double taker = sym.value("taker_commission", 0.0);

        refdata::FeeScheduleState fs;
        fs.exchange_id = bpt::messages::ExchangeId::DERIBIT;
        fs.instrument_id = inst.inst_uid;
        fs.instrument_type = to_sbe_inst_type(itype);
        fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
        fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
        fs.updated_ts = collected_ts;

        if (on_fee_schedule)
            on_fee_schedule(fs);
    }
    bpt::common::log::info("[DeribitRefData] Loaded {} active instruments from get_instruments", loaded);
}

// ---------------------------------------------------------------------------
// fetchSnapshot — blocking
// ---------------------------------------------------------------------------
namespace {
void fetch_all_instruments(const std::vector<std::string>& currencies,
                           const std::vector<std::string>& kinds,
                           const char* log_prefix,
                           const std::function<std::string(const std::string&, const std::string&)>& do_fetch,
                           const std::function<void(const std::string&)>& on_body) {
    for (const auto& currency : currencies) {
        for (const auto& kind : kinds) {
            try {
                on_body(do_fetch(currency, kind));
            } catch (const std::exception& e) {
                bpt::common::log::error("[DeribitRefData] {} {} {} failed: {}", log_prefix, currency, kind, e.what());
            }
        }
    }
}
}  // namespace

void DeribitRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info("[DeribitRefData] Starting snapshot fetch...");

    if (!rest_client_) {
        const std::string host = cfg_.rest_host.empty() ? "test.deribit.com" : cfg_.rest_host;
        rest_client_ = std::make_unique<bpt::refdata::http::RestClient>(host, cfg_.rest_port, cfg_.use_tls);
    }

    const uint64_t ts = now_ns();
    const std::vector<std::string> currencies = {"BTC", "ETH"};
    const std::vector<std::string> kinds = {"future", "option"};

    fetch_all_instruments(
        currencies, kinds, "snapshot",
        [this](const std::string& currency, const std::string& kind) {
            json params;
            params["currency"] = currency;
            params["kind"] = kind;
            params["expired"] = false;
            return post_jsonrpc("public/get_instruments", params.dump());
        },
        [this, ts](const std::string& body) { parse_instruments(body, ts); });

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info("[DeribitRefData] Snapshot complete. Registry has {} instruments.", registry_->count());
}

// ---------------------------------------------------------------------------
// fetchInstrumentListing — called hourly
// ---------------------------------------------------------------------------
void DeribitRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info("[DeribitRefData] Hourly instrument listing refresh...");
    if (!rest_client_) {
        const std::string host = cfg_.rest_host.empty() ? "test.deribit.com" : cfg_.rest_host;
        rest_client_ = std::make_unique<bpt::refdata::http::RestClient>(host, cfg_.rest_port, cfg_.use_tls);
    }

    const uint64_t ts = now_ns();
    const std::vector<std::string> currencies = {"BTC", "ETH"};
    const std::vector<std::string> kinds = {"future", "option"};

    fetch_all_instruments(
        currencies, kinds, "hourly",
        [this](const std::string& currency, const std::string& kind) {
            json params;
            params["currency"] = currency;
            params["kind"] = kind;
            params["expired"] = false;
            return post_jsonrpc("public/get_instruments", params.dump());
        },
        [this, ts](const std::string& body) { parse_instruments(body, ts); });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::vector<std::string> DeribitRefDataAdapter::get_perp_instrument_names() const {
    std::vector<std::string> names;
    registry_->for_each([&](const refdata::Instrument& inst) {
        if (inst.venue == "DERIBIT" && inst.inst_type == refdata::InstrumentType::PERP)
            names.push_back(inst.venue_symbol);
    });
    return names;
}

}  // namespace bpt::refdata::adapter
