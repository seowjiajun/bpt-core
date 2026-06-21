#pragma once

/// \file
/// \brief CanonWriter — single-writer append for `.canon` derived event files.
///
/// Owned by the producer thread that converts raw bytes (replay of
/// `.wslog`, OKX archive ingest, etc.) into canonical SBE events. Same
/// shape as `bpt::tape::io::Tape`: userspace buffer + stdio + atomic
/// counters. NOT thread-safe — single writer per file.
///
/// File-header constraint: every `.canon` file starts with a 96-byte
/// `CanonFileHeader`. The writer emits that header on `open()` and from
/// then on appends only records. There's no rotation — canon is derived
/// data, the producer decides shard granularity by opening separate
/// CanonWriters for separate shards.

#include "canon/canon_format.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bpt::canon {

class CanonWriter {
public:
    struct Config {
        std::string path;                    ///< absolute output path (dirs created on open)
        std::string producer_kind;           ///< populated into FileHeader.producer_kind (truncated to 16)
        std::string producer_sha;            ///< populated into FileHeader.producer_sha (truncated to 40)
        uint8_t venue_id{0};                 ///< messages::ExchangeId enum
        uint32_t date_utc{0};                ///< YYYYMMDD packed
        std::size_t buffer_bytes{1u << 20};  ///< 1 MiB userspace buffer
    };

    explicit CanonWriter(Config cfg);
    ~CanonWriter();

    CanonWriter(const CanonWriter&) = delete;
    CanonWriter& operator=(const CanonWriter&) = delete;

    /// \brief Open the file and emit the canon file header.
    /// \return false on directory-create / fopen / header-write failure
    ///         (rare; logs error). Subsequent write_event() calls return
    ///         false until open() succeeds.
    bool open();

    /// \brief Append one event.
    /// \param ts_ns wall-clock ns since Unix epoch (must be monotonic
    ///              within a file; readers don't enforce, but downstream
    ///              consumers assume).
    /// \param type  event-type tag.
    /// \param sbe   SBE-encoded payload bytes.
    /// \return false on a failed flush — same semantics as
    ///         `bpt::tape::io::Tape::write_record`.
    bool write_event(uint64_t ts_ns, EventType type, std::string_view sbe);

    /// \brief Force userspace buffer → stdio → kernel.
    void flush();

    /// \brief Close the file (also called automatically by the destructor).
    void close();

    /// \name Counters for metrics / log lines.
    /// Atomic so a reporting thread can read them without synchronising
    /// with the writer.
    /// \{
    [[nodiscard]] uint64_t events_written() const noexcept { return events_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& path() const noexcept { return cfg_.path; }
    /// \}

private:
    bool write_file_header();
    bool flush_buffer_to_fp();

    Config cfg_;
    std::FILE* fp_{nullptr};

    std::vector<uint8_t> buffer_;
    std::size_t buffer_pos_{0};

    std::atomic<uint64_t> events_written_{0};
    std::atomic<uint64_t> bytes_written_{0};
};

}  // namespace bpt::canon
