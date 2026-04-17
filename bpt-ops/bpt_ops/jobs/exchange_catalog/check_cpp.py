"""Verify that messages/generated/cpp/messages/ExchangeId.h agrees with the YAML.

The SBE toolchain regenerates this header from an XML schema; in practice the
repo ships the hand-committed header without a live XML source. This check
doesn't depend on SBE — it regex-parses the enum literals out of the header
and compares them to the YAML. If anyone edits one side without the other,
CI fails.
"""
from __future__ import annotations

import re
from pathlib import Path

from bpt_ops.jobs.exchange_catalog.model import ExchangeCatalog

# Matches lines like:   BINANCE = static_cast<std::uint8_t>(1),
_ENUM_LINE_RE = re.compile(
    r"^\s*(?P<name>[A-Z][A-Z0-9_]*)\s*=\s*static_cast<std::uint8_t>\((?P<id>\d+)\)\s*,\s*$",
    re.MULTILINE,
)


_SENTINELS: set[str] = {
    "NULL_VALUE",  # SBE-emitted placeholder
    "ALL",         # protocol sentinel meaning "all venues" (cancel-all, etc.)
}


def parse_cpp_enum(header_path: Path) -> dict[str, int]:
    text = header_path.read_text()
    found: dict[str, int] = {}
    for m in _ENUM_LINE_RE.finditer(text):
        name = m.group("name")
        if name in _SENTINELS:
            continue
        found[name] = int(m.group("id"))
    return found


def check(catalog: ExchangeCatalog, header_path: Path) -> list[str]:
    """Return a list of discrepancy messages. Empty list means header matches YAML."""
    if not header_path.exists():
        return [f"C++ header not found: {header_path}"]

    cpp_enum = parse_cpp_enum(header_path)
    yaml_enum = {e.name: e.id for e in catalog.exchanges}

    findings: list[str] = []

    missing_in_cpp = set(yaml_enum) - set(cpp_enum)
    missing_in_yaml = set(cpp_enum) - set(yaml_enum)
    for name in sorted(missing_in_cpp):
        findings.append(
            f"{name} (id={yaml_enum[name]}) is in YAML but missing from {header_path.name}"
        )
    for name in sorted(missing_in_yaml):
        findings.append(
            f"{name} (id={cpp_enum[name]}) is in {header_path.name} but missing from YAML"
        )

    for name in sorted(set(cpp_enum) & set(yaml_enum)):
        if cpp_enum[name] != yaml_enum[name]:
            findings.append(
                f"{name}: YAML says id={yaml_enum[name]}, "
                f"{header_path.name} says id={cpp_enum[name]}"
            )

    return findings
