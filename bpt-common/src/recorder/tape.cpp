#include "bpt_common/recorder/tape.h"

#include "bpt_common/logging.h"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace bpt::common::recorder {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kHeaderBytes = sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t);

uint64_t wall_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

Tape::Tape(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.buffer_bytes == 0)
        cfg_.buffer_bytes = 1u << 20;
    if (cfg_.rotate_interval_seconds == 0)
        cfg_.rotate_interval_seconds = 3600;
    buffer_.resize(cfg_.buffer_bytes);
}

Tape::~Tape() {
    // Best-effort SESSION_STOP marker. Caller should also call write_marker
    // explicitly with a structured exit reason; this is just the safety net.
    if (fp_ != nullptr) {
        const std::string payload = R"({"reason":"destructor"})";
        write_record(wall_now_ns(), RecordType::SESSION_STOP, payload);
        close_current(wall_now_ns());
    }
}

std::string Tape::build_path(uint64_t recv_ts_ns) const {
    const std::time_t t = static_cast<std::time_t>(recv_ts_ns / 1'000'000'000ULL);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return fmt::format(
        "{}/{}/{:04}-{:02}-{:02}/{}-{:02}{:02}{:02}.wslog",
        cfg_.root_dir, cfg_.venue_tag,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        cfg_.venue_tag,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}

bool Tape::ensure_file_open(uint64_t recv_ts_ns) {
    const uint64_t rotate_ns =
        static_cast<uint64_t>(cfg_.rotate_interval_seconds) * 1'000'000'000ULL;
    const bool need_rotate =
        (fp_ == nullptr) || (recv_ts_ns - current_file_start_ns_ >= rotate_ns);
    if (!need_rotate)
        return true;

    if (fp_ != nullptr)
        close_current(recv_ts_ns);

    const std::string path = build_path(recv_ts_ns);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        // Surface the cause: ENOSPC, EACCES, EROFS all look identical at
        // the bool-return seam. The 2026-05-09 incident burned 36 hours
        // because this path was silent. See docs/backlog.md.
        bpt::common::log::error(
            "Tape: create_directories({}) failed: {} ({})",
            fs::path(path).parent_path().string(),
            ec.message(), ec.value());
        if (cfg_.metrics.on_rotation_failure)
            cfg_.metrics.on_rotation_failure("create_directories");
        return false;
    }

    fp_ = std::fopen(path.c_str(), "ab");
    if (fp_ == nullptr) {
        bpt::common::log::error(
            "Tape: fopen({}, \"ab\") failed: {} (errno={})",
            path, std::strerror(errno), errno);
        if (cfg_.metrics.on_rotation_failure)
            cfg_.metrics.on_rotation_failure("fopen");
        return false;
    }

    current_path_ = path;
    current_file_start_ns_ = recv_ts_ns;
    if (cfg_.metrics.on_rotation_success)
        cfg_.metrics.on_rotation_success();
    return true;
}

void Tape::close_current(uint64_t /*recv_ts_ns*/) {
    if (fp_ == nullptr)
        return;
    if (buffer_pos_ > 0) {
        std::fwrite(buffer_.data(), 1, buffer_pos_, fp_);
        buffer_pos_ = 0;
    }
    std::fflush(fp_);
    std::fclose(fp_);
    fp_ = nullptr;
    current_path_.clear();
}

bool Tape::write_record(uint64_t recv_ts_ns, RecordType type, std::string_view payload) {
    if (!ensure_file_open(recv_ts_ns))
        return false;

    const uint8_t type_byte = static_cast<uint8_t>(type);
    const uint32_t length = static_cast<uint32_t>(payload.size());
    const std::size_t total = kHeaderBytes + length;

    // Flush userspace buffer if this record won't fit.
    if (buffer_pos_ + total > buffer_.size()) {
        if (buffer_pos_ > 0) {
            if (std::fwrite(buffer_.data(), 1, buffer_pos_, fp_) != buffer_pos_) {
                bpt::common::log::error(
                    "Tape: fwrite(buffer={} B) to {} failed: {} (errno={})",
                    buffer_pos_, current_path_, std::strerror(errno), errno);
                return false;
            }
            buffer_pos_ = 0;
        }
        // Pathological-size record: write direct.
        if (total > buffer_.size()) {
            const auto fail = [&](const char* what) {
                bpt::common::log::error(
                    "Tape: fwrite({} direct, {} B) to {} failed: {} (errno={})",
                    what, total, current_path_, std::strerror(errno), errno);
                return false;
            };
            if (std::fwrite(&recv_ts_ns, sizeof(recv_ts_ns), 1, fp_) != 1) return fail("ts");
            if (std::fwrite(&type_byte, sizeof(type_byte), 1, fp_) != 1)   return fail("type");
            if (std::fwrite(&length,    sizeof(length),    1, fp_) != 1)   return fail("len");
            if (length > 0 &&
                std::fwrite(payload.data(), 1, length, fp_) != length)     return fail("payload");
            frames_written_.fetch_add(1, std::memory_order_relaxed);
            bytes_written_.fetch_add(total, std::memory_order_relaxed);
            return true;
        }
    }

    std::memcpy(buffer_.data() + buffer_pos_, &recv_ts_ns, sizeof(recv_ts_ns));
    buffer_pos_ += sizeof(recv_ts_ns);
    std::memcpy(buffer_.data() + buffer_pos_, &type_byte, sizeof(type_byte));
    buffer_pos_ += sizeof(type_byte);
    std::memcpy(buffer_.data() + buffer_pos_, &length, sizeof(length));
    buffer_pos_ += sizeof(length);
    if (length > 0) {
        std::memcpy(buffer_.data() + buffer_pos_, payload.data(), length);
        buffer_pos_ += length;
    }

    frames_written_.fetch_add(1, std::memory_order_relaxed);
    bytes_written_.fetch_add(total, std::memory_order_relaxed);

    // Auto-flush if it's been a while since the last one. Caps replay-loss
    // on crash to ~flush_interval_ns regardless of buffer fill rate.
    if (cfg_.flush_interval_ns > 0 &&
        recv_ts_ns - last_flush_ns_ >= cfg_.flush_interval_ns) {
        if (buffer_pos_ > 0) {
            std::fwrite(buffer_.data(), 1, buffer_pos_, fp_);
            buffer_pos_ = 0;
        }
        std::fflush(fp_);
        last_flush_ns_ = recv_ts_ns;
    }
    if (cfg_.metrics.on_write_success)
        cfg_.metrics.on_write_success(recv_ts_ns, total);
    return true;
}

bool Tape::write_frame(uint64_t recv_ts_ns, std::string_view payload) {
    return write_record(recv_ts_ns, RecordType::WS_FRAME, payload);
}

bool Tape::write_marker(uint64_t recv_ts_ns, RecordType type, std::string_view payload) {
    return write_record(recv_ts_ns, type, payload);
}

void Tape::flush() {
    if (fp_ == nullptr)
        return;
    if (buffer_pos_ > 0) {
        std::fwrite(buffer_.data(), 1, buffer_pos_, fp_);
        buffer_pos_ = 0;
    }
    std::fflush(fp_);
    last_flush_ns_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace bpt::common::recorder
