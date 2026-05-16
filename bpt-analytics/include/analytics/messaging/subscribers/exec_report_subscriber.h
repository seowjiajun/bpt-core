#pragma once

/// @file
/// Subscriber for the order-gateway exec-report stream. Decodes the
/// SBE `ExecutionReport` once and dispatches via a std::function
/// callback. Lifts the inline subscriber + handle_exec_report combo
/// from `analytics_service.cpp` so the app stops pulling `<Aeron.h>` into its
/// header.

#include <messages/ExecutionReport.h>

#include <bpt_common/aeron/subscriber.h>
#include <functional>
#include <memory>
#include <string>

namespace bpt::analytics::messaging {

class ExecReportSubscriber {
public:
    using OnReportFn = std::function<void(const bpt::messages::ExecutionReport&)>;

    ExecReportSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 10);

    OnReportFn on_report;

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
};

}  // namespace bpt::analytics::messaging
