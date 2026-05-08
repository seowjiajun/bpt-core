#pragma once

/// \file
/// \brief Append-only raw-frame spool used by the recording host.
///
/// Captures venue payloads (WS frames from md-gateway adapters, REST
/// response bodies from refdata adapters) in their native bytes. Replay
/// through the backtester / converter goes through the same parser code
/// as live, so any parser drift surfaces in test rather than production.
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
/// Hourly rotation by default. One spool per writer thread — no contention.
///
/// Thread model: callers own the spool from a single thread (the IO thread
/// that produces the bytes); the spool buffers in userspace and flushes to
/// fwrite on buffer-full or on flush(). NOT thread-safe — single writer.
///
/// Disk-on-hot-path note: write_frame() does an in-memory memcpy in the
/// common case. Buffer-full or rotation triggers a synchronous fwrite,
/// which under disk stall would backpressure the producer thread. For
/// prod-grade recording, wrap with a writer-thread ring buffer (TODO).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bpt::common::recorder {

/// \brief Record-type tag stamped on every entry written to the spool.
enum class RecordType : uint8_t {
    WS_FRAME      = 0,  ///< raw venue frame (JSON / FIX / etc.)
    SESSION_START = 1,  ///< recorder process started; payload = config snapshot JSON
    SESSION_STOP  = 2,  ///< recorder process stopping cleanly; payload = exit reason JSON
    CHECKPOINT    = 3,  ///< periodic heartbeat; payload = JSON {frames, bytes, uptime_s}
    WS_DISCONNECT = 4,  ///< unexpected WS connection loss; payload = JSON {reason, attempt}
    WS_RECONNECT  = 5,  ///< WS reconnect succeeded after a prior disconnect; payload = JSON {attempt}
    /// REST response body captured from a refdata poll (bpt-tape only).
    /// Payload envelope (little-endian):
    ///   u8  method  (0=GET, 1=POST)
    ///   u16 target_len
    ///   char target[target_len]
    ///   char body[remainder]
    REST_RESPONSE = 6,
};

/// \brief Single-writer append-only spool that emits the .wslog binary format.
///
/// Owned by the IO thread that produces bytes (one spool per writer). NOT
/// thread-safe — counter accessors (frames_written / bytes_written) are
/// the only methods that may be called from another thread.
class RawSpool {
public:
    struct Config {
        std::string root_dir;                  ///< e.g. "/opt/bpt/data/raw"
        std::string venue_tag;                 ///< e.g. "okx" — used in path + audit log
        uint32_t rotate_interval_seconds{3600};
        uint32_t buffer_bytes{1u << 20};       ///< 1 MiB userspace buffer
        /// Auto-flush cadence. write_record() flushes if more than this many
        /// wall-clock ns have elapsed since the last flush — bounds replay-
        /// loss on crash to this interval regardless of buffer fill rate.
        uint64_t flush_interval_ns{1'000'000'000ULL};
    };

    explicit RawSpool(Config cfg);
    ~RawSpool();

    RawSpool(const RawSpool&) = delete;
    RawSpool& operator=(const RawSpool&) = delete;

    /// \brief Append a raw venue frame.
    /// \return false on file open / rotation / write failure (rare; logs error).
    bool write_frame(uint64_t recv_ts_ns, std::string_view payload);

    /// \brief Append a structured marker (SESSION_START/STOP/CHECKPOINT/etc).
    ///
    /// The payload is opaque to the spool — caller is responsible for any
    /// structure (typically JSON). Used for non-frame events recorded
    /// alongside the data stream.
    bool write_marker(uint64_t recv_ts_ns, RecordType type, std::string_view payload);

    /// \brief Force-flush userspace buffer → stdio buffer → kernel.
    void flush();

    /// \name Counters for metrics / log lines.
    ///
    /// Atomic so a heartbeat callback running on a different thread can
    /// read them safely without the writer-side lock.
    /// \{
    [[nodiscard]] uint64_t frames_written() const noexcept { return frames_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& current_path() const noexcept { return current_path_; }
    /// \}

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

}  // namespace bpt::common::recorder
