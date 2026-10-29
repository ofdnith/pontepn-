"""Perfis de compatibilidade por firmware (SPEC §1.1.1)."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .versions import parse_firmware

STATUSES = {"ok", "experimental", "unknown", "blocked"}


@dataclass
class Profile:
    platform: str
    firmware: str
    kinds: dict[str, dict]

    def status(self, kind: str) -> tuple[str, str | None]:
        entry = self.kinds.get(kind)
        if not isinstance(entry, dict) or entry.get("status") not in STATUSES:
            return "unknown", None
        return entry["status"], entry.get("note")


def load_profiles(directory: Path) -> list[Profile]:
    out = []
    if not directory.is_dir():
        return out
    for p in sorted(directory.glob("*.json")):
        try:
            data = json.loads(p.read_text("utf-8"))
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict) or parse_firmware(data.get("firmware")) is None:
            continue
        out.append(Profile(data.get("platform", ""), data["firmware"], data.get("kinds", {})))
    return out


def select_profile(profiles: list[Profile], platform: str, firmware: str | None) -> tuple[Profile | None, bool]:
    """Perfil de maior firmware <= firmware do console.

    Retorna (perfil, adivinhado). Sem perfil menor ou igual, usa o de menor
    firmware e marca como adivinhado. Firmware desconhecido -> (None, False).
    """
    fw = parse_firmware(firmware)
    candidates = [p for p in profiles if p.platform == platform]
    if fw is None or not candidates:
        return None, False
    candidates.sort(key=lambda p: parse_firmware(p.firmware))
    below = [p for p in candidates if parse_firmware(p.firmware) <= fw]
    if below:
        return below[-1], False
    return candidates[0], True
