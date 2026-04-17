#!/usr/bin/env python3
"""Emergency flatten tool for Hyperliquid positions.

Fetches open positions + resting orders for the configured HL wallet,
prints them, waits for explicit confirmation, then cancels all resting
orders and closes every non-zero position with a market order.

Read-only by default. Actually closes only after you type 'FLATTEN' at
the prompt. Use this when the stack has been stopped with open state
and you need to clean up before restarting — or as a manual kill switch
when something has gone wrong and you don't trust fenrir's shutdown.

Usage:
    python3 scripts/flatten_hl_positions.py                  # testnet (default)
    python3 scripts/flatten_hl_positions.py --mainnet        # mainnet
    python3 scripts/flatten_hl_positions.py --dry-run        # just show state

Credentials are loaded from AWS Secrets Manager at bpt/{testnet,prod}/
HYPERLIQUID — same path the C++ stack reads. Requires AWS creds with
secretsmanager:GetSecretValue on that secret.
"""
import argparse
import json
import sys

import boto3
from hyperliquid.exchange import Exchange
from hyperliquid.info import Info
from hyperliquid.utils import constants
from eth_account import Account


def fetch_credentials(mainnet: bool) -> tuple[str, str]:
    env = "prod" if mainnet else "testnet"
    secret_id = f"bpt/{env}/HYPERLIQUID"
    sm = boto3.client("secretsmanager", region_name="ap-southeast-1")
    resp = sm.get_secret_value(SecretId=secret_id)
    blob = json.loads(resp["SecretString"])
    pk = blob["HYPERLIQUID_PRIVATE_KEY"]
    addr = blob["HYPERLIQUID_WALLET_ADDRESS"]
    if not pk.startswith("0x"):
        pk = "0x" + pk
    return pk, addr


def show_state(info: Info, wallet: str) -> tuple[list, list]:
    """Return (open_positions, open_orders). Also prints them."""
    state = info.user_state(wallet)
    positions = [
        ap for ap in state.get("assetPositions", [])
        if float(ap["position"]["szi"]) != 0.0
    ]
    orders = info.open_orders(wallet)

    print(f"\n─── Account state for {wallet} ───")
    print(f"  marginSummary.accountValue : {state.get('marginSummary', {}).get('accountValue', '?')}")
    print(f"  withdrawable               : {state.get('withdrawable', '?')}")

    if not positions:
        print("\n  Open positions: NONE")
    else:
        print(f"\n  Open positions ({len(positions)}):")
        for ap in positions:
            p = ap["position"]
            print(
                f"    {p['coin']:<8} size={p['szi']:>12}  "
                f"entry={p.get('entryPx', '?')}  "
                f"unrealizedPnl={p.get('unrealizedPnl', '?')}"
            )

    if not orders:
        print("\n  Resting orders: NONE")
    else:
        print(f"\n  Resting orders ({len(orders)}):")
        for o in orders:
            print(
                f"    oid={o['oid']:<20} {o['coin']:<8} "
                f"{'BUY ' if o['side'] == 'B' else 'SELL'} "
                f"sz={o['sz']:<10} px={o['limitPx']}"
            )
    return positions, orders


def flatten(exch: Exchange, positions: list, orders: list) -> int:
    errors = 0

    # Cancel resting orders first so they can't interfere with the close.
    for o in orders:
        try:
            resp = exch.cancel(o["coin"], int(o["oid"]))
            status = resp.get("status", "?")
            print(f"  cancel  {o['coin']:<8} oid={o['oid']:<20} → {status}")
            if status != "ok":
                errors += 1
        except Exception as e:
            print(f"  cancel  {o['coin']:<8} oid={o['oid']:<20} → EXCEPTION {e}")
            errors += 1

    # Close positions with market orders (reduce-only).
    for ap in positions:
        p = ap["position"]
        coin = p["coin"]
        sz = float(p["szi"])
        is_buy_to_close = sz < 0  # short → buy to cover; long → sell to close
        abs_sz = abs(sz)
        try:
            resp = exch.market_close(coin)
            status = resp.get("status", "?")
            print(
                f"  close   {coin:<8} {'cover ' if is_buy_to_close else 'sell  '}"
                f"sz={abs_sz:<12} → {status}"
            )
            if status != "ok":
                errors += 1
                print(f"          response: {resp}")
        except Exception as e:
            print(f"  close   {coin:<8} → EXCEPTION {e}")
            errors += 1

    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mainnet", action="store_true", help="target mainnet (default: testnet)")
    ap.add_argument("--dry-run", action="store_true", help="show state only, do not flatten")
    args = ap.parse_args()

    network = "MAINNET" if args.mainnet else "testnet"
    api_url = constants.MAINNET_API_URL if args.mainnet else constants.TESTNET_API_URL

    print(f"HL flatten tool — {network} ({api_url})")
    pk, wallet = fetch_credentials(args.mainnet)
    signer = Account.from_key(pk)
    if signer.address.lower() != wallet.lower():
        print(
            f"NOTE: signer address {signer.address} differs from wallet {wallet} — "
            "that's expected for an API/agent wallet signing for the main wallet."
        )

    info = Info(api_url, skip_ws=True)
    positions, orders = show_state(info, wallet)

    if not positions and not orders:
        print("\nNothing to flatten. Exiting.")
        return 0

    if args.dry_run:
        print("\n--dry-run: exiting without touching anything.")
        return 0

    print(f"\nAbout to flatten {len(positions)} position(s) and cancel {len(orders)} order(s) on {network}.")
    reply = input("Type 'FLATTEN' to proceed, anything else to abort: ").strip()
    if reply != "FLATTEN":
        print("Aborted.")
        return 1

    exch = Exchange(signer, api_url, account_address=wallet)
    print("\nExecuting...")
    errors = flatten(exch, positions, orders)

    print("\n─── Post-flatten state ───")
    show_state(info, wallet)

    if errors:
        print(f"\n{errors} error(s) encountered. Review output above and re-run if needed.")
        return 2
    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
