"""
Lambda entry point for the daily secmaster refresh.

Flow per invocation:
  1. For each venue (OKX, HL, Binance, Deribit):
       a. Open a fresh ingest_run row.
       b. Call the venue's ingester to pull + normalize.
       c. UPSERT each NormalizedInstrument via the SCD-2 helpers.
       d. Close the ingest_run row with stats + status.
  2. Aggregate stats across venues into a summary.
  3. POST summary to Discord webhook (if SECMASTER_DISCORD_WEBHOOK set).
  4. Return summary to the Lambda runtime.

Env vars (Lambda config or local .env):
  Either:
    SECMASTER_DSN              postgres://user:pass@host:5432/db
  Or (preferred in AWS — Lambda fetches from Secrets Manager):
    SECMASTER_DB_SECRET_ARN    arn:aws:secretsmanager:...:secret/bpt-secmaster/db-xxxxxx
  Plus optional:
    SECMASTER_DISCORD_WEBHOOK  if set, daily summary is posted
    SECMASTER_VENUES           CSV filter, e.g. 'okx,hl'

Per-venue failures are isolated: one venue erroring doesn't stop the
others. The ingest_run table records per-venue outcome so you can
spot a chronically broken venue from the UI without diving into logs.

Run locally:
    SECMASTER_DSN=postgres://localhost/secmaster_dev \
    python -m handler
"""

from __future__ import annotations

import json
import logging
import os
import sys
import time
from dataclasses import asdict
from typing import Any

import httpx

from db import (
    IngestStats,
    connect,
    exchange_id_by_code,
    finish_ingest_run,
    start_ingest_run,
    transaction,
    upsert_instrument,
    upsert_listing,
    upsert_symbology,
)
from venues import all_ingesters

log = logging.getLogger("bpt-secmaster.refresh")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)

CHANGE_SOURCE = "refresh.lambda.v1"


def lambda_handler(event: dict, context: Any) -> dict:
    """AWS Lambda entrypoint. Event payload is ignored for the cron path."""
    return _run()


# ─────────────────────────── core refresh loop ────────────────────────


def _run() -> dict:
    dsn = _resolve_dsn()
    venue_filter = _parse_venue_filter()

    conn = connect(dsn)
    summary = {"venues": {}, "total": IngestStats()}

    try:
        for ingester_cls in all_ingesters():
            code = ingester_cls.exchange_code
            if venue_filter and code not in venue_filter:
                continue
            try:
                stats = _run_one_venue(conn, ingester_cls)
                summary["venues"][code] = asdict(stats)
                summary["total"].rows_added += stats.rows_added
                summary["total"].rows_modified += stats.rows_modified
                summary["total"].rows_unchanged += stats.rows_unchanged
                summary["total"].error_count += stats.error_count
            except Exception as e:
                log.exception("venue %s failed entirely: %s", code, e)
                summary["venues"][code] = {"fatal_error": str(e)}
                summary["total"].error_count += 1
        # End-of-refresh: render the instrument_mapping.json snapshot
        # for trading hosts to pull from S3. Only render if at least
        # one venue succeeded — don't overwrite the last-good snapshot
        # with one built from a fully-failed refresh.
        if summary["total"].rows_added + summary["total"].rows_unchanged > 0:
            try:
                _render_and_upload(conn)
            except Exception as e:
                log.exception("snapshot render/upload failed (data still in RDS): %s", e)
                summary["snapshot_error"] = str(e)
    finally:
        conn.close()

    summary["total"] = asdict(summary["total"])
    log.info("refresh complete: %s", summary["total"])

    _post_discord_summary(summary)
    return summary


def _render_and_upload(conn) -> None:
    """
    Render the legacy instrument_mapping.json snapshot from RDS and
    upload to S3. Trading hosts pull from S3 via systemd timer.

    Skipped silently if SECMASTER_SNAPSHOT_S3_URI isn't set (local-dev
    or single-host mode with no S3 cutover yet).
    """
    s3_uri = os.environ.get("SECMASTER_SNAPSHOT_S3_URI")
    if not s3_uri:
        log.info("no SECMASTER_SNAPSHOT_S3_URI set; skipping render+upload")
        return

    import boto3  # lazy: only when actually uploading

    from render import render_mapping_json

    log.info("rendering instrument_mapping snapshot")
    blob = render_mapping_json(conn)
    log.info("rendered %d bytes; uploading to %s", len(blob), s3_uri)

    bucket, key = _parse_s3_uri(s3_uri)
    s3 = boto3.client("s3")
    s3.put_object(
        Bucket=bucket,
        Key=key,
        Body=blob.encode("utf-8"),
        ContentType="application/json",
        # Cache-Control lets pullers know to revalidate aggressively.
        CacheControl="max-age=0, must-revalidate",
    )
    log.info("uploaded snapshot to s3://%s/%s", bucket, key)


def _parse_s3_uri(uri: str) -> tuple[str, str]:
    if not uri.startswith("s3://"):
        raise ValueError(f"bad S3 URI: {uri!r}")
    rest = uri[len("s3://"):]
    bucket, _, key = rest.partition("/")
    if not bucket or not key:
        raise ValueError(f"S3 URI missing bucket or key: {uri!r}")
    return bucket, key


