#pragma once

// Fetches and owns the OKX instrument metadata tables required for
// order submission:
//
//   - instIdCode: numeric id required by the WS trading endpoint's
//     `order` op. Not required by REST but the WS path enforces it.
//   - ctVal:      contract multiplier. For SWAP/FUTURES this converts
//                 fenrir's base-currency qty into OKX's contract
//                 count (sz = qty_base / ctVal). SPOT/MARGIN fall
//                 through to 1.0 so the same pipeline handles both.
//
// Fetched from /api/v5/public/instruments at adapter startup. The
// service swallows per-instType failures and logs a warning so a
// single bad inst_type (e.g. OPTION unavailable in demo) doesn't
// take down the rest of the adapter bring-up.

#include "order_gateway/adapter/okx/okx_action_codec.h"
#include "order_gateway/adapter/okx/okx_https_client.h"

namespace bpt::order_gateway::adapter::okx {

class OKXInstrumentsService {
public:
    explicit OKXInstrumentsService(OKXHttpsClient& client);

    // Populates inst_id_codes_ and contract_sizes_ from REST.
    // Idempotent — calling again replaces prior contents.
    void fetch();

    [[nodiscard]] const InstIdCodeMap& inst_id_codes() const { return inst_id_codes_; }
    [[nodiscard]] const ContractSizes& contract_sizes() const { return contract_sizes_; }

private:
    OKXHttpsClient& client_;
    InstIdCodeMap inst_id_codes_;
    ContractSizes contract_sizes_;
};

}  // namespace bpt::order_gateway::adapter::okx
