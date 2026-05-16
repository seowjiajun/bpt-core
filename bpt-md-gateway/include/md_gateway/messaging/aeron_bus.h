#pragma once

/// \file
/// \brief Composition root that wires the Aeron-backed implementations of the messaging ports.
///
/// MdGatewayService talks to the four messaging ports (api::MdControlSubscriber +
/// api::AckPublisher + api::FundingRatePublisher + api::InstrumentStatsPublisher
/// + concrete MdPublisher) without knowing how they are implemented.
/// `AeronBus::build()` is the single place that constructs the Aeron-backed
/// concrete classes and bundles them into a struct the app accepts in its
/// constructor — swap this factory for a different one (e.g. an in-memory bus
/// for seam tests, a NoopMdPublisher for bpt-tape) and the app code is
/// unchanged.
///
/// The MD sink is exposed by concrete type rather than via a port: venue
/// adapters are templated on the publisher type so the publish() chain
/// inlines all the way to the wire — see md_gateway/md/validating_publisher.h.
/// Variation at the MD sink is therefore a compile-time choice (which `Pub`
/// you instantiate the adapters with), not a runtime polymorphism.
///
/// Lifetime: AeronBus owns the publisher and subscriber objects but
/// hands ownership to MdGatewayService at construction; AeronBus itself is
/// a value type that can be moved out at the wiring site.

#include "md_gateway/messaging/publishers/api/ack_publisher.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"
#include "md_gateway/messaging/publishers/md_publisher.h"
#include "md_gateway/messaging/subscribers/api/md_control_subscriber.h"

#include <Aeron.h>

#include <memory>

namespace bpt::md_gateway::config {
struct Settings;
}

namespace bpt::md_gateway::messaging {

/// \brief Bundle of messaging-port implementations handed to MdGatewayService.
///
/// Each field is one port. Four are exposed via interface type so that
/// alternate implementations (test fakes, recorder no-ops) can substitute
/// without rebuilding the app; the MD sink is concrete because the hot
/// path is templated on `Pub` (see file-level doc above).
struct AeronBus {
    /// \brief Inbound: SBE `MdSubscribeBatch` control fragments from strategy.
    ///
    /// Polled from MdGatewayService::run(); each fragment dispatched to the
    /// SubscriptionManager.
    std::unique_ptr<api::MdControlSubscriber> control_source;

    /// \brief Outbound: normalised market-data on stream 2002.
    ///
    /// Concrete type rather than interface — venue adapters are
    /// templated on `Pub`, so the decoder→ValidatingPublisher→MdPublisher
    /// chain is fully devirtualised. Swap by instantiating the templated
    /// adapters with a different concrete type at the composition root
    /// (e.g. bpt-tape uses NoopMdPublisher to drop without writing).
    std::shared_ptr<MdPublisher> md_sink;

    /// \brief Outbound: subscription ACKs + service heartbeats to strategy.
    std::unique_ptr<api::AckPublisher> ack_sink;

    /// \brief Outbound: per-instrument funding-rate updates on stream 1005.
    ///
    /// Wired into each adapter's `on_funding_rate` callback by MdGatewayService;
    /// adapter threads call publish() directly off their IO thread.
    std::shared_ptr<api::FundingRatePublisher> funding_sink;

    /// \brief Outbound: per-instrument stats updates on stream 2004.
    ///
    /// Wired into each adapter's `on_instrument_stats` callback by MdGatewayService.
    /// Slow-cadence (updates every few seconds) — kept off the BBO firehose so
    /// strategy consumers that don't need OI don't pay decode cost on every tick.
    std::shared_ptr<api::InstrumentStatsPublisher> stats_sink;

    /// \brief Construct the prod (Aeron-backed) implementations of all five ports.
    ///
    /// Reads channel + stream-id assignments from `settings.aeron`. The
    /// supplied `aeron` shared client must already have a MediaDriver
    /// connection — see bpt::app::run() which sets it up.
    static AeronBus build(std::shared_ptr<::aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::md_gateway::messaging
