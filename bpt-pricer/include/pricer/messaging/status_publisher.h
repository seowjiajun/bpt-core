#pragma once

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::pricer::messaging {

class StatusPublisher {
public:
    StatusPublisher(std::shared_ptr<aeron::Aeron> aeron,
                    const std::string& channel,
                    int32_t stream_id,
                    int pub_timeout_ms = 5000,
                    int pub_poll_interval_ms = 10);

    void publish_heartbeat(uint64_t timestamp_ns, uint64_t seq_num);
    void publish_ready(uint64_t timestamp_ns,
                       uint8_t exchanges_loaded,
                       uint16_t underlying_count,
                       uint32_t point_count);

private:
    std::shared_ptr<aeron::Publication> pub_;
};

}  // namespace bpt::pricer::messaging
