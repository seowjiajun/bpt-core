#pragma once

/// \file
/// \brief Subscribes to bpt-md-gateway's md_data stream (typically 2002) and
/// fans out MdMarketData (BBO) fragments only.
///
/// Sibling of MdTradeSubscriber — both attach to the same stream and filter
/// by templateId so each consumer pays only for the message family it needs.

#include <Aeron.h>

#include <messages/MdMarketData.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class MdMarketDataSubscriber {
public:
    using OnBboFn = std::function<void(bpt::messages::MdMarketData&)>;

    MdMarketDataSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                           const std::string& channel,
                           int stream_id);

    int poll(int fragment_limit = 16);

    OnBboFn on_bbo;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::radar::messaging
