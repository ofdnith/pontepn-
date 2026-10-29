"""Title ID do Switch: tipo de conteúdo e jogo base (SPEC §1.2)."""

from __future__ import annotations

import re

TID_IN_NAME_RE = re.compile(r"\[([0-9A-Fa-f]{16})\]")
VERSION_IN_NAME_RE = re.compile(r"\[v(\d+)\]", re.IGNORECASE)
TIK_RE = re.compile(r"^([0-9A-Fa-f]{16})[0-9A-Fa-f]{16}\.tik$")


def fmt_tid(value: int) -> str:
    return f"{value:016X}"


def classify(tid: str) -> tuple[str, str | None]:
    """Retorna (content, parent_title_id)."""
    t = int(tid, 16)
    low = t & 0xFFF
    bit12 = bool(t & 0x1000)
    if low == 0x000 and not bit12:
        return "base", None
    if low == 0x800:
        return "update", fmt_tid(t & ~0xFFF)
    if bit12 and low != 0:
        return "dlc", fmt_tid((t & ~0xFFF) - 0x1000)
    return "unknown", None


def tid_from_name(name: str) -> str | None:
    m = TID_IN_NAME_RE.search(name)
    return m.group(1).upper() if m else None


def version_from_name(name: str) -> str | None:
    m = VERSION_IN_NAME_RE.search(name)
    return str(int(m.group(1))) if m else None


def tids_from_tickets(names: list[str]) -> set[str]:
    out = set()
    for n in names:
        m = TIK_RE.match(n)
        if m:
            out.add(m.group(1).upper())
    return out


def clean_title(stem: str) -> str:
    """'Jogo Legal [0100...][v0] (BR)' -> 'Jogo Legal'."""
    t = re.sub(r"\[[^\]]*\]", "", stem)
    t = re.sub(r"\s+", " ", t).strip(" -_.")
    return t or stem
