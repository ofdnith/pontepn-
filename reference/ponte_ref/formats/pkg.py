"""Leitor de cabeçalho de .pkg (PS4/PS5) — SPEC §1.4.

Campos do cabeçalho em big-endian:
  0x00 magic 7F 43 4E 54
  0x10 entry_count (u32)
  0x18 table_offset (u32)
  0x40 content_id (36 bytes ASCII)
Entradas da tabela: 32 bytes cada; id (+0), offset (+16), size (+20).
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from typing import BinaryIO

from .sfo import SfoError, parse_sfo

MAGIC = b"\x7fCNT"
ENTRY_PARAM_SFO = 0x1000
ENTRY_ICON0 = 0x1200
ENTRY_SIZE = 32
MAX_ENTRIES = 10_000
MAX_SFO = 1 << 20
MAX_ICON = 4 << 20

DEFAULT_PS5_PREFIXES = ("PPSA", "PPSB")

CONTENT_ID_RE = re.compile(r"^[A-Z]{2}\d{4}-([A-Z]{4}\d{5})_\d{2}-[A-Z0-9]{16}$")


class PkgError(ValueError):
    pass


@dataclass
class PkgInfo:
    content_id: str
    title_id: str
    platform_kind: str  # "pkg_ps4" | "pkg_ps5"
    sfo: dict | None
    icon: bytes | None


def read_pkg(f: BinaryIO, file_size: int, ps5_prefixes=DEFAULT_PS5_PREFIXES) -> PkgInfo:
    f.seek(0)
    header = f.read(0x80)
    if len(header) < 0x64:
        raise PkgError("arquivo curto demais para um .pkg")
    if header[:4] != MAGIC:
        raise PkgError("magic desconhecido")

    raw_cid = header[0x40 : 0x40 + 36]
    content_id = raw_cid.split(b"\x00", 1)[0].decode("ascii", "replace")
    m = CONTENT_ID_RE.match(content_id)
    if not m:
        raise PkgError(f"content id inválido: {content_id!r}")
    title_id = m.group(1)
    kind = "pkg_ps5" if title_id[:4] in ps5_prefixes else "pkg_ps4"

    entry_count = struct.unpack_from(">I", header, 0x10)[0]
    table_offset = struct.unpack_from(">I", header, 0x18)[0]
    sfo = None
    icon = None
    if 0 < entry_count <= MAX_ENTRIES and table_offset + entry_count * ENTRY_SIZE <= file_size:
        f.seek(table_offset)
        table = f.read(entry_count * ENTRY_SIZE)
        for i in range(entry_count):
            eid = struct.unpack_from(">I", table, i * ENTRY_SIZE)[0]
            off, size = struct.unpack_from(">II", table, i * ENTRY_SIZE + 16)
            if off + size > file_size:
                continue
            if eid == ENTRY_PARAM_SFO and 0 < size <= MAX_SFO and sfo is None:
                f.seek(off)
                try:
                    sfo = parse_sfo(f.read(size))
                except SfoError:
                    sfo = None
            elif eid == ENTRY_ICON0 and 0 < size <= MAX_ICON and icon is None:
                f.seek(off)
                icon = f.read(size)
    return PkgInfo(content_id, title_id, kind, sfo, icon)


def build_pkg(content_id: str, sfo: bytes | None = None, icon: bytes | None = None, body: bytes = b"") -> bytes:
    """Gera um .pkg sintético com cabeçalho, tabela de entradas e dados. Para testes."""
    entries = []
    if sfo is not None:
        entries.append((ENTRY_PARAM_SFO, sfo))
    if icon is not None:
        entries.append((ENTRY_ICON0, icon))
    table_offset = 0x1000
    data_offset = table_offset + len(entries) * ENTRY_SIZE
    table = b""
    data = b""
    for eid, blob in entries:
        table += struct.pack(">IIIIII", eid, 0, 0, 0, data_offset + len(data), len(blob)) + b"\x00" * 8
        data += blob
    header = bytearray(table_offset)
    header[0:4] = MAGIC
    struct.pack_into(">I", header, 0x10, len(entries))
    struct.pack_into(">I", header, 0x18, table_offset)
    cid = content_id.encode("ascii")[:36]
    header[0x40 : 0x40 + len(cid)] = cid
    return bytes(header) + table + data + body
