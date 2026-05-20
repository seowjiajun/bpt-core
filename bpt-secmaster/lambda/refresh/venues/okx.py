"""
OKX instrument ingester.

Source: GET https://www.okx.com/api/v5/public/instruments?instType={...}
Docs:   https://www.okx.com/docs-v5/en/#public-data-rest-api-get-instruments

OKX returns one call per instType. We fetch SPOT, SWAP, FUTURES, OPTION
and yield normalized rows for each.

Caveats:
  - OKX's `state` field: 'live' | 'suspend' | 'preopen' | 'test' | 'expired'.
    We map 'live'/'preopen' → 'live' (preopen will trade soon),
    'suspend' → 'suspended', everything else → 'delisted'.
  - SWAP `ctType`: 'linear' or 'inverse'. We use this to disambiguate;
    settle_ccy alone isn't always reliable (some non-USDT-margin
    linears exist).
  - OPTION strikes: OKX returns strike as a string. Convert to Decimal
    via str→Decimal (NOT via float — float precision would alter the
    canonical_symbol).
"""

from __future__ import annotations

import logging
from decimal import Decimal
from typing import Iterable

import symbology
from db import NormalizedInstrument
from venues.base import VenueIngester

log = logging.getLogger(__name__)

OKX_REST_BASE = "https://www.okx.com"

# OKX requires `instFamily` (or per-instId) when querying instType=OPTION.
# The /api/v5/public/instruments endpoint 400s on a bare instType=OPTION
# call. Iterate the underlyings we care about; adding a new option
# underlying = adding a string here.
OKX_OPTION_FAMILIES = ("BTC-USD", "ETH-USD", "SOL-USD")


