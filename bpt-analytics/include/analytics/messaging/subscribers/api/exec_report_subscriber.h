#pragma once

/// @file
/// Port: exec-report subscriber. The per-frame dispatch path was lifted
/// to a CRTP-templated concrete subscriber — see
/// `aeron::ExecReportSubscriber<Handler>` (Handler is `AnalyticsService`
/// in prod).

namespace bpt::analytics::messaging::api {

class ExecReportSubscriber {
public:
    virtual ~ExecReportSubscriber() = default;

    virtual int poll(int fragment_limit = 10) = 0;
};

}  // namespace bpt::analytics::messaging::api
