#include "bpt_common/secrets/secrets_client.h"

#include "bpt_common/logging.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace bpt::common::secrets {

std::map<std::string, std::string> fetch(const std::string& secret_name, Env env) {
    std::string name = secret_name;
    std::replace(name.begin(), name.end(), '/', '-');

    const bool strict = (env == Env::QA || env == Env::PROD);

    std::filesystem::path path;
    if (const char* dir = std::getenv("CREDENTIALS_DIRECTORY")) {
        path = std::filesystem::path(dir) / name;
    } else if (strict) {
        // QA + PROD refuse the dev fallback — a misconfigured unit that
        // forgets LoadCredentialEncrypted= must FAIL LOUDLY here, not
        // silently read a stale or attacker-planted file from HOME.
        throw std::runtime_error(
            "[secrets] env is qa|prod but CREDENTIALS_DIRECTORY is unset — "
            "systemd-creds delivery is required. Refusing the dev fallback.");
    } else if (const char* override_dir = std::getenv("BPT_DEV_SECRETS_DIR")) {
        // Explicit dev override — useful for per-checkout secret sets
        // or CI runs that don't want to read from $HOME.
        path = std::filesystem::path(override_dir) / name;
    } else if (const char* home = std::getenv("HOME")) {
        path = std::filesystem::path(home) / ".bpt-secrets" / name;
    } else {
        throw std::runtime_error(
            "[secrets] neither CREDENTIALS_DIRECTORY, BPT_DEV_SECRETS_DIR, "
            "nor HOME is set — no way to locate dev secrets.");
    }

    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("[secrets] cannot open " + path.string());
    }

    std::map<std::string, std::string> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }

    bpt::common::log::info("Loaded secret '{}' ({} keys)", name, out.size());
    return out;
}

}  // namespace bpt::common::secrets
