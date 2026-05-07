#pragma once

/// \file
/// \brief Outbound port: fee schedule publish.

namespace bpt::refdata::refdata {
struct FeeScheduleState;
}

namespace bpt::refdata::port {

class IFeeScheduleSink {
public:
    virtual ~IFeeScheduleSink() = default;

    virtual void publish(const refdata::FeeScheduleState& fs) = 0;
};

}  // namespace bpt::refdata::port
