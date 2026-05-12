#pragma once

/// \file
/// \brief Append-only tape — the recording medium written by bpt-tape.
///
/// Captures venue payloads (WS frames from md-gateway adapters, REST
/// response bodies from refdata adapters) in their native bytes. Replay
/// through the backtester / converter goes through the same parser code
/// as live, so any parser drift surfaces in test rather than production.
///
/// On-disk format lives in `bpt_common/recorder/wslog_format.h` — that
/// header is the contract shared with consumers (WslogReader, converters).
///
/// Thread model: callers own the Tape from a single thread (the IO thread
/// that produces the bytes); the Tape buffers in userspace and flushes to
/// fwrite on buffer-full or on flush(). NOT thread-safe — single writer.
///
/// Disk-on-hot-path note: write_frame() does an in-memory memcpy in the
/// common case. Buffer-full or rotation triggers a synchronous fwrite,
/// which under disk stall would backpressure the producer thread. For
/// prod-grade recording, wrap with a writer-thread ring buffer (TODO).

#include "bpt_common/recorder/wslog_format.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bpt::tape::io {

using ::bpt::common::recorder::RecordType;

/// \brief Single-writer append-only tape that emits the .wslog binary format.
///
/// Owned by the IO thread that produces bytes (one Tape per writer). NOT
/// thread-safe — counter accessors (frames_written / bytes_written) are
/// the only methods that may be called from another thread.
class Tape {
public:
    /// \brief Optional metrics callbacks. All fields default to no-op; if any
    /// is set, Tape calls it at the corresponding event. Using
    /// std::function (not concrete prometheus types) so this header doesn't
    /// pull in prometheus-cpp — wiring lives in the consumer (TapeMetrics).
    struct MetricsHooks {
        /// Called after a successful write_record(). recv_ts_ns is the
        /// payload timestamp (used to populate last-write gauges);
        /// total_bytes is the on-disk size of the record (header + payload).
        std::function<void(uint64_t recv_ts_ns, std::size_t total_bytes)> on_write_success;

        /// Called after a successful file rotation (new wslog opened).
        std::function<void()> on_rotation_success;

        /// Called when ensure_file_open() fails. cause is one of
        /// "create_directories" or "fopen" so the consumer can label the
        /// failure for the rotation_failures counter.
        std::function<void(std::string_view cause)> on_rotation_failure;
    };

    struct Config {
        std::string root_dir;   ///< e.g. "/opt/bpt/data/raw"
        std::string venue_tag;  ///< e.g. "okx" — used in path + audit log
        uint32_t rotate_interval_seconds{3600};
        uint32_t buffer_bytes{1u << 20};  ///< 1 MiB userspace buffer
        /// Auto-flush cadence. write_record() flushes if more than this many
        /// wall-clock ns have elapsed since the last flush — bounds replay-
        /// loss on crash to this interval regardless of buffer fill rate.
        uint64_t flush_interval_ns{1'000'000'000ULL};
        /// Optional. All hooks are no-op if unset.
        MetricsHooks metrics{};
    };

    explicit Tape(Config cfg);
    ~Tape();

    Tape(const Tape&) = delete;
    Tape& operator=(const Tape&) = delete;

    /// \brief Append a raw venue frame.
    /// \return false on file open / rotation / write failure (rare; logs error).
    bool write_frame(uint64_t recv_ts_ns, std::string_view payload);

    /// \brief Append a structured marker (SESSION_START/STOP/CHECKPOINT/etc).
    ///
    /// The payload is opaque to the Tape — caller is responsible for any
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

}  // namespace bpt::tape::io
