#pragma once

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/AccountSnapshotRequest.h>
#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>

#include <functional>
#include <memory>
#include <string>

namespace bpt::order_gateway::messaging {

using OnNewOrderFn = std::function<void(const bpt::messages::NewOrder&)>;
using OnCancelFn = std::function<void(const bpt::messages::CancelOrder&)>;
using OnCancelAllFn = std::function<void(const bpt::messages::CancelAll&)>;
using OnModifyFn = std::function<void(const bpt::messages::ModifyOrder&)>;
using OnAccountSnapshotRequestFn = std::function<void(const bpt::messages::AccountSnapshotRequest&)>;

class OrderSubscriber {
public:
    OrderSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    // Poll for incoming order messages. Returns number of fragments processed.
    int poll(int fragment_limit = 10);

    // Callbacks — set before calling poll().
    OnNewOrderFn on_new_order;
    OnCancelFn on_cancel;
    OnCancelAllFn on_cancel_all;
    OnModifyFn on_modify;
    OnAccountSnapshotRequestFn on_account_snapshot_request;

private:
    void handle_fragment(aeron::AtomicBuffer& buf,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& hdr);

    std::shared_ptr<aeron::Subscription> subscription_;
    std::unique_ptr<aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::order_gateway::messaging
