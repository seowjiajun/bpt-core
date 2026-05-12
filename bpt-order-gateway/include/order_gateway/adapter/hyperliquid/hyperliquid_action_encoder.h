#pragma once

/// \file
/// \brief Hyperliquid action JSON builders — pure input → JSON, no I/O, no state, no mutex.
///
/// Each helper produces a boost::json::value in exactly the shape
/// Hyperliquid's `/exchange` endpoint (or the equivalent WS post action
/// payload) expects.
///
/// The returned action object is handed to the signer (which
/// msgpack-encodes it and hashes) and then to the transport layer
/// (which wraps it with nonce + signature and sends). Callers must NOT
/// mutate the action after signing — see HyperliquidSigner for the
/// exact bytes contract.

#include <boost/json/fwd.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bpt::order_gateway::adapter::hyperliquid {

/// \brief Format a double matching Hyperliquid's Python SDK `float_to_wire`.
///
/// Steps:
///   1. Format to 8 decimals (`%.8f`).
///   2. Strip trailing zeros after the decimal point.
///   3. Strip the decimal point if nothing follows.
///
/// Examples (must match Python's HL SDK exactly):
/// \code
///   72198.0575 → "72198.0575"
///   50000.0    → "50000"
///   0.001      → "0.001"
/// \endcode
///
/// HL's server normalises the wire string to this canonical form before
/// hashing, so our msgpack input MUST match byte-for-byte — otherwise
/// the server-computed hash diverges from ours and ECDSA recovers a
/// garbage address ("User or API Wallet does not exist"). This helper
/// is the load-bearing piece that keeps REST and WS order posting
/// working on HL testnet.
[[nodiscard]] std::string float_to_wire(double v);

/// \brief Per-asset metadata needed to build a well-formed order action.
///
/// Populated at adapter startup from HL's `/info` meta endpoint so
/// mainnet + testnet + any relisting are handled without a code change
/// — see `HyperliquidOrderAdapter::load_asset_meta()`.
struct AssetMeta {
    int asset_idx;        ///< HL universe index — what goes into action.orders[].a
    int sz_decimals;      ///< used to enforce price precision rules
    int max_px_decimals;  ///< 6 - sz_decimals; max decimal places allowed in the price string
};

/// \brief Map of coin symbol → AssetMeta.
///
/// Callers (adapter, tests) populate one instance (from `/info` meta in
/// production, a literal in tests) and pass individual AssetMeta entries
/// into the encoder.
using AssetTable = std::unordered_map<std::string, AssetMeta>;

/// \brief Parse HL's `/info` response body (returned by `POST {"type":"meta"}`) into an AssetTable.
///
/// Pure function for testability — no I/O, takes the raw response
/// string. Throws on malformed JSON or missing "universe" array.
/// Entries with non-string or missing `name` are silently skipped so
/// that a new HL field shape doesn't crash startup.
[[nodiscard]] AssetTable parse_universe_meta(std::string_view meta_response_body);

/// \brief Parse HL's `/info` response body (POST {"type":"spotMeta"}) into an AssetTable.
///
/// Entries are keyed by the HL spot pair `name` field — "PURR/USDC" for
/// canonical pairs, "@N" for user-deployed pairs. `asset_idx` is
/// `10000 + universe[i].index` per HL's spot order convention.
/// `sz_decimals` comes from the base token; `max_px_decimals = 8 -
/// sz_decimals` (HL spot's MAX_DECIMALS=8, vs perps' 6).
///
/// Throws on malformed JSON or missing tokens/universe arrays.
[[nodiscard]] AssetTable parse_spot_universe_meta(std::string_view spot_meta_response_body);

/// \brief HL time-in-force for the limit variant.
///
/// Alo = Add Liquidity Only (post-only) — HL rejects the order if it
/// would cross the book at submission time. Essential for market-making
/// strategies that must not pay the spread on fills.
enum class HlTif { Gtc, Alo, Ioc };

/// \brief Wire string HL expects in action.orders[].t.limit.tif.
[[nodiscard]] const char* tif_to_string(HlTif tif);

/// \brief Build the `action` JSON for a new order (one-leg limit).
///
/// Caller resolves coin symbol → AssetMeta via the AssetTable populated
/// at startup from `/info` meta. `price_natural` / `size_natural` are in
/// natural units (e.g. 72108.5 USD, 0.001 BTC). The builder rounds the
/// price to satisfy HL's "max 5 sig figs / max (6 - szDecimals)
/// decimals" rule using meta.sz_decimals.
[[nodiscard]] boost::json::value build_order_action(const AssetMeta& meta,
                                                    bool is_buy,
                                                    double price_natural,
                                                    double size_natural,
                                                    HlTif tif = HlTif::Gtc);

/// \brief Build the `action` JSON for a cancel-by-exchange-oid.
///
/// The oid is the HL exchange order id from the original `resting`
/// response — NOT our internal client order_id.
[[nodiscard]] boost::json::value build_cancel_action(const AssetMeta& meta, uint64_t exch_oid);

/// \brief Build the `action` JSON for a modify.
///
/// HL does not accept modify over the WS `post` endpoint — use REST for
/// this path. Builder is transport-agnostic, caller decides.
[[nodiscard]] boost::json::value build_modify_action(const AssetMeta& meta,
                                                     uint64_t client_or_exch_oid,
                                                     double price_natural,
                                                     double size_natural);

/// \brief Build the `action` JSON for a scheduleCancel dead-man switch.
///
/// `time_ms` is the unix-epoch millisecond deadline at which HL will
/// cancel all open orders across every coin for the signing wallet if
/// no further scheduleCancel arrives before it. Passing 0 clears any
/// pending schedule without setting a new one.
///
/// \warning Not currently used. HL gates scheduleCancel behind
///          $1,000,000 of lifetime traded volume — smaller accounts get
///          "Cannot set scheduled cancel time until enough volume
///          traded". The helper is kept for a future wallet tier that
///          clears the threshold; see the TODO in
///          hyperliquid_order_adapter.h for the current crash-safety
///          status. Constraints when it IS usable: `time_ms` must be
///          ≥5 seconds in the future, and HL caps scheduled-cancel
///          fires at 10 per UTC day.
[[nodiscard]] boost::json::value build_schedule_cancel_action(int64_t time_ms);

}  // namespace bpt::order_gateway::adapter::hyperliquid
