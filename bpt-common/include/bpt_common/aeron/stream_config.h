#pragma once

// yggdrasil/stream_config.h — StreamConfig struct with no external dependencies.
//
// Separated from config.h so services that use a non-YAML config parser
// (e.g. toml++) can include just this header without pulling in yaml-cpp.
//
// Services that use bpt::common::config::load_stream() continue to include <yggdrasil/config.h>,
// which re-exports this header.

#include <cstdint>
#include <string>

namespace bpt::common::config {

// The Aeron addressing unit: a channel URI and a stream ID.
struct StreamConfig {
    std::string channel{"aeron:ipc"};
    int32_t stream_id{0};
};

}  // namespace bpt::common::config
