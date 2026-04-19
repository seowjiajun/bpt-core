#include "bpt_common/secrets/secrets_client.h"

#include "bpt_common/logging.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace bpt::common::secrets {

namespace {
// BPT_ENV declares which tier this process believes it's running in:
//   "dev"  — local developer laptop. Dev fallback (~/.bpt-secrets/) allowed.
//   "qa"   — QA host. systemd-creds required; dev fallback REFUSED. QA's
//            job is to mimic prod, so a missing LoadCredentialEncrypted=
//            must fail here, not slip through to prod.
//   "prod" — production / real money. Same as qa — strict.
//
// Logged once on first fetch so operators can confirm after a deploy
// that the process sees the environment it's supposed to. Unknown
// values throw — silent fallback to a default environment is exactly
// how identity leaks happen.
bool resolve_strict_mode() {
    static const bool strict = [] {
        const char* raw = std::getenv("BPT_ENV");
        const std::string value = raw ? raw : "";
        bool is_strict;
        if (value == "dev") {
            is_strict = false;
        } else if (value == "qa" || value == "prod") {
            is_strict = true;
        } else {
            throw std::runtime_error(
                "[secrets] BPT_ENV has unrecognized value '" + value +
                "' (expected dev | qa | prod). Every process must declare "
                "its environment identity — refusing to guess.");
        }
        bpt::common::log::info("[secrets] BPT_ENV='{}' strict={}", value, is_strict);
        return is_strict;
    }();
    return strict;
}
}  // namespace

std::map<std::string, std::string> fetch(const std::string& secret_name) {
    std::string name = secret_name;
    std::replace(name.begin(), name.end(), '/', '-');

    const bool strict = resolve_strict_mode();

    std::filesystem::path path;
    if (const char* dir = std::getenv("CREDENTIALS_DIRECTORY")) {
        path = std::filesystem::path(dir) / name;
    } else if (strict) {
        // Strict tiers (qa, prod) refuse the ~/.bpt-secrets fallback —
        // a misconfigured unit that forgets LoadCredentialEncrypted= must
        // FAIL LOUDLY here, not silently read a stale or attacker-planted
        // file from HOME.
        throw std::runtime_error(
            "[secrets] strict mode (BPT_ENV=qa|prod) but CREDENTIALS_DIRECTORY "
            "is unset — systemd-creds delivery is required. Refusing the dev "
            "fallback.");
    } else if (const char* home = std::getenv("HOME")) {
        path = std::filesystem::path(home) / ".bpt-secrets" / name;
    } else {
        throw std::runtime_error(
            "[secrets] neither CREDENTIALS_DIRECTORY nor HOME is set — "
            "is this running under systemd with LoadCredentialEncrypted=?");
    }

    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("[secrets] cannot open " + path.string());
    }

    std::map<std::string, std::string> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }

    bpt::common::log::info("[secrets] Loaded '{}' ({} keys)", name, out.size());
    return out;
}

}  // namespace bpt::common::secrets
