#pragma once

#include <map>
#include <string>

namespace bpt::refdata::mapping {

// Fetches per-exchange instrument mapping files from AWS S3, merges them in
// memory, and writes the merged result atomically to a local path.
//
// Credentials are resolved automatically by the AWS SDK in order:
//   1. Environment variables (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY)
//   2. EC2 instance profile / ECS task role (IMDS)
//   3. ~/.aws/credentials
//
// Aws::InitAPI() must be called once before constructing this class.
class InstrumentMappingS3Fetcher {
public:
    struct Config {
        std::string bucket;
        std::string region;
        std::map<std::string, std::string> keys;  // exchange_name → s3_key
    };

    explicit InstrumentMappingS3Fetcher(const Config& cfg);

    // Downloads all configured per-exchange files from S3, merges them, and
    // writes the merged mapping atomically to local_path (via .tmp + rename).
    // Returns true on success, false on any error (logs details).
    [[nodiscard]] bool fetch(const std::string& local_path) const;

private:
    Config cfg_;
};

}  // namespace bpt::refdata::mapping
