#pragma once

/// \file
/// \brief On-disk format definition for `.wslog` capture files.
///
/// Defines the record-type tag stamped on every entry written to the
/// tape. Producers (bpt-tape's `Tape` writer) and consumers
/// (bpt-backtester's WslogReader, future converters) both depend on
/// this header — the wire/disk format is the contract between them.
///
/// File format (little-endian):
///
///   struct RecordHeader {
///       uint64_t recv_ts_ns;   // wall-clock ns since Unix epoch
///       uint8_t  record_type;  // see RecordType
///       uint32_t length;       // bytes of payload that follow
///   };
///   uint8_t payload[length];
///
/// File layout: {root}/{venue_tag}/YYYY-MM-DD/{venue_tag}-HHMMSS.wslog
/// Hourly rotation by default.

#include <cstdint>

namespace bpt::common::recorder {

/// \brief Record-type tag stamped on every entry written to the tape.
enum class RecordType : uint8_t {
    WS_FRAME = 0,       ///< raw venue frame (JSON / FIX / etc.)
    SESSION_START = 1,  ///< recorder process started; payload = config snapshot JSON
    SESSION_STOP = 2,   ///< recorder process stopping cleanly; payload = exit reason JSON
    CHECKPOINT = 3,     ///< periodic heartbeat; payload = JSON {frames, bytes, uptime_s}
    WS_DISCONNECT = 4,  ///< unexpected WS connection loss; payload = JSON {reason, attempt}
    WS_RECONNECT = 5,   ///< WS reconnect succeeded after a prior disconnect; payload = JSON {attempt}
    /// REST response body captured from a refdata poll (bpt-tape only).
    /// Payload envelope (little-endian):
    ///   u8  method  (0=GET, 1=POST)
    ///   u16 target_len
    ///   char target[target_len]
    ///   char body[remainder]
    REST_RESPONSE = 6,
};

}  // namespace bpt::common::recorder
