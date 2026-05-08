#pragma once

/// @file
/// Recording subclass of bpt::refdata::http::RestClient. Overrides get/post
/// to call the parent transport, then tees the response body to a shared
/// RawSpool tagged REST_RESPONSE before returning the body unchanged to the
/// caller. The bpt-refdata service binary imports the base class only;
/// recording lives entirely in bpt-tape — same split as the mdgw side.
///
/// Thread model: one client per poll thread. RawSpool is single-writer, so
/// every client wired to a given spool MUST be driven by the same thread.
/// The bpt-tape RefdataPoller enforces this (one std::thread per venue).
///
/// Envelope written to the spool (little-endian, framed inside the RawSpool
/// record's `length` bytes):
///
///   uint8_t  method      // 0=GET, 1=POST
///   uint16_t target_len
///   char     target[target_len]
///   char     body[remainder]
///
/// `remainder` falls out of the outer RawSpool length minus the header
/// bytes — no second length field needed. Keeps the format binary-safe
/// (no JSON escaping of binary response bodies).

#include "bpt_common/recorder/raw_spool.h"
#include "refdata/http/rest_client.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace bpt::tape::http {

class RecordingRestClient : public ::bpt::refdata::http::RestClient {
public:
    RecordingRestClient(std::string host,
                        std::string port,
                        bool use_tls,
                        std::shared_ptr<::bpt::common::recorder::RawSpool> spool)
        : RestClient(std::move(host), std::move(port), use_tls),
          spool_(std::move(spool)) {}

    std::string get(const std::string& target,
                    const Headers& extra_headers = {}) const override {
        std::string body = RestClient::get(target, extra_headers);
        record_response(/*method_byte=*/0, target, body);
        return body;
    }

    std::string post(const std::string& target,
                     const std::string& req_body,
                     const Headers& extra_headers = {}) const override {
        std::string body = RestClient::post(target, req_body, extra_headers);
        record_response(/*method_byte=*/1, target, body);
        return body;
    }

private:
    static uint64_t wall_now_ns() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void record_response(uint8_t method_byte,
                         std::string_view target,
                         std::string_view body) const {
        if (!spool_) return;
        const uint16_t target_len = static_cast<uint16_t>(target.size());
        std::string buf;
        buf.reserve(sizeof(method_byte) + sizeof(target_len) + target.size() + body.size());
        buf.push_back(static_cast<char>(method_byte));
        buf.push_back(static_cast<char>(target_len & 0xff));
        buf.push_back(static_cast<char>((target_len >> 8) & 0xff));
        buf.append(target);
        buf.append(body);
        spool_->write_marker(wall_now_ns(),
                             ::bpt::common::recorder::RecordType::REST_RESPONSE,
                             buf);
        // Force the record to disk. RawSpool's auto-flush runs INSIDE
        // write_record and only fires when a subsequent record is written —
        // for an hourly REST poller, the buffer would otherwise sit unflushed
        // for an entire interval (mdgw doesn't have this problem because the
        // tick stream re-enters write_record continuously). Cost is one
        // fwrite + fflush per response, acceptable for a poll cadence.
        spool_->flush();
    }

    std::shared_ptr<::bpt::common::recorder::RawSpool> spool_;
};

}  // namespace bpt::tape::http