# ─────────────────────────── per-venue execution ──────────────────────


def _run_one_venue(conn, ingester_cls) -> IngestStats:
    code = ingester_cls.exchange_code
    stats = IngestStats()
    started = time.time()

    with transaction(conn) as cur:
        exchange_id = exchange_id_by_code(cur, code)
        run_id = start_ingest_run(cur, code)

    # Fetch + normalize OUTSIDE the transaction so HTTP latency doesn't
    # hold a long-running write lock. Then upsert in a second
    # transaction.
    log.info("[%s] fetching from venue…", code)
    with httpx.Client(http2=True) as http:
        ingester = ingester_cls(http)
        normalized = list(ingester.fetch())
    log.info("[%s] fetched %d normalized rows", code, len(normalized))

    with transaction(conn) as cur:
        for inst in normalized:
            try:
                inst_id, inst_outcome = upsert_instrument(cur, inst, CHANGE_SOURCE)
                _, listing_outcome = upsert_listing(
                    cur, inst_id, exchange_id, inst, CHANGE_SOURCE
                )
                # Bookkeep symbology: venue_native + ccxt (ccxt-style
                # canonical happens to be derivable from canonical_symbol
                # for now; deferred until we need vendor-specific
                # variations).
                upsert_symbology(cur, inst_id, "venue_native", inst.venue_native_symbol)

                # Roll-up outcome — count instrument-level change for
                # added/modified; otherwise count as unchanged.
                if inst_outcome == "added" or listing_outcome == "added":
                    stats.rows_added += 1
                elif inst_outcome == "modified" or listing_outcome == "modified":
                    stats.rows_modified += 1
                else:
                    stats.rows_unchanged += 1
            except Exception as e:
                stats.error_count += 1
                stats.errors.append(f"{inst.canonical_symbol}: {e}")
                log.warning("[%s] upsert failed for %s: %s", code, inst.canonical_symbol, e)

        status = "ok" if stats.error_count == 0 else "partial"
        finish_ingest_run(cur, run_id, stats, status)

    elapsed = time.time() - started
    log.info(
        "[%s] complete in %.1fs: +%d ~%d =%d (errors=%d)",
        code,
        elapsed,
        stats.rows_added,
        stats.rows_modified,
        stats.rows_unchanged,
        stats.error_count,
    )
    return stats


# ────────────────────────────── helpers ───────────────────────────────


def _resolve_dsn() -> str:
    """
    Two delivery paths:
      - SECMASTER_DSN env var (local dev, docker run)
      - SECMASTER_DB_SECRET_ARN env var (Lambda — fetch from Secrets Manager)

    Direct DSN wins if both are set, so local-dev overrides remain easy.
    """
    direct = os.environ.get("SECMASTER_DSN")
    if direct:
        return direct

    secret_arn = os.environ.get("SECMASTER_DB_SECRET_ARN")
    if not secret_arn:
        raise RuntimeError(
            "neither SECMASTER_DSN nor SECMASTER_DB_SECRET_ARN is set"
        )

    # boto3 is in the Lambda Python base image — no need to pin in
    # requirements.txt. Import lazily so local-dev users without
    # boto3 installed can still use the direct DSN path.
    import boto3  # type: ignore

    sm = boto3.client("secretsmanager")
    secret = sm.get_secret_value(SecretId=secret_arn)
    payload = json.loads(secret["SecretString"])
    return payload["dsn"]


def _parse_venue_filter() -> set[str] | None:
    raw = os.environ.get("SECMASTER_VENUES", "").strip()
    if not raw:
        return None
    return {v.strip().lower() for v in raw.split(",") if v.strip()}


def _post_discord_summary(summary: dict) -> None:
    """Optional Discord webhook for the daily ingest report."""
    webhook = os.environ.get("SECMASTER_DISCORD_WEBHOOK")
    if not webhook:
        return
    total = summary["total"]
    lines = [
        "**bpt-secmaster refresh complete**",
        f"  added: {total['rows_added']}",
        f"  modified: {total['rows_modified']}",
        f"  unchanged: {total['rows_unchanged']}",
        f"  errors: {total['error_count']}",
        "",
        "Per venue:",
    ]
    for code, v in summary["venues"].items():
        if "fatal_error" in v:
            lines.append(f"  {code}: ✗ {v['fatal_error']}")
        else:
            lines.append(
                f"  {code}: +{v['rows_added']} ~{v['rows_modified']} "
                f"={v['rows_unchanged']} (errors={v['error_count']})"
            )
    try:
        httpx.post(webhook, json={"content": "\n".join(lines)}, timeout=10.0)
    except Exception as e:
        log.warning("Discord webhook post failed: %s", e)


# ─────────────────────────── local invocation ─────────────────────────


if __name__ == "__main__":
    try:
        out = _run()
    except Exception:
        log.exception("refresh failed")
        sys.exit(1)
    print(out)
