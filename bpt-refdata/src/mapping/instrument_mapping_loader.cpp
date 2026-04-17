#include "refdata/mapping/instrument_mapping_loader.h"

#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <stdexcept>
#include <yggdrasil/logging.h>

namespace bpt::refdata::mapping {

using json = nlohmann::json;

void InstrumentMappingLoader::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("instrument_mapping.json not found at: " + path);

    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("instrument_mapping.json parse error: ") + e.what());
    }

    if (!j.contains("forward") || !j.contains("reverse"))
        throw std::runtime_error("instrument_mapping.json missing 'forward' or 'reverse' keys");

    std::unordered_map<std::string, uint32_t> new_forward;
    std::unordered_map<uint32_t, ReverseEntry> new_reverse;

    for (const auto& [key, val] : j["forward"].items())
        new_forward[key] = val.get<uint32_t>();

    for (const auto& [id_str, entry] : j["reverse"].items()) {
        uint32_t cid = static_cast<uint32_t>(std::stoul(id_str));
        ReverseEntry re;
        re.info.base = entry.value("base", "");
        re.info.quote = entry.value("quote", "");
        re.info.type = entry.value("type", "");
        for (const auto& [ex_str, sym] : entry["exchanges"].items()) {
            uint8_t ex_id = static_cast<uint8_t>(std::stoul(ex_str));
            re.exchanges[ex_id] = sym.get<std::string>();
        }
        new_reverse[cid] = std::move(re);
    }

    std::size_t count = new_reverse.size();
    uint64_t exported_at = j.value("exported_at", uint64_t{0});

    {
        std::unique_lock lock(mutex_);
        forward_ = std::move(new_forward);
        reverse_ = std::move(new_reverse);
        instrument_count_ = count;
    }

    ygg::log::info("[InstrumentMapping] Loaded {} instruments, exported_at={}", count, exported_at);
}

std::optional<uint32_t> InstrumentMappingLoader::try_resolve_canonical_id(uint8_t exchange_id,
                                                                          const std::string& exchange_symbol) const {
    std::string key = std::to_string(exchange_id) + "_" + exchange_symbol;
    std::shared_lock lock(mutex_);
    auto it = forward_.find(key);
    if (it == forward_.end())
        return std::nullopt;
    return it->second;
}

uint32_t InstrumentMappingLoader::resolve_canonical_id(uint8_t exchange_id, const std::string& exchange_symbol) const {
    auto result = try_resolve_canonical_id(exchange_id, exchange_symbol);
    if (!result) {
        ygg::log::warn("[InstrumentMapping] No canonical ID for exchange={} symbol={}", exchange_id, exchange_symbol);
        return UNKNOWN_INSTRUMENT;
    }
    return *result;
}

std::string InstrumentMappingLoader::resolve_symbol(uint32_t canonical_id, uint8_t exchange_id) const {
    std::shared_lock lock(mutex_);
    auto it = reverse_.find(canonical_id);
    if (it == reverse_.end()) {
        ygg::log::warn("[InstrumentMapping] No reverse entry for canonical_id={}", canonical_id);
        return {};
    }
    auto sit = it->second.exchanges.find(exchange_id);
    if (sit == it->second.exchanges.end()) {
        ygg::log::warn("[InstrumentMapping] No symbol for canonical_id={} exchange={}", canonical_id, exchange_id);
        return {};
    }
    return sit->second;
}

std::optional<InstrumentInfo> InstrumentMappingLoader::get_instrument_info(uint32_t canonical_id) const {
    std::shared_lock lock(mutex_);
    auto it = reverse_.find(canonical_id);
    if (it == reverse_.end())
        return std::nullopt;
    return it->second.info;
}

std::size_t InstrumentMappingLoader::instrument_count() const {
    std::shared_lock lock(mutex_);
    return instrument_count_;
}

}  // namespace bpt::refdata::mapping
