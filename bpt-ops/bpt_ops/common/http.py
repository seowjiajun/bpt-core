"""HTTP client helpers — retries, rate-limit handling, sane defaults.

All exchange-facing fetchers should go through `get_json()` so that transient
failures don't break a scheduled run and so every job has uniform logging.
"""

from __future__ import annotations

from typing import Any

import httpx
from tenacity import (
    retry,
    retry_if_exception_type,
    stop_after_attempt,
    wait_exponential,
)

_DEFAULT_TIMEOUT = httpx.Timeout(connect=10.0, read=30.0, write=10.0, pool=10.0)


@retry(
    reraise=True,
    stop=stop_after_attempt(5),
    wait=wait_exponential(multiplier=1, min=1, max=30),
    retry=retry_if_exception_type((httpx.HTTPError, httpx.ReadTimeout)),
)
def get_json(url: str, *, client: httpx.Client | None = None, **kwargs: Any) -> Any:
    """GET `url`, parse JSON body, retry on transient failures."""
    owned = client is None
    c = client or httpx.Client(timeout=_DEFAULT_TIMEOUT)
    try:
        resp = c.get(url, **kwargs)
        resp.raise_for_status()
        return resp.json()
    finally:
        if owned:
            c.close()
