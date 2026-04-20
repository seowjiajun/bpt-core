#include "refdata/adapter/deribit/deribit_refdata_encoder.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace bpt::refdata::adapter::deribit {

std::string build_jsonrpc_body(uint64_t rpc_id,
                               const std::string& method,
                               const std::string& params_json) {
    json req_body;
    req_body["jsonrpc"] = "2.0";
    req_body["id"] = rpc_id;
    req_body["method"] = method;
    req_body["params"] = json::parse(params_json);
    return req_body.dump();
}

std::string build_get_instruments_params(const std::string& currency, const std::string& kind) {
    json params;
    params["currency"] = currency;
    params["kind"] = kind;
    params["expired"] = false;
    return params.dump();
}

}  // namespace bpt::refdata::adapter::deribit
