#pragma once

// Deribit refdata JSON-RPC envelope builders — pure input → body
// string, no state, no I/O. Counterpart to deribit_parser (wire →
// internal). The adapter composes these with rest_client.post() to
// talk to /api/v2.

#include <cstdint>
#include <string>

namespace bpt::refdata::adapter::deribit {

// Build a JSON-RPC 2.0 request body for `method` + `params`. params is
// the already-serialised JSON object/array for the params slot (e.g.
// the output of json::dump() at the caller).
[[nodiscard]] std::string build_jsonrpc_body(uint64_t rpc_id,
                                              const std::string& method,
                                              const std::string& params_json);

// Build the params JSON for public/get_instruments. `kind` is one of
// "future" / "option" / "spot". Bundled here (rather than built at the
// call site) so a future API change is a one-file edit.
[[nodiscard]] std::string build_get_instruments_params(const std::string& currency,
                                                        const std::string& kind);

}  // namespace bpt::refdata::adapter::deribit
