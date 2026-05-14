#pragma once

/// \file
/// \brief Pricer-side md-gateway control client: publishes MdSubscribeBatch.
///
/// Pricer is one of multiple consumers of md-gateway's subscriptions
/// (strategy is the other today). It stamps every batch with a stable
/// correlation_id so md-gateway's refcounting can scope this consumer's
/// desired set independently of strategy's.
///
/// Replace-semantics: every send is the full desired option universe.
/// Md-gateway diffs this against pricer's previous desired set and
/// only sub/unsubs the venue when the union refcount crosses 0↔1.

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bpt::pricer::md {

class MdSubscribeClient {
public:
    struct InstrumentDesc {
        uint64_t instrument_id;
        std::string exchange;  ///< canonical name, e.g. "DERIBIT"
        std::string symbol;    ///< venue-native, e.g. "BTC-15MAY26-65000-C"
        uint8_t depth{0};      ///< 0 = BBO only; >0 = top-N ladder
    };

    MdSubscribeClient(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \brief Send the full desired option universe to md-gateway.
    ///
    /// `correlation_id` identifies this consumer to md-gateway's
    /// per-consumer refcounting. Pricer should reuse a stable id across
    /// its process lifetime (e.g. derived from process startup time or
    /// a fixed constant) so md-gateway treats successive calls as
    /// replace-the-same-consumer's-desired-set, not different consumers.
    void publish(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments);

private:
    std::unique_ptr<bpt::common::aeron::Publisher> publisher_;
};

}  // namespace bpt::pricer::md
