#pragma once

/// @file
/// Recording subclasses of the bpt-md-gateway venue adapters. Each overrides
/// on_frame() to tee the raw WS payload to a shared RawSpool BEFORE calling
/// the parent's on_frame() — preserves the existing frame-queue + parser
/// pipeline while adding the recording tap. The mdgw adapter source is
/// untouched; recording is a md-recorder-only concern.

#include "bpt_common/recorder/raw_spool.h"
#include "md_gateway/adapter/binance/binance_adapter.h"
#include "md_gateway/adapter/deribit/deribit_adapter.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_adapter.h"
#include "md_gateway/adapter/okx/okx_adapter.h"

#include <memory>
#include <string_view>
#include <utility>

namespace bpt::md_recorder::adapter {

#define BPT_DECLARE_RECORDING_ADAPTER(Class, BaseClass)                                       \
    class Class : public ::bpt::md_gateway::adapter::BaseClass {                              \
    public:                                                                                   \
        Class(std::shared_ptr<::bpt::common::recorder::RawSpool> spool,                       \
              const ::bpt::md_gateway::config::AdapterConfig& cfg,                            \
              std::shared_ptr<::bpt::md_gateway::messaging::IMdPublisher> md_pub)             \
            : ::bpt::md_gateway::adapter::BaseClass(cfg, std::move(md_pub)),                  \
              spool_(std::move(spool)) {}                                                     \
                                                                                              \
    protected:                                                                                \
        void on_frame(std::string_view payload, uint64_t recv_ns) override {                  \
            if (spool_) spool_->write_frame(recv_ns, payload);                                \
            ::bpt::md_gateway::adapter::BaseClass::on_frame(payload, recv_ns);                \
        }                                                                                     \
                                                                                              \
    private:                                                                                  \
        std::shared_ptr<::bpt::common::recorder::RawSpool> spool_;                            \
    };

BPT_DECLARE_RECORDING_ADAPTER(RecordingBinanceAdapter, BinanceAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingOkxAdapter, OkxAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingHyperliquidAdapter, HyperliquidAdapter)
BPT_DECLARE_RECORDING_ADAPTER(RecordingDeribitAdapter, DeribitAdapter)

#undef BPT_DECLARE_RECORDING_ADAPTER

}  // namespace bpt::md_recorder::adapter
