#include "order_gateway/adapter/okx/okx_instruments_service.h"

#include <boost/json.hpp>
#include <string>
#include <vector>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::adapter::okx {

namespace json = boost::json;

OKXInstrumentsService::OKXInstrumentsService(OKXHttpsClient& client) : client_(client) {}

void OKXInstrumentsService::fetch() {
    inst_id_codes_.clear();
    contract_sizes_.clear();

    const std::vector<std::string> inst_types = {"SPOT", "SWAP", "FUTURES", "MARGIN"};
    for (const auto& inst_type : inst_types) {
        try {
            std::string resp = client_.get_unsigned("/api/v5/public/instruments?instType=" + inst_type);
            auto root = json::parse(resp);
            if (!root.is_object())
                continue;
            auto data_it = root.as_object().find("data");
            if (data_it == root.as_object().end() || !data_it->value().is_array())
                continue;
            const bool is_contract_type = inst_type == "SWAP" || inst_type == "FUTURES" || inst_type == "OPTION";
            for (const auto& item : data_it->value().as_array()) {
                const auto& d = item.as_object();
                auto id_it = d.find("instId");
                auto code_it = d.find("instIdCode");
                if (id_it == d.end() || code_it == d.end())
                    continue;

                std::string inst_id = std::string(id_it->value().as_string());
                int64_t code = code_it->value().is_int64()
                                   ? code_it->value().as_int64()
                                   : std::stoll(std::string(code_it->value().as_string()));
                inst_id_codes_[inst_id] = code;

                // ctVal: base currency per contract for SWAP/FUTURES.
                // SPOT/MARGIN: sz is in base currency, treat as ctVal=1.
                double ctval = 1.0;
                if (is_contract_type) {
                    auto ctval_it = d.find("ctVal");
                    if (ctval_it != d.end() && ctval_it->value().is_string()) {
                        std::string sv = std::string(ctval_it->value().as_string());
                        if (!sv.empty())
                            ctval = std::stod(sv);
                    }
                }
                contract_sizes_[inst_id] = ctval;
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn("[OrderGateway] OKXInstrumentsService: fetch({}) failed: {}", inst_type, e.what());
        }
    }
    bpt::common::log::info("[OrderGateway] OKXInstrumentsService: loaded {} instIdCodes, {} contract sizes from REST",
                   inst_id_codes_.size(),
                   contract_sizes_.size());
}

}  // namespace bpt::order_gateway::adapter::okx
