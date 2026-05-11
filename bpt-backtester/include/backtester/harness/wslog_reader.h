#pragma once

/// @file
/// WslogReader — minimal sequential reader for the .wslog binary
/// format owned by `bpt::common::recorder::Tape`. Each record is
/// a fixed 13-byte header (recv_ts_ns u64 | record_type u8 | length u32)
/// followed by `length` payload bytes.
///
/// Used by the deterministic backtest harness to consume captures
/// produced by bpt-tape without depending on Arrow/Parquet (which is
/// what keeps DataLoader on CMake). Reader is single-threaded,
/// synchronous, no buffering beyond what stdio provides.

#include "bpt_common/recorder/wslog_format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace bpt::backtester::harness {

struct WslogRecord {
    uint64_t                              ts_ns;
    bpt::common::recorder::RecordType     type;
    std::vector<uint8_t>                  payload;
};

class WslogReader {
public:
    explicit WslogReader(const std::string& path) {
        fp_ = std::fopen(path.c_str(), "rb");
    }
    ~WslogReader() {
        if (fp_) std::fclose(fp_);
    }
    WslogReader(const WslogReader&) = delete;
    WslogReader& operator=(const WslogReader&) = delete;

    [[nodiscard]] bool ok() const { return fp_ != nullptr; }

    /// Read the next record. Returns std::nullopt at EOF or on a
    /// truncated read. The buffer is owned by the returned record.
    std::optional<WslogRecord> next() {
        if (!fp_) return std::nullopt;

        WslogRecord rec;
        uint8_t type_byte;
        uint32_t length;

        if (std::fread(&rec.ts_ns, sizeof(rec.ts_ns), 1, fp_) != 1) return std::nullopt;
        if (std::fread(&type_byte, sizeof(type_byte), 1, fp_) != 1) return std::nullopt;
        if (std::fread(&length, sizeof(length), 1, fp_) != 1) return std::nullopt;

        rec.type = static_cast<bpt::common::recorder::RecordType>(type_byte);
        rec.payload.resize(length);
        if (length > 0 &&
            std::fread(rec.payload.data(), 1, length, fp_) != length) {
            return std::nullopt;
        }
        return rec;
    }

private:
    std::FILE* fp_{nullptr};
};

}  // namespace bpt::backtester::harness
