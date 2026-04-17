#pragma once

#include "refdata/refdata/instrument.h"

#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::refdata::registry {

class InstrumentRegistry {
public:
    InstrumentRegistry() = default;
    ~InstrumentRegistry() = default;

    // Delete copy/move for thread safety simplicity unless needed
    InstrumentRegistry(const InstrumentRegistry&) = delete;
    InstrumentRegistry& operator=(const InstrumentRegistry&) = delete;

    void add(const refdata::Instrument& instrument);
    void update(const refdata::Instrument& instrument);
    void remove(uint64_t inst_uid);

    // Update the instrument only if its content-significant fields changed
    // (tick_size, lot_size, contract_multiplier, status, expiry, strike).
    // Returns true if the instrument was new or actually changed, false if unchanged.
    bool update_if_changed(const refdata::Instrument& instrument);

    [[nodiscard]] std::optional<refdata::Instrument> get(uint64_t inst_uid) const;
    [[nodiscard]] std::optional<refdata::Instrument> get(const std::string& venue,
                                                         const std::string& venue_symbol) const;
    [[nodiscard]] std::optional<refdata::Instrument> get(const std::string& venue,
                                                         const std::string& venue_symbol,
                                                         refdata::InstrumentType type) const;

    // Returns the number of instruments in the registry.
    [[nodiscard]] std::size_t count() const;

    // Invoke fn(instrument) for every instrument under a shared lock.
    // Avoids allocating a full copy of the registry for callers that only need to iterate.
    void for_each(const std::function<void(const refdata::Instrument&)>& fn) const;

    // Kept for backward compatibility where a full copy is genuinely needed.
    [[nodiscard]] std::vector<refdata::Instrument> getAll() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, refdata::Instrument> instruments_by_uid_;

    // Secondary index: venue:venue_symbol -> inst_uid
    std::unordered_map<std::string, uint64_t> uid_by_venue_symbol_;
};

}  // namespace bpt::refdata::registry
