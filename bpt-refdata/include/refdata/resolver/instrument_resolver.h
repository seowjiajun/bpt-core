#pragma once

#include "refdata/registry/instrument_registry.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bpt::refdata::resolver {

class InstrumentResolver {
public:
    explicit InstrumentResolver(std::shared_ptr<registry::InstrumentRegistry> registry);

    // Resolves exact match
    [[nodiscard]] std::optional<uint64_t> resolve(const std::string& venue, const std::string& venue_symbol) const;

    // Future: helper to resolve fuzzy queries or other keys
    // [[nodiscard]] std::vector<uint64_t> search(const std::string& query) const;

private:
    std::shared_ptr<registry::InstrumentRegistry> registry_;
};

}  // namespace bpt::refdata::resolver