class OkxIngester(VenueIngester):
    exchange_code = "okx"

    def fetch(self) -> Iterable[NormalizedInstrument]:
        for inst_type in ("SPOT", "SWAP", "FUTURES"):
            yield from self._fetch_and_normalize(inst_type)
        # OPTION needs per-family fetches. Wrap individually — a family
        # that doesn't exist on OKX yet (e.g. newly-listed coin) raises
        # 400, which without isolation would yield-from-propagate and
        # abort the whole OKX venue run.
        for family in OKX_OPTION_FAMILIES:
            try:
                yield from self._fetch_and_normalize("OPTION", inst_family=family)
            except Exception as e:
                log.warning("OKX: OPTION family %s skipped: %s", family, e)

    def _fetch_and_normalize(
        self, inst_type: str, inst_family: str | None = None
    ) -> Iterable[NormalizedInstrument]:
        payload = self._fetch_inst_type(inst_type, inst_family=inst_family)
        for row in payload.get("data", []):
            try:
                norm = self._normalize(inst_type, row)
                if norm is not None:
                    yield norm
            except Exception as e:
                log.warning(
                    "OKX: skipping %s row instId=%s: %s",
                    inst_type,
                    row.get("instId"),
                    e,
                )

    # ─────────────────────────── HTTP fetch ──────────────────────────

    def _fetch_inst_type(self, inst_type: str, inst_family: str | None = None) -> dict:
        url = f"{OKX_REST_BASE}/api/v5/public/instruments"
        params: dict[str, str] = {"instType": inst_type}
        if inst_family:
            params["instFamily"] = inst_family
        r = self.http.get(url, params=params, timeout=30.0)
        r.raise_for_status()
        body = r.json()
        if body.get("code") != "0":
            raise RuntimeError(f"OKX returned code={body.get('code')} msg={body.get('msg')}")
        return body

    # ──────────────────────────── normalize ──────────────────────────

    def _normalize(self, inst_type: str, row: dict) -> NormalizedInstrument | None:
        instId = row["instId"]
        state = row.get("state", "")

        # Map OKX state → canonical status.
        if state in ("live", "preopen"):
            status = "live"
        elif state == "suspend":
            status = "suspended"
        else:  # 'expired', 'test', or anything unknown
            return None  # skip expired/test instruments entirely

        base = row["baseCcy"].upper() if row.get("baseCcy") else None
        quote = row["quoteCcy"].upper() if row.get("quoteCcy") else None
        settle = row.get("settleCcy", "").upper() or None

        # OKX leaves baseCcy/quoteCcy empty for SWAP/FUTURES — the base
        # and quote currencies have to be parsed out of `uly` or
        # `instFamily` (format: "BASE-QUOTE", e.g. "BTC-USDT").
        if (not base or not quote) and inst_type in ("SWAP", "FUTURES"):
            uly = row.get("uly") or row.get("instFamily", "")
            if "-" in uly:
                b, q = uly.split("-", 1)
                base = base or b.upper()
                quote = quote or q.upper()

        tick = Decimal(row["tickSz"])
        lot = Decimal(row["lotSz"])
        min_qty = Decimal(row["minSz"]) if row.get("minSz") else None
        # OKX's `ctVal` is the contract multiplier (face value in base
        # ccy units). 1.0 for spot.
        multiplier = Decimal(row.get("ctVal") or "1")

        if inst_type == "SPOT":
            # Spot: baseCcy/quoteCcy, no settle (use quote).
            if not base or not quote:
                return None
            key = symbology.CanonicalKey(
                class_=symbology.SPOT,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=quote,
            )
            return NormalizedInstrument(
                canonical_symbol=symbology.derive_canonical(key),
                class_=symbology.SPOT,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=quote,
                multiplier=Decimal(1),
                expiry=None,
                strike=None,
                putcall=None,
                venue_native_symbol=instId,
                tick_size=tick,
                lot_size=lot,
                min_qty=min_qty,
                min_notional=None,
                maker_bps=None,
                taker_bps=None,
                listed_at=None,
                status=status,
            )

        if inst_type == "SWAP":
            ct_type = row.get("ctType", "linear")
            if ct_type == "linear":
                class_ = symbology.LINEAR_PERP
            elif ct_type == "inverse":
                class_ = symbology.INVERSE_PERP
            else:
                log.warning("OKX SWAP unknown ctType=%s for %s", ct_type, instId)
                return None
            if not base or not quote or not settle:
                return None
            key = symbology.CanonicalKey(
                class_=class_,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
            )
            return NormalizedInstrument(
                canonical_symbol=symbology.derive_canonical(key),
                class_=class_,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
                multiplier=multiplier,
                expiry=None,
                strike=None,
                putcall=None,
                venue_native_symbol=instId,
                tick_size=tick,
                lot_size=lot,
                min_qty=min_qty,
                min_notional=None,
                maker_bps=None,
                taker_bps=None,
                listed_at=None,
                status=status,
            )

        if inst_type == "FUTURES":
            if not base or not quote or not settle:
                return None
            exp_time = row.get("expTime")
            if not exp_time:
                return None
            expiry = symbology.expiry_from_ms(int(exp_time))
            key = symbology.CanonicalKey(
                class_=symbology.FUTURE,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
                expiry=expiry,
            )
            return NormalizedInstrument(
                canonical_symbol=symbology.derive_canonical(key),
                class_=symbology.FUTURE,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
                multiplier=multiplier,
                expiry=expiry,
                strike=None,
                putcall=None,
                venue_native_symbol=instId,
                tick_size=tick,
                lot_size=lot,
                min_qty=min_qty,
                min_notional=None,
                maker_bps=None,
                taker_bps=None,
                listed_at=None,
                status=status,
            )

        if inst_type == "OPTION":
            # OKX options on BTC-USD index; baseCcy may be empty so
            # extract from `uly` (underlying) when needed.
            uly = row.get("uly", "")  # e.g. 'BTC-USD'
            if "-" in uly:
                base_under, quote_under = uly.split("-", 1)
                base = base or base_under.upper()
                quote = quote or quote_under.upper()
            if not base or not quote:
                return None
            settle = settle or base  # OKX options settle in the underlying
            exp_time = row.get("expTime")
            stk = row.get("stk")
            opt_type = row.get("optType", "").upper()
            if not exp_time or not stk or opt_type not in ("C", "P"):
                return None
            expiry = symbology.expiry_from_ms(int(exp_time))
            strike = Decimal(stk)
            key = symbology.CanonicalKey(
                class_=symbology.OPTION,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
                expiry=expiry,
                strike=strike,
                putcall=opt_type,
            )
            return NormalizedInstrument(
                canonical_symbol=symbology.derive_canonical(key),
                class_=symbology.OPTION,
                base_ccy=base,
                quote_ccy=quote,
                settle_ccy=settle,
                multiplier=multiplier,
                expiry=expiry,
                strike=strike,
                putcall=opt_type,
                venue_native_symbol=instId,
                tick_size=tick,
                lot_size=lot,
                min_qty=min_qty,
                min_notional=None,
                maker_bps=None,
                taker_bps=None,
                listed_at=None,
                status=status,
            )

        return None
