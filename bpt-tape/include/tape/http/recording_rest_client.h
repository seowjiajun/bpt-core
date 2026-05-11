#pragma once

/// \file
/// \brief Recording REST client — tees each response body into the tape.
///
/// Overrides RestClient::get/post to call the parent transport, then
/// tees the response into a Tape tagged REST_RESPONSE before
/// returning the body unchanged. The bpt-refdata service uses the
/// base class only; recording lives entirely in bpt-tape (same split
/// as the mdgw recording adapters).
///
/// Thread model: one client per poll thread. Tape is single-writer,
/// so every client wired to a given tape MUST be driven by the same
/// thread — bpt-tape's RefdataPoller enforces one std::thread per venue.
///
/// On-disk envelope (little-endian, inside the Tape record):
///
///     uint8_t  method        // 0=GET, 1=POST
///     uint16_t target_len
///     char     target[target_len]
///     char     body[remainder]
///
/// `remainder` is derived from the outer Tape record length minus
/// the header bytes — no second length field needed. Keeps the format
/// binary-safe (no JSON escaping of binary response bodies).

#include "tape/io/tape.h"
#include "bpt_common/util/tsc_clock.h"
#include "refdata/http/rest_client.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace bpt::tape::http {

/// \brief RestClient that tees each response into a Tape.
class RecordingRestClient : public ::bpt::refdata::http::RestClient {
public:
    /// \param host     remote host, e.g. "api.hyperliquid.xyz"
    /// \param port     remote port (as string — Boost.Asio resolver shape)
    /// \param use_tls  true → https, false → http
    /// \param tape    where to tee response bodies; nullptr disables recording
    RecordingRestClient(std::string host,
                        std::string port,
                        bool use_tls,
                        std::shared_ptr<::bpt::tape::io::Tape> tape)
        : RestClient(std::move(host), std::move(port), use_tls),
          tape_(std::move(tape)) {}

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
    /// Build the on-disk envelope and write it as one Tape record.
    /// Aborts on tape failure so systemd recycles us — silent drops
    /// were the failure mode this guards against.
    void record_response(uint8_t method_byte,
                         std::string_view target,
                         std::string_view body) const {
        if (!tape_) return;
        const uint16_t target_len = static_cast<uint16_t>(target.size());
        std::string buf;
        buf.reserve(sizeof(method_byte) + sizeof(target_len) + target.size() + body.size());
        buf.push_back(static_cast<char>(method_byte));
        buf.push_back(static_cast<char>(target_len & 0xff));
        buf.push_back(static_cast<char>((target_len >> 8) & 0xff));
        buf.append(target);
        buf.append(body);
        if (!tape_->write_marker(::bpt::common::util::WallClock::now_ns(),
                                  ::bpt::common::recorder::RecordType::REST_RESPONSE,
                                  buf)) {
            std::fputs("[FATAL] bpt-tape: Tape::write_marker (REST) failed; "
                       "aborting (Restart=always recycles).\n", stderr);
            std::fflush(stderr);
            std::abort();
        }
        // Force flush — Tape's auto-flush only fires on the NEXT
        // write_record call, and an hourly REST poller would otherwise
        // leave each response sitting unflushed for an entire interval.
        // mdgw doesn't need this since its tick stream re-enters
        // write_record continuously.
        tape_->flush();
    }

    std::shared_ptr<::bpt::tape::io::Tape> tape_;
};

}  // namespace bpt::tape::http
