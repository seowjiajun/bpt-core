#pragma once

namespace bpt::pricer::messaging {

// Pricer → Strategy
constexpr int VOL_SURFACE_STREAM_ID = 4001;   // VolSurface messages
constexpr int SURTR_STATUS_STREAM_ID = 4002;  // PricerHeartbeat + PricerReady

}  // namespace bpt::pricer::messaging
