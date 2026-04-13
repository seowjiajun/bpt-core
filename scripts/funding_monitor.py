#!/usr/bin/env python3
"""
Mainnet funding-rate monitor.

Polls public REST endpoints on OKX, Binance, and Deribit for the current
perpetual funding rate on BTC and ETH, and prints a table every minute.
No authentication required — everything is public market data.

Use this to answer "does funding arb have real edge right now?" before
investing time wiring a live strategy. If mainnet funding is consistently
above ~5 bps per period, there's tradeable signal. Near zero = no edge.

Usage:
    ./scripts/funding_monitor.py                 # poll every 60s, print table
    ./scripts/funding_monitor.py --interval 30   # custom poll interval
    ./scripts/funding_monitor.py --csv out.csv   # also append to CSV

Annualized rate assumes 8h funding periods for OKX/Binance/Deribit
(3 payments per day × 365 days = 1095 periods/year).
"""

import argparse
import csv
import json
import sys
import time
import urllib.request
from datetime import datetime, timezone


# Funding period varies by venue.  Annualization must use the right count.
#   8h venues (OKX/Binance/Deribit): 3 payments/day × 365 = 1095 periods/yr
#   Hyperliquid: hourly → 24 × 365 = 8760 periods/yr
PERIODS_8H = 1095
PERIODS_1H = 8760


def _okx_extract(j):
    return float(j["data"][0]["fundingRate"])


def _binance_extract(j):
    return float(j["lastFundingRate"])


def _deribit_extract(j):
    return float(j["result"]["funding_8h"])


# Hyperliquid uses one POST endpoint for everything. We fetch once and cache.
_hl_cache = {"ts": 0, "data": None}


def _hl_fetch():
    now = time.time()
    if now - _hl_cache["ts"] < 5 and _hl_cache["data"]:
        return _hl_cache["data"]
    req = urllib.request.Request(
        "https://api.hyperliquid.xyz/info",
        data=b'{"type":"metaAndAssetCtxs"}',
        headers={"Content-Type": "application/json", "User-Agent": "funding-monitor/0.1"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        j = json.loads(resp.read())
    meta, ctxs = j[0], j[1]
    by_name = {}
    for i, u in enumerate(meta["universe"]):
        by_name[u["name"]] = ctxs[i]
    _hl_cache["ts"] = now
    _hl_cache["data"] = by_name
    return by_name


def _hl_extract(asset):
    def _inner(_):
        data = _hl_fetch()
        return float(data[asset]["funding"])
    return _inner


VENUES = [
    {
        "name": "OKX",
        "symbol": "BTC-USDT-SWAP",
        "periods": PERIODS_8H,
        "url": "https://www.okx.com/api/v5/public/funding-rate?instId=BTC-USDT-SWAP",
        "extract": _okx_extract,
    },
    {
        "name": "OKX",
        "symbol": "ETH-USDT-SWAP",
        "periods": PERIODS_8H,
        "url": "https://www.okx.com/api/v5/public/funding-rate?instId=ETH-USDT-SWAP",
        "extract": _okx_extract,
    },
    {
        "name": "Binance",
        "symbol": "BTCUSDT",
        "periods": PERIODS_8H,
        "url": "https://fapi.binance.com/fapi/v1/premiumIndex?symbol=BTCUSDT",
        "extract": _binance_extract,
    },
    {
        "name": "Binance",
        "symbol": "ETHUSDT",
        "periods": PERIODS_8H,
        "url": "https://fapi.binance.com/fapi/v1/premiumIndex?symbol=ETHUSDT",
        "extract": _binance_extract,
    },
    {
        "name": "Deribit",
        "symbol": "BTC-PERPETUAL",
        "periods": PERIODS_8H,
        "url": "https://www.deribit.com/api/v2/public/ticker?instrument_name=BTC-PERPETUAL",
        "extract": _deribit_extract,
    },
    {
        "name": "Deribit",
        "symbol": "ETH-PERPETUAL",
        "periods": PERIODS_8H,
        "url": "https://www.deribit.com/api/v2/public/ticker?instrument_name=ETH-PERPETUAL",
        "extract": _deribit_extract,
    },
    {
        "name": "Hyperliquid",
        "symbol": "BTC",
        "periods": PERIODS_1H,
        "url": "https://api.hyperliquid.xyz/info",  # unused — custom POST in _hl_fetch
        "extract": _hl_extract("BTC"),
    },
    {
        "name": "Hyperliquid",
        "symbol": "ETH",
        "periods": PERIODS_1H,
        "url": "https://api.hyperliquid.xyz/info",
        "extract": _hl_extract("ETH"),
    },
]


def fetch(venue):
    try:
        # Hyperliquid uses a cached POST — extract() ignores the passed arg.
        if venue["name"] == "Hyperliquid":
            rate = venue["extract"](None)
        else:
            req = urllib.request.Request(
                venue["url"],
                headers={"User-Agent": "funding-monitor/0.1"},
            )
            with urllib.request.urlopen(req, timeout=10) as resp:
                j = json.loads(resp.read())
            rate = venue["extract"](j)
        return rate, None
    except Exception as e:
        return None, str(e)[:80]


def fmt_pct(x):
    return f"{x * 100:+.4f}%"


def fmt_bps(x):
    return f"{x * 10000:+.2f}"


def fmt_annual(rate, periods):
    return f"{rate * periods * 100:+.1f}%"


def print_table(rows):
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    print(f"\n{now}")
    print(f"{'Venue':<12} {'Symbol':<18} {'Period':>7} {'Rate':>12} {'bps':>8} {'Annualized':>12}")
    print("-" * 75)
    for venue, symbol, periods, rate, err in rows:
        period_label = "1h" if periods == PERIODS_1H else "8h"
        if err:
            print(f"{venue:<12} {symbol:<18} {period_label:>7} {'ERR':>12} {'-':>8} {err:>12}")
        else:
            print(
                f"{venue:<12} {symbol:<18} {period_label:>7} {fmt_pct(rate):>12} "
                f"{fmt_bps(rate):>8} {fmt_annual(rate, periods):>12}"
            )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--interval", type=int, default=60, help="seconds between polls (default 60)")
    ap.add_argument("--csv", type=str, default=None, help="optional path to append a CSV row per poll")
    ap.add_argument("--once", action="store_true", help="poll once and exit")
    args = ap.parse_args()

    csv_file = None
    csv_writer = None
    if args.csv:
        new_file = not __import__("os").path.exists(args.csv)
        csv_file = open(args.csv, "a", newline="")
        csv_writer = csv.writer(csv_file)
        if new_file:
            csv_writer.writerow(["timestamp", "venue", "symbol", "periods_per_year", "rate", "rate_bps", "annualized_pct"])

    try:
        while True:
            rows = []
            for venue in VENUES:
                rate, err = fetch(venue)
                rows.append((venue["name"], venue["symbol"], venue["periods"], rate, err))
                if csv_writer and rate is not None:
                    csv_writer.writerow(
                        [
                            datetime.now(timezone.utc).isoformat(),
                            venue["name"],
                            venue["symbol"],
                            venue["periods"],
                            f"{rate:.10f}",
                            f"{rate * 10000:.4f}",
                            f"{rate * venue['periods'] * 100:.2f}",
                        ]
                    )
            print_table(rows)
            if csv_file:
                csv_file.flush()
            if args.once:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if csv_file:
            csv_file.close()


if __name__ == "__main__":
    main()
