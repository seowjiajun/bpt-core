#pragma once

/// \file
/// \brief Tiny refdata-snapshot consumer: tracks perp instruments only.
///
/// Radar needs to map (perp instrument_id) → (underlying, exchange) to attach
/// incoming FundingRate updates to the right MarketColor entry. The full
/// pricer-side RefdataSubscriber tracks options + perps + ports; radar only
/// needs the perp half, so this is a narrow subscriber that extracts
/// instrumentType == PERPETUAL rows from RefDataSnapshot and surfaces them
/// via callback.

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class RefdataPerpSubscriber {
public:
    struct PerpInfo {
        uint64_t instrument_id;
        std::string underlying;  ///< canonical, e.g. "BTC"
        uint8_t exchange_id;     ///< bpt::messages::ExchangeId::Value
    };

    using OnPerpFn = std::function<void(const PerpInfo&)>;

    RefdataPerpSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 4);

    OnPerpFn on_perp;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
    std::unique_ptr<aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::radar::messaging
