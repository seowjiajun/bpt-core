"""Codegen: YAML → Python IntEnum module.

Writes bpt_ops/common/_exchanges_generated.py. The content is deterministic
(sorted by id) so re-running the generator with an unchanged YAML produces a
byte-identical file; CI compares the generator's output against the committed
file and fails on drift.
"""

from __future__ import annotations

from pathlib import Path

from bpt_ops.jobs.exchange_catalog.model import ExchangeCatalog


_TEMPLATE = '''\
# AUTO-GENERATED — DO NOT EDIT BY HAND.
# Regenerate with: bpt-ops exchange-catalog generate-python
# Source of truth: messages/exchanges.yaml
"""Exchange IDs — wire-format enum generated from messages/exchanges.yaml."""
from __future__ import annotations

from enum import IntEnum


class ExchangeId(IntEnum):
{members}


EXCHANGE_DISPLAY_NAMES: dict[ExchangeId, str] = {{
{display_names}
}}
'''


def render(catalog: ExchangeCatalog) -> str:
    sorted_exchanges = sorted(catalog.exchanges, key=lambda e: e.id)
    members = "\n".join(f"    {e.name} = {e.id}" for e in sorted_exchanges)
    display_names = "\n".join(
        f"    ExchangeId.{e.name}: {e.display_name!r}," for e in sorted_exchanges
    )
    return _TEMPLATE.format(members=members, display_names=display_names)


def write(catalog: ExchangeCatalog, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(render(catalog))
