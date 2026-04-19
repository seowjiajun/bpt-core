#pragma once

// yggdrasil/aeron_utils.h — Helpers for Aeron publication and subscription setup.
//
// Usage:
//   auto pub = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
//   auto sub = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);

#include <Aeron.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace bpt::common::aeron {

// Error handler signature used by the Aeron client when it catches an
// exception on the client-driver conductor thread. Services typically
// want to log + optionally print a backtrace, without crashing the
// process (Aeron is designed to keep running on its side too).
using ErrorHandler = std::function<void(const std::exception&)>;

// Register a publication and spin until Aeron connects it.
// Throws std::runtime_error if not connected within max_retries * 10 ms.
inline std::shared_ptr<::aeron::Publication> wait_for_publication(std::shared_ptr<::aeron::Aeron> aeron,
                                                                  const std::string& channel,
                                                                  int stream_id,
                                                                  int max_retries = 500) {
    long id = aeron->addPublication(channel, stream_id);
    std::shared_ptr<::aeron::Publication> pub;
    for (int i = 0; i <= max_retries; ++i) {
        pub = aeron->findPublication(id);
        if (pub)
            return pub;
        if (i == max_retries)
            throw std::runtime_error("Timed out waiting for publication on " + channel + ":" +
                                     std::to_string(stream_id));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pub;  // unreachable
}

// Register a subscription and spin until Aeron connects it.
// Throws std::runtime_error if not connected within max_retries * 10 ms.
inline std::shared_ptr<::aeron::Subscription> wait_for_subscription(std::shared_ptr<::aeron::Aeron> aeron,
                                                                    const std::string& channel,
                                                                    int stream_id,
                                                                    int max_retries = 500) {
    long id = aeron->addSubscription(channel, stream_id);
    std::shared_ptr<::aeron::Subscription> sub;
    for (int i = 0; i <= max_retries; ++i) {
        sub = aeron->findSubscription(id);
        if (sub)
            return sub;
        if (i == max_retries)
            throw std::runtime_error("Timed out waiting for subscription on " + channel + ":" +
                                     std::to_string(stream_id));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return sub;  // unreachable
}

// Connect to an Aeron media driver. Optional error_handler is wired to
// aeron::Context::errorHandler and fires on the client-driver conductor
// thread whenever Aeron catches an exception. Services should pass a
// handler that logs (and optionally captures a backtrace) — default is
// to do nothing, which makes Aeron fall back to its own stderr output.
inline std::shared_ptr<::aeron::Aeron> connect(const std::string& media_driver_dir = "",
                                               ErrorHandler error_handler = {}) {
    ::aeron::Context ctx;
    if (!media_driver_dir.empty())
        ctx.aeronDir(media_driver_dir);
    if (error_handler)
        ctx.errorHandler(std::move(error_handler));
    return ::aeron::Aeron::connect(ctx);
}

}  // namespace bpt::common::aeron
