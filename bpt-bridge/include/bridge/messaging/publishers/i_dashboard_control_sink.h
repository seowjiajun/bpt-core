#pragma once

/// @file
/// Port interface for the bridge → strategy control publication.
/// BridgeService depends on this rather than the concrete
/// DashboardControlPublisher so the dashboard HALT/RESUME plumbing can be
/// exercised without an Aeron driver in tests.
///
/// Wire format: single-byte command. 0x00 = HALT, 0x01 = RESUME.

namespace bpt::bridge::messaging {

class IDashboardControlSink {
public:
    virtual ~IDashboardControlSink() = default;

    /// Publish a HALT command (byte 0x00) to the control stream. Strategy
    /// stops generating new orders on receipt.
    virtual void publish_halt() = 0;

    /// Publish a RESUME command (byte 0x01) to the control stream. Strategy
    /// resumes new-order generation on receipt.
    virtual void publish_resume() = 0;
};

}  // namespace bpt::bridge::messaging
