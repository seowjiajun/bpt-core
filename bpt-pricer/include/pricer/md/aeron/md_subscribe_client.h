#pragma once

/// \file
/// Aeron-backed concrete for api::MdSubscribeClient. Publishes
/// MdSubscribeBatch on the md-gateway control stream.
///
/// Replace-semantics: every send is the full desired option universe.
/// Md-gateway diffs this against pricer's previous desired set and
/// only sub/unsubs the venue when the union refcount crosses 0↔1.

#include "pricer/md/api/md_subscribe_client.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/stream_config.h>
#include <memory>
#include <string>

namespace bpt::pricer::md::aeron {

class MdSubscribeClient final : public api::MdSubscribeClient {
public:
    MdSubscribeClient(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    void publish(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) override;

private:
    std::unique_ptr<bpt::common::aeron::Publisher> publisher_;
};

}  // namespace bpt::pricer::md::aeron
