"""Leitor e gerador de param.sfo (PS4).

Layout (little-endian):
  0x00 magic "\\0PSF"
  0x04 versão (u32)
  0x08 início da tabela de chaves (u32)
  0x0C início da tabela de dados (u32)
  0x10 número de entradas (u32)
  0x14 entradas de 16 bytes: key_offset u16, fmt u16, len u32, max_len u32, data_offset u32
"""

from __future__ import annotations

import struct

MAGIC = b"\x00PSF"
FMT_UTF8_SPECIAL = 0x0004
FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404

MAX_ENTRIES = 1024


class SfoError(ValueError):
    pass


def parse_sfo(data: bytes) -> dict[str, str | int]:
    if len(data) < 20 or data[:4] != MAGIC:
        raise SfoError("não é um param.sfo")
    _version, key_start, data_start, count = struct.unpack_from("<IIII", data, 4)
    if count > MAX_ENTRIES or 20 + count * 16 > len(data):
        raise SfoError("número de entradas inválido")
    out: dict[str, str | int] = {}
    for i in range(count):
        key_off, fmt, length, _max_len, data_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        k0 = key_start + key_off
        k1 = data.find(b"\x00", k0)
        if k0 >= len(data) or k1 < 0:
            raise SfoError("chave fora dos limites")
        key = data[k0:k1].decode("ascii", "replace")
        d0 = data_start + data_off
        raw = data[d0 : d0 + length]
        if len(raw) != length:
            raise SfoError(f"dado fora dos limites em {key}")
        if fmt == FMT_INT32:
            out[key] = struct.unpack("<I", raw[:4].ljust(4, b"\x00"))[0]
        elif fmt in (FMT_UTF8, FMT_UTF8_SPECIAL):
            out[key] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
        else:
            continue
    return out


def build_sfo(values: dict[str, str | int]) -> bytes:
    """Gera um param.sfo válido. Usado em testes e em dados sintéticos."""
    keys = sorted(values)
    key_table = b""
    key_offsets = []
    for k in keys:
        key_offsets.append(len(key_table))
        key_table += k.encode("ascii") + b"\x00"
    while len(key_table) % 4:
        key_table += b"\x00"

    data_table = b""
    entries = b""
    for k, koff in zip(keys, key_offsets):
        v = values[k]
        if isinstance(v, int):
            raw = struct.pack("<I", v)
            fmt, length, max_len = FMT_INT32, 4, 4
        else:
            raw = v.encode("utf-8") + b"\x00"
            fmt, length = FMT_UTF8, len(raw)
            max_len = (length + 3) // 4 * 4
            raw = raw.ljust(max_len, b"\x00")
        entries += struct.pack("<HHIII", koff, fmt, length, max_len, len(data_table))
        data_table += raw

    key_start = 20 + len(entries)
    data_start = key_start + len(key_table)
    header = MAGIC + struct.pack("<IIII", 0x0101, key_start, data_start, len(keys))
    return header + entries + key_table + data_table
