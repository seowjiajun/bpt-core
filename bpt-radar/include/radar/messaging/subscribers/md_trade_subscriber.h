#pragma once

/// \file
/// \brief Subscribes to bpt-md-gateway's md_data stream (typically 2002) and
/// fans out MdTrade fragments only.
///
/// md_data carries MdMarketData (BBO), MdOrderBook (L2), and MdTrade — radar
/// only needs trades for the flow-color section, so the handler short-circuits
/// other template IDs without decoding them.

#include <Aeron.h>

#include <messages/MdTrade.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class MdTradeSubscriber {
public:
    using OnTradeFn = std::function<void(bpt::messages::MdTrade&)>;

    MdTradeSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 16);

    OnTradeFn on_trade;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::radar::messaging
