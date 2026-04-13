#include "fenrir/refdata/instrument_cache.h"

#include <bifrost_protocol/InstrumentStatus.h>
#include <bifrost_protocol/InstrumentType.h>
#include <bifrost_protocol/OptionSide.h>

#include <cstring>

namespace fenrir::refdata {

namespace {

InstrumentType from_sbe_type(bifrost::protocol::InstrumentType::Value v) {
    using P = bifrost::protocol::InstrumentType;
    switch (v) {
        case P::SPOT:
            return InstrumentType::SPOT;
        case P::FUTURE:
            return InstrumentType::FUTURE;
        case P::PERPETUAL:
            return InstrumentType::PERPETUAL;
        case P::OPTION:
            return InstrumentType::OPTION;
        default:
            return InstrumentType::SPOT;
    }
}

// The SBE "exchange" field is a fixed 8-char array. Exchange names longer
// than 8 characters get silently truncated on the wire (e.g. "HYPERLIQUID"
// → "HYPERLIQ"). Restore the canonical full name here so all downstream
// string comparisons in strategies/configs keep working with the long name.
// Remove once the SBE schema is widened.
std::string canonicalize_exchange(const std::string& truncated) {
    if (truncated == "HYPERLIQ") return "HYPERLIQUID";
    return truncated;
}

InstrumentStatus from_sbe_status(bifrost::protocol::InstrumentStatus::Value v) {
    using P = bifrost::protocol::InstrumentStatus;
    switch (v) {
        case P::ACTIVE:
            return InstrumentStatus::ACTIVE;
        case P::INACTIVE:
            return InstrumentStatus::INACTIVE;
        case P::HALTED:
            return InstrumentStatus::HALTED;
        default:
            return InstrumentStatus::ACTIVE;
    }
}

OptionSide from_sbe_option_side(bifrost::protocol::OptionSide::Value v) {
    using P = bifrost::protocol::OptionSide;
    switch (v) {
        case P::CALL:
            return OptionSide::CALL;
        case P::PUT:
            return OptionSide::PUT;
        default:
            return OptionSide::NA;
    }
}

}  // namespace

void InstrumentCache::apply_snapshot(bifrost::protocol::RefDataSnapshot& msg) {
    cache_.clear();
    snapshot_seq_num_ = msg.snapshotSeqNum();
    last_delta_seq_ = 0;  // reset; first applicable delta has seqNum > snapshot_seq_num_

    auto& g = msg.instruments();
    while (g.hasNext()) {
        g.next();
        Instrument inst;
        inst.instrument_id = g.instrumentId();
        inst.symbol = g.getSymbolAsString();
        inst.exchange = canonicalize_exchange(g.getExchangeAsString());
        inst.base_currency = g.getBaseCurrencyAsString();
        inst.quote_currency = g.getQuoteCurrencyAsString();
        inst.type = from_sbe_type(g.instrumentType());
        inst.status = from_sbe_status(g.status());
        inst.lot_size = g.lotSize();
        inst.tick_size = g.tickSize();
        inst.contract_size = g.contractSize();
        inst.expiry_date = g.expiryDate();
        inst.option_side = from_sbe_option_side(g.optionSide());
        inst.strike_price = g.strikePrice();
        cache_.emplace(inst.instrument_id, std::move(inst));
    }

    snapshot_received_ = true;
}

bool InstrumentCache::apply_delta(bifrost::protocol::RefDataDelta& msg) {
    using DUT = bifrost::protocol::DeltaUpdateType;

    uint64_t seq = msg.deltaSeqNum();

    // Ignore deltas that predate the snapshot (defensive; caller already guards this).
    if (seq <= snapshot_seq_num_)
        return true;

    // Gap detection: every delta after the first must be exactly last+1.
    if (last_delta_seq_ > 0 && seq != last_delta_seq_ + 1)
        return false;

    last_delta_seq_ = seq;

    // NULL_VALUE update type is a heartbeat — sequence tracked above, cache unchanged.
    if (msg.updateType() == DUT::NULL_VALUE)
        return true;

    switch (msg.updateType()) {
        case DUT::ADD:
        case DUT::MODIFY: {
            Instrument inst;
            inst.instrument_id = msg.instrumentId();
            inst.symbol = msg.getSymbolAsString();
            inst.exchange = canonicalize_exchange(msg.getExchangeAsString());
            inst.base_currency = msg.getBaseCurrencyAsString();
            inst.quote_currency = msg.getQuoteCurrencyAsString();
            inst.type = from_sbe_type(msg.instrumentType());
            inst.status = from_sbe_status(msg.status());
            inst.lot_size = msg.lotSize();
            inst.tick_size = msg.tickSize();
            inst.contract_size = msg.contractSize();
            inst.expiry_date = msg.expiryDate();
            inst.option_side = from_sbe_option_side(msg.optionSide());
            inst.strike_price = msg.strikePrice();
            cache_[inst.instrument_id] = std::move(inst);
            break;
        }
        case DUT::REMOVE:
            cache_.erase(msg.instrumentId());
            break;
        default:
            break;
    }
    return true;
}

void InstrumentCache::reset() {
    cache_.clear();
    snapshot_received_ = false;
    snapshot_seq_num_ = 0;
    last_delta_seq_ = 0;
}

std::optional<Instrument> InstrumentCache::get(uint64_t instrument_id) const {
    auto it = cache_.find(instrument_id);
    if (it == cache_.end())
        return std::nullopt;
    return it->second;
}

std::vector<Instrument> InstrumentCache::get_all() const {
    std::vector<Instrument> out;
    out.reserve(cache_.size());
    for (const auto& [_, inst] : cache_) {
        out.push_back(inst);
    }
    return out;
}

}  // namespace fenrir::refdata
