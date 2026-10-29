"""Detecção e agrupamento de arquivos compactados (SPEC §1.3).

Não extrai nada: isso é trabalho do app do Mac. Aqui só se reconhece o
conjunto de partes e se há buracos na numeração.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

_PATTERNS = [
    # (regex, extensão do conjunto, se é numerado)
    (re.compile(r"^(?P<base>.+)\.part(?P<n>\d+)\.rar$", re.I), "rar"),
    (re.compile(r"^(?P<base>.+)\.(?P<ext>7z|zip|rar|tar)\.(?P<n>\d{3})$", re.I), None),
    (re.compile(r"^(?P<base>.+)\.r(?P<n>\d{2})$", re.I), "rar-old"),
    (re.compile(r"^(?P<base>.+)\.z(?P<n>\d{2})$", re.I), "zip-old"),
]
_SINGLE = re.compile(r"^(?P<base>.+)\.(?P<ext>zip|rar|7z|tar|tgz|tar\.gz)$", re.I)


@dataclass
class ArchiveSet:
    key: str  # nome do conjunto, ex.: "Jogo.rar"
    parts: list[Path] = field(default_factory=list)
    numbers: list[int] = field(default_factory=list)

    @property
    def missing(self) -> list[int]:
        if not self.numbers:
            return []
        nums = sorted(set(self.numbers))
        start = 0 if 0 in nums else 1
        return [n for n in range(start, nums[-1] + 1) if n not in nums]

    @property
    def complete(self) -> bool:
        return not self.missing


def match_archive(name: str) -> tuple[str, int | None] | None:
    """Retorna (chave do conjunto, número da parte ou None) se `name` for compactado."""
    for rx, kind in _PATTERNS:
        m = rx.match(name)
        if not m:
            continue
        base = m.group("base")
        n = int(m.group("n"))
        if kind == "rar":
            return f"{base}.rar", n
        if kind == "rar-old":
            return f"{base}.rar", n + 1  # .rar é a parte 0, .r00 a parte 1…
        if kind == "zip-old":
            return f"{base}.zip", n
        return f"{base}.{m.group('ext').lower()}", n
    m = _SINGLE.match(name)
    if m:
        return f"{m.group('base')}.{m.group('ext').lower()}", None
    return None


def group_archives(paths: list[Path]) -> list[ArchiveSet]:
    sets: dict[tuple[Path, str], ArchiveSet] = {}
    for p in paths:
        hit = match_archive(p.name)
        if not hit:
            continue
        key, n = hit
        s = sets.setdefault((p.parent, key.lower()), ArchiveSet(key))
        s.parts.append(p)
        if n is not None:
            s.numbers.append(n)
        elif p.suffix.lower() == ".rar":
            # rar antigo: "Jogo.rar" é a parte 0 de "Jogo.r00", "Jogo.r01"…
            # Sozinho, é um rar simples; o número 0 só importa se houver .rNN.
            s.numbers.append(0)
        # "Jogo.zip" ao lado de "Jogo.z01…" é a última parte do zip dividido:
        # não tem número próprio e não entra na checagem de buracos.
    for s in sets.values():
        s.parts.sort()
        if s.numbers == [0]:
            s.numbers = []
    return sorted(sets.values(), key=lambda s: s.key.lower())
