#pragma once

/// @file
/// Raw WS-frame spool. Tees every frame received by an md-gateway venue
/// adapter to disk in its native bytes — JSON for OKX/Binance/HL, FIX for
/// Deribit if/when we add it. Replay through bpt-backtester's exchange
/// simulators preserves bit-exact wire-level fidelity, surfacing parser
/// bugs the same way the live trading path would.
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
/// Hourly rotation by default. One spool per adapter — no contention.
///
/// Thread model: callers (the adapter IO thread) call write_frame()
/// directly; the spool buffers in userspace and flushes to fwrite on
/// buffer-full or on flush(). NOT thread-safe — single writer per spool.
///
/// Disk-on-hot-path note: write_frame() does an in-memory memcpy in the
/// common case. Buffer-full or rotation triggers a synchronous fwrite,
/// which under disk stall would backpressure the WS reader. For prod-grade
/// recording, wrap with a writer-thread ring buffer (TODO).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bpt::md_gateway::recorder {

enum class RecordType : uint8_t {
    WS_FRAME      = 0,  // raw venue frame (JSON / FIX / etc.)
    SESSION_START = 1,  // recorder process started; payload = config snapshot JSON
    SESSION_STOP  = 2,  // recorder process stopping cleanly; payload = exit reason JSON
    CHECKPOINT    = 3,  // periodic heartbeat; payload = JSON {frames, bytes, uptime_s}
    WS_DISCONNECT = 4,  // unexpected WS connection loss; payload = JSON {reason, attempt}
    WS_RECONNECT  = 5,  // WS reconnect succeeded after a prior disconnect; payload = JSON {attempt}
};

class RawSpool {
public:
    struct Config {
        std::string root_dir;        // e.g. /opt/bpt/data/raw
        std::string venue_tag;       // e.g. "okx" — used for path + audit log
        uint32_t rotate_interval_seconds{3600};
        uint32_t buffer_bytes{1u << 20};  // 1 MiB userspace buffer
        // Auto-flush cadence. write_record() flushes if more than this many
        // wall-clock ns have elapsed since the last flush. Bounds replay-loss
        // on crash to ≤ this interval, regardless of on_tick() availability.
        uint64_t flush_interval_ns{1'000'000'000ULL};  // 1s default
    };

    explicit RawSpool(Config cfg);
    ~RawSpool();

    RawSpool(const RawSpool&) = delete;
    RawSpool& operator=(const RawSpool&) = delete;

    // Append a raw venue frame.
    bool write_frame(uint64_t recv_ts_ns, std::string_view payload);

    // Append a structured marker (SESSION_START / STOP / CHECKPOINT).
    // The payload is opaque to the spool — caller passes JSON bytes.
    bool write_marker(uint64_t recv_ts_ns, RecordType type, std::string_view payload);

    // Force flush userspace buffer → stdio buffer → kernel.
    void flush();

    // Counters for metrics / log lines. Atomic so a heartbeat callback
    // running on a different thread can read them safely.
    [[nodiscard]] uint64_t frames_written() const noexcept { return frames_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& current_path() const noexcept { return current_path_; }

private:
    bool ensure_file_open(uint64_t recv_ts_ns);
    void close_current(uint64_t recv_ts_ns);
    bool write_record(uint64_t recv_ts_ns, RecordType type, std::string_view payload);
    std::string build_path(uint64_t recv_ts_ns) const;

    Config cfg_;
    std::FILE* fp_{nullptr};
    std::string current_path_;
    uint64_t current_file_start_ns_{0};
    uint64_t last_flush_ns_{0};

    std::vector<uint8_t> buffer_;
    std::size_t buffer_pos_{0};

    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> bytes_written_{0};
};

}  // namespace bpt::md_gateway::recorder
