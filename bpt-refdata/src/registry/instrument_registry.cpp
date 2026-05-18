#include "refdata/registry/instrument_registry.h"

#include <functional>
#include <mutex>

namespace bpt::refdata::registry {

void InstrumentRegistry::add(const model::Instrument& instrument) {
    std::unique_lock lock(mutex_);
    instruments_by_uid_[instrument.inst_uid] = instrument;

    // Secondary index key includes type to handle exchanges where the same symbol is
    // used for multiple instrument types (e.g. Binance BTCUSDT for both SPOT and PERP).
    // Format: "VENUE:SYMBOL:TYPE" e.g. "BINANCE:BTCUSDT:2"
    std::string key =
        instrument.venue + ":" + instrument.venue_symbol + ":" + std::to_string(static_cast<int>(instrument.inst_type));
    uid_by_venue_symbol_[key] = instrument.inst_uid;
}

void InstrumentRegistry::update(const model::Instrument& instrument) {
    // For now, update behaves same as add (upsert)
    add(instrument);
}

void InstrumentRegistry::remove(uint64_t inst_uid) {
    std::unique_lock lock(mutex_);
    auto it = instruments_by_uid_.find(inst_uid);
    if (it != instruments_by_uid_.end()) {
        const auto& inst = it->second;
        std::string key = inst.venue + ":" + inst.venue_symbol + ":" + std::to_string(static_cast<int>(inst.inst_type));
        uid_by_venue_symbol_.erase(key);
        instruments_by_uid_.erase(it);
    }
}

std::optional<model::Instrument> InstrumentRegistry::get(uint64_t inst_uid) const {
    std::shared_lock lock(mutex_);
    auto it = instruments_by_uid_.find(inst_uid);
    if (it != instruments_by_uid_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<model::Instrument> InstrumentRegistry::get(const std::string& venue,
                                                           const std::string& venue_symbol) const {
    std::shared_lock lock(mutex_);
    // Linear scan — most callers use venues where symbols are unique per type.
    for (const auto& [uid, inst] : instruments_by_uid_) {
        if (inst.venue == venue && inst.venue_symbol == venue_symbol)
            return inst;
    }
    return std::nullopt;
}

std::optional<model::Instrument> InstrumentRegistry::get(const std::string& venue,
                                                           const std::string& venue_symbol,
                                                           model::InstrumentType type) const {
    std::shared_lock lock(mutex_);
    std::string key = venue + ":" + venue_symbol + ":" + std::to_string(static_cast<int>(type));
    auto it_uid = uid_by_venue_symbol_.find(key);
    if (it_uid != uid_by_venue_symbol_.end()) {
        auto it_inst = instruments_by_uid_.find(it_uid->second);
        if (it_inst != instruments_by_uid_.end())
            return it_inst->second;
    }
    return std::nullopt;
}

bool InstrumentRegistry::update_if_changed(const model::Instrument& instrument) {
    std::unique_lock lock(mutex_);
    auto it = instruments_by_uid_.find(instrument.inst_uid);
    if (it == instruments_by_uid_.end()) {
        instruments_by_uid_[instrument.inst_uid] = instrument;
        std::string key = instrument.venue + ":" + instrument.venue_symbol + ":" +
                          std::to_string(static_cast<int>(instrument.inst_type));
        uid_by_venue_symbol_[key] = instrument.inst_uid;
        return true;
    }
    const auto& existing = it->second;
    if (existing.tick_size == instrument.tick_size && existing.lot_size == instrument.lot_size &&
        existing.contract_multiplier == instrument.contract_multiplier && existing.status == instrument.status &&
        existing.expiry_timestamp == instrument.expiry_timestamp && existing.strike_price == instrument.strike_price) {
        return false;
    }
    it->second = instrument;
    return true;
}

std::size_t InstrumentRegistry::count() const {
    std::shared_lock lock(mutex_);
    return instruments_by_uid_.size();
}

void InstrumentRegistry::for_each(const std::function<void(const model::Instrument&)>& fn) const {
    std::shared_lock lock(mutex_);
    for (const auto& [uid, inst] : instruments_by_uid_)
        fn(inst);
}

std::vector<model::Instrument> InstrumentRegistry::getAll() const {
    std::shared_lock lock(mutex_);
    std::vector<model::Instrument> result;
    result.reserve(instruments_by_uid_.size());
    for (const auto& pair : instruments_by_uid_) {
        result.push_back(pair.second);
    }
    return result;
}

}  // namespace bpt::refdata::registry
