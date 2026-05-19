#pragma once

/// \file
/// Port: refdata-snapshot consumer scoped to perpetual instruments only.
/// CRTP-templated concrete in `aeron::RefdataPerpSubscriber<H>`.

#include <cstdint>
#include <string>

namespace bpt::radar::messaging::api {

class RefdataPerpSubscriber {
public:
    struct PerpInfo {
        uint64_t instrument_id;
        std::string underlying;  ///< canonical, e.g. "BTC"
        uint8_t exchange_id;     ///< bpt::messages::ExchangeId::Value
    };

    virtual ~RefdataPerpSubscriber() = default;

    virtual int poll(int fragment_limit = 4) = 0;
};

}  // namespace bpt::radar::messaging::api
