"""Leitor do contêiner PFS0 (NSP/NSZ do Switch).

Layout (little-endian):
  0x00 magic "PFS0"
  0x04 número de arquivos (u32)
  0x08 tamanho da tabela de nomes (u32)
  0x0C reservado
  0x10 entradas de 0x18 bytes: offset u64, size u64, name_offset u32, reservado u32
  depois: tabela de nomes (textos terminados em zero)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import BinaryIO

MAGIC = b"PFS0"
ENTRY_SIZE = 0x18
MAX_FILES = 100_000
MAX_STRINGS = 16 << 20


class Pfs0Error(ValueError):
    pass


@dataclass
class Pfs0Entry:
    name: str
    offset: int
    size: int


def list_pfs0(f: BinaryIO) -> list[Pfs0Entry]:
    f.seek(0)
    head = f.read(16)
    if len(head) < 16 or head[:4] != MAGIC:
        raise Pfs0Error("não é um PFS0")
    count, str_size = struct.unpack_from("<II", head, 4)
    if count > MAX_FILES or str_size > MAX_STRINGS:
        raise Pfs0Error("cabeçalho PFS0 inválido")
    raw_entries = f.read(count * ENTRY_SIZE)
    strings = f.read(str_size)
    if len(raw_entries) != count * ENTRY_SIZE or len(strings) != str_size:
        raise Pfs0Error("PFS0 truncado")
    out = []
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", raw_entries, i * ENTRY_SIZE)
        end = strings.find(b"\x00", name_off)
        if name_off >= len(strings) or end < 0:
            raise Pfs0Error("nome fora dos limites")
        out.append(Pfs0Entry(strings[name_off:end].decode("utf-8", "replace"), off, size))
    return out


def build_pfs0(files: dict[str, bytes]) -> bytes:
    """Gera um PFS0 sintético. Para testes."""
    names = list(files)
    strings = b""
    name_offsets = []
    for n in names:
        name_offsets.append(len(strings))
        strings += n.encode("utf-8") + b"\x00"
    while (16 + len(names) * ENTRY_SIZE + len(strings)) % 0x20:
        strings += b"\x00"
    entries = b""
    data = b""
    for n, noff in zip(names, name_offsets):
        entries += struct.pack("<QQII", len(data), len(files[n]), noff, 0)
        data += files[n]
    return MAGIC + struct.pack("<III", len(names), len(strings), 0) + entries + strings + data
