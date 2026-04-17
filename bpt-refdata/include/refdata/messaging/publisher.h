#pragma once

#include "refdata/refdata/instrument.h"

#include <memory>
#include <vector>

// Forward decl for Aeron
namespace aeron {
class ExclusivePublication;
class Aeron;
}  // namespace aeron

namespace bpt::refdata::messaging {

class RefdataPublisher {
public:
    RefdataPublisher();  // In real app, would take Aeron context/publication
    ~RefdataPublisher();

    void publishSnapshot(const std::vector<refdata::Instrument>& instruments);
    void publishDelta(const refdata::Instrument& instrument);

private:
    // std::shared_ptr<aeron::ExclusivePublication> snapshot_pub_;
    // std::shared_ptr<aeron::ExclusivePublication> delta_pub_;
};

}  // namespace bpt::refdata::messaging
