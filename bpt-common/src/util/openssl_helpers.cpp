#include "bpt_common/util/openssl_helpers.h"

#include <cstdio>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace bpt::common::util {

std::vector<uint8_t> hmac_sha256(std::string_view key, std::string_view data) {
    std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         static_cast<int>(data.size()),
         digest.data(),
         &digest_len);
    digest.resize(digest_len);
    return digest;
}

std::string base64_encode(const uint8_t* data, std::size_t len) {
    // EVP_EncodeBlock writes `((len + 2) / 3) * 4` bytes + a NUL terminator
    // (which we then drop). Single-line output — no embedded newlines, which
    // is what every exchange auth flow expects.
    std::string out;
    out.resize(((len + 2) / 3) * 4);
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::string hex_encode(const uint8_t* data, std::size_t len) {
    std::string out(len * 2, '\0');
    for (std::size_t i = 0; i < len; ++i)
        std::snprintf(out.data() + i * 2, 3, "%02x", data[i]);
    return out;
}

}  // namespace bpt::common::util
