"""Comparação de versões e firmwares (SPEC §5.7) e conversões BCD (SPEC §1.4, §1.5)."""

from __future__ import annotations

import re


def _bcd_byte(b: int) -> int:
    return (b >> 4) * 10 + (b & 0x0F)


def firmware_from_bcd(value: int, width_bytes: int) -> str | None:
    """Converte um firmware em BCD para "X.YY".

    Os dois bytes mais altos guardam versão maior e menor:
    0x05050000 (4 bytes) -> "5.05"; 0x0403000000000000 (8 bytes) -> "4.03".
    """
    if value <= 0:
        return None
    shift = (width_bytes - 1) * 8
    major = _bcd_byte((value >> shift) & 0xFF)
    minor = _bcd_byte((value >> (shift - 8)) & 0xFF)
    return f"{major}.{minor:02d}"


def firmware_from_hex_text(text: str | None) -> str | None:
    """`requiredSystemSoftwareVersion` do param.json: "0x0403000000000000" -> "4.03"."""
    if not text or not isinstance(text, str):
        return None
    try:
        value = int(text, 16)
    except ValueError:
        return None
    digits = text.lower().removeprefix("0x")
    width = max(1, (len(digits) + 1) // 2)
    if width < 2:
        return None
    return firmware_from_bcd(value, width)


_FW_RE = re.compile(r"^\s*(\d+)\.(\d+)\s*$")


def parse_firmware(text: str | None) -> tuple[int, int] | None:
    if not text:
        return None
    m = _FW_RE.match(text)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def compare_firmware(a: str | None, b: str | None) -> int | None:
    """-1, 0, 1; None se algum não for um firmware válido."""
    pa, pb = parse_firmware(a), parse_firmware(b)
    if pa is None or pb is None:
        return None
    return (pa > pb) - (pa < pb)


def _version_parts(text: str) -> list[int] | None:
    parts = text.strip().split(".")
    out = []
    for p in parts:
        if not p.isdigit():
            return None
        out.append(int(p))
    return out


def compare_versions(a: str | None, b: str | None, platform: str) -> int:
    """Compara versões de conteúdo. Retorna -1, 0 ou 1.

    Versão None (ou ilegível) de qualquer lado conta como igual, para não
    sugerir cópias à toa (SPEC §5.7).
    """
    if a is None or b is None:
        return 0
    if platform == "switch":
        if not (a.strip().isdigit() and b.strip().isdigit()):
            return 0
        ia, ib = int(a), int(b)
        return (ia > ib) - (ia < ib)
    pa, pb = _version_parts(a), _version_parts(b)
    if pa is None or pb is None:
        return 0
    n = max(len(pa), len(pb))
    pa += [0] * (n - len(pa))
    pb += [0] * (n - len(pb))
    return (pa > pb) - (pa < pb)
