#include "refdata/mapping/instrument_mapping_s3_fetcher.h"

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sstream>
#include <yggdrasil/logging.h>

namespace bpt::refdata::mapping {

using json = nlohmann::json;

namespace {

std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return ss.str();
}

// Merge src into dst in-place.
// forward: union (src keys overwrite on collision — should never happen for different exchanges).
// reverse: entries are merged by unioning their exchanges dicts; base/quote/type from first seen.
void merge_into(json& dst, const json& src) {
    // forward
    if (src.contains("forward") && src["forward"].is_object()) {
        for (const auto& [key, val] : src["forward"].items())
            dst["forward"][key] = val;
    }

    // reverse
    if (src.contains("reverse") && src["reverse"].is_object()) {
        for (const auto& [cid, entry] : src["reverse"].items()) {
            if (!dst["reverse"].contains(cid)) {
                dst["reverse"][cid] = entry;
            } else {
                // Merge exchanges dict only — base/quote/type are identical by construction.
                if (entry.contains("exchanges") && entry["exchanges"].is_object()) {
                    for (const auto& [ex_id, sym] : entry["exchanges"].items())
                        dst["reverse"][cid]["exchanges"][ex_id] = sym;
                }
            }
        }
    }

    // exported_at: take the latest
    uint64_t src_ts = src.value("exported_at", uint64_t{0});
    uint64_t dst_ts = dst.value("exported_at", uint64_t{0});
    if (src_ts > dst_ts)
        dst["exported_at"] = src_ts;
}

}  // namespace

InstrumentMappingS3Fetcher::InstrumentMappingS3Fetcher(const Config& cfg) : cfg_(cfg) {}

bool InstrumentMappingS3Fetcher::fetch(const std::string& local_path) const {
    if (cfg_.keys.empty()) {
        ygg::log::error("[InstrumentMappingS3Fetcher] No S3 keys configured");
        return false;
    }

    Aws::Client::ClientConfiguration client_cfg;
    client_cfg.region = cfg_.region;
    client_cfg.connectTimeoutMs = 10000;  // 10 s connect timeout
    client_cfg.requestTimeoutMs = 30000;  // 30 s request timeout
    Aws::S3::S3Client client(client_cfg);

    json merged;
    merged["forward"] = json::object();
    merged["reverse"] = json::object();
    merged["exported_at"] = uint64_t{0};

    for (const auto& [exchange_name, s3_key] : cfg_.keys) {
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(cfg_.bucket);
        request.SetKey(s3_key);

        auto outcome = client.GetObject(request);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            ygg::log::error("[InstrumentMappingS3Fetcher] S3 GetObject failed for {}: {} — {}",
                            exchange_name,
                            err.GetExceptionName().c_str(),
                            err.GetMessage().c_str());
            return false;
        }

        auto& result = outcome.GetResult();

        std::ostringstream body_ss;
        body_ss << result.GetBody().rdbuf();
        const std::string body = body_ss.str();

        // Verify SHA256 if the uploader embedded it in object metadata.
        const auto& metadata = result.GetMetadata();
        auto it = metadata.find("sha256");
        if (it != metadata.end()) {
            const std::string expected = it->second.c_str();
            const std::string actual = sha256_hex(body);
            if (actual != expected) {
                ygg::log::error("[InstrumentMappingS3Fetcher] SHA256 mismatch for {}: expected={} actual={}",
                                exchange_name,
                                expected,
                                actual);
                return false;
            }
            ygg::log::debug("[InstrumentMappingS3Fetcher] SHA256 verified for {}: {}", exchange_name, actual);
        } else {
            ygg::log::warn("[InstrumentMappingS3Fetcher] No SHA256 metadata on s3://{}/{} — skipping integrity check",
                           cfg_.bucket,
                           s3_key);
        }

        json parsed;
        try {
            parsed = json::parse(body);
        } catch (const json::exception& e) {
            ygg::log::error("[InstrumentMappingS3Fetcher] JSON parse error for {}: {}", exchange_name, e.what());
            return false;
        }

        merge_into(merged, parsed);
        ygg::log::info("[InstrumentMappingS3Fetcher] Fetched s3://{}/{} ({} bytes)", cfg_.bucket, s3_key, body.size());
    }

    // Recompute instrument_count from merged reverse map size.
    merged["instrument_count"] = merged["reverse"].size();

    // Atomic write: serialise → .tmp → rename.
    const std::string serialised = merged.dump();
    const std::string tmp_path = local_path + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            ygg::log::error("[InstrumentMappingS3Fetcher] Cannot open tmp file for writing: {}", tmp_path);
            return false;
        }
        out.write(serialised.data(), static_cast<std::streamsize>(serialised.size()));
        if (!out) {
            ygg::log::error("[InstrumentMappingS3Fetcher] Write to tmp file failed: {}", tmp_path);
            return false;
        }
    }

    if (std::rename(tmp_path.c_str(), local_path.c_str()) != 0) {
        ygg::log::error("[InstrumentMappingS3Fetcher] Atomic rename failed: {} → {}", tmp_path, local_path);
        return false;
    }

    ygg::log::info("[InstrumentMappingS3Fetcher] Merged {} exchange(s) → {} ({} instruments, {} bytes)",
                   cfg_.keys.size(),
                   local_path,
                   merged["reverse"].size(),
                   serialised.size());
    return true;
}

}  // namespace bpt::refdata::mapping
