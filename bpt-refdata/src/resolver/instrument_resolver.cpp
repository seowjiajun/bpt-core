#include "refdata/resolver/instrument_resolver.h"

namespace bpt::refdata::resolver {

InstrumentResolver::InstrumentResolver(std::shared_ptr<registry::InstrumentRegistry> registry)
    : registry_(std::move(registry)) {}

std::optional<uint64_t> InstrumentResolver::resolve(const std::string& venue, const std::string& venue_symbol) const {
    if (!registry_)
        return std::nullopt;

    auto inst = registry_->get(venue, venue_symbol);
    if (inst) {
        return inst->inst_uid;
    }
    return std::nullopt;
}

}  // namespace bpt::refdata::resolver
