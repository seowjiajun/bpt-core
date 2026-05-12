#pragma once

/// \file
/// \brief Recording subclasses of bpt-md-gateway venue adapters.
///
/// Each Recording* subclass overrides handle_frame() to tee the raw WS
/// payload into a shared Tape *before* calling the base's parser.
/// The mdgw adapter source stays untouched — recording is bpt-tape-only.
///
/// handle_frame() is the IO-thread seam invoked once per application
/// frame (post-protocol filtering: no ping/pong keepalive reaches the
/// tape). It is noexcept hot-path code; a tape write failure aborts
/// the process so systemd's Restart=always recycles us with a clean
/// journal entry — silent drops are the failure mode that caused the
/// disk-full incident this code now guards against.
///
/// Templated on Pub so bpt-tape can instantiate with NoopMdPublisher
/// and have the publish() chain dead-code-eliminated by the optimizer.

#include "md_gateway/adapter/binance/binance_md_adapter.h"
#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/adapter/deribit/deribit_md_adapter.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_adapter.h"
#include "md_gateway/adapter/okx/okx_md_adapter.h"
#include "tape/io/tape.h"

#include <messages/ExchangeRegistry.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

namespace bpt::tape::adapter {

/// \brief Declare a Recording<Venue>MdAdapter subclass that tees raw
///        frames into a shared tape before delegating to the parent.
///
/// Macro because each subclass differs only in (Class, BaseClass) and
/// the body is mechanically identical. Inheritance is the canonical
/// way to inject behavior without modifying the mdgw adapter source.
#define BPT_DECLARE_RECORDING_ADAPTER(Class, BaseClass)                                   \
    template <class Pub>                                                                  \
    class Class : public ::bpt::md_gateway::adapter::BaseClass<Pub> {                     \
    public:                                                                               \
        Class(std::shared_ptr<::bpt::tape::io::Tape> tape,                                \
              const ::bpt::md_gateway::config::AdapterConfig& cfg,                        \
              std::shared_ptr<Pub> md_pub)                                                \
            : ::bpt::md_gateway::adapter::BaseClass<Pub>(cfg, std::move(md_pub)),         \
              tape_(std::move(tape)) {}                                                   \
                                                                                          \
    protected:                                                                            \
        void handle_frame(std::string_view payload, uint64_t recv_ns) noexcept override { \
            if (tape_ && !tape_->write_frame(recv_ns, payload)) {                         \
                /* Tape already logged the cause via Quill (async). Synchronous */        \
                /* stderr line so journald captures the fatal even if Quill hasn't  */    \
                /* drained before abort().                                          */    \
                std::fputs(                                                               \
                    "[FATAL] bpt-tape: Tape::write_frame failed; "                        \
                    "aborting (Restart=always recycles).\n",                              \
                    stderr);                                                              \
                std::fflush(stderr);                                                      \
                std::abort();                                                             \
            }                                                                             \
            ::bpt::md_gateway::adapter::BaseClass<Pub>::handle_frame(payload, recv_ns);   \
        }                                                                                 \
                                                                                          \
    private:                                                                              \
        std::shared_ptr<::bpt::tape::io::Tape> tape_;                                     \
    };

BPT_DECLARE_RECORDING_ADAPTER(RecordingBinanceMdAdapter, BinanceMdAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingOkxMdAdapter, OkxMdAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingHyperliquidMdAdapter, HyperliquidMdAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingDeribitMdAdapter, DeribitMdAdapter)

#undef BPT_DECLARE_RECORDING_ADAPTER

/// \brief Build the venue-specific recording adapter for `exch_id`.
///
/// Returns nullptr if the exchange is in messages/exchanges.yaml but
/// bpt-tape has no recording subclass for it — caller decides whether
/// to throw or fall back. `exch_id` is the enum value (what
/// `ExchangeRegistry::from_name()` returns through its std::optional),
/// not the wrapping struct.
template <class Pub>
inline std::shared_ptr<::bpt::md_gateway::adapter::IAdapter> make_recording_adapter(
    bpt::messages::ExchangeId::Value exch_id,
    std::shared_ptr<::bpt::tape::io::Tape> tape,
    const ::bpt::md_gateway::config::AdapterConfig& cfg,
    std::shared_ptr<Pub> md_pub) {
    using namespace bpt::messages;
    switch (exch_id) {
        case ExchangeId::BINANCE:
            return std::make_shared<RecordingBinanceMdAdapter<Pub>>(tape, cfg, md_pub);
        case ExchangeId::OKX:
            return std::make_shared<RecordingOkxMdAdapter<Pub>>(tape, cfg, md_pub);
        case ExchangeId::HYPERLIQUID:
            return std::make_shared<RecordingHyperliquidMdAdapter<Pub>>(tape, cfg, md_pub);
        case ExchangeId::DERIBIT:
            return std::make_shared<RecordingDeribitMdAdapter<Pub>>(tape, cfg, md_pub);
        default:
            return nullptr;
    }
}

}  // namespace bpt::tape::adapter
