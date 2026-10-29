"""Biblioteca do Mac: varredura, estabilidade, itens, duplicados e hashes (SPEC §5.3, §5.4, §7).

A referência não extrai compactados nem converte pastas em .exfat (isso exige
ferramentas do macOS); ela só lista os compactados encontrados e calcula o
plano de conversão, para o app do Mac ter um gabarito.
"""

from __future__ import annotations

import hashlib
import os
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from .formats.archives import ArchiveSet, group_archives, match_archive
from .formats.pkg import DEFAULT_PS5_PREFIXES
from .identify import Identified, NeedsAttention, identify, is_auxiliary

DOWNLOAD_MARKERS = (".part", ".crdownload", ".download", ".tmp")
SKIP_DIRS = {"_originais"}
MAX_DEPTH = 2
HASH_CHUNK = 1 << 20


def iso_utc(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


@dataclass
class FileEntry:
    index: int
    path: str  # relativo à raiz do item, com "/"
    abs_path: Path
    size: int
    mtime_ns: int
    sha256: str | None = None


@dataclass
class Item:
    id: str
    revision: str
    rel: str  # caminho relativo à biblioteca
    abs_path: Path
    ident: Identified
    files: list[FileEntry]
    added_ts: float
    warnings: list[str] = field(default_factory=list)

    @property
    def size(self) -> int:
        return sum(f.size for f in self.files)

    @property
    def platform(self) -> str:
        return self.ident.platform

    def has_icon(self) -> bool:
        return self.ident.icon_bytes is not None or (
            self.ident.icon_path is not None and self.ident.icon_path.is_file()
        )

    def summary(self) -> dict:
        i = self.ident
        return {
            "id": self.id,
            "revision": self.revision,
            "platform": i.platform,
            "kind": i.kind,
            "content": i.content,
            "title": i.title or self.abs_path.name,
            "title_id": i.title_id,
            "content_id": i.content_id,
            "version": i.version,
            "required_firmware": i.required_firmware,
            "size": self.size,
            "file_count": len(self.files),
            "parent_title_id": i.parent_title_id,
            "warnings": list(dict.fromkeys(i.warnings + self.warnings)),
            "added_at": iso_utc(self.added_ts),
            "icon": f"/api/v1/items/{self.id}/icon" if self.has_icon() else None,
        }

    def detail(self) -> dict:
        d = self.summary()
        d["files"] = [
            {"index": f.index, "path": f.path, "size": f.size, "sha256": f.sha256} for f in self.files
        ]
        return d


def item_id(platform: str, rel: str) -> str:
    return hashlib.sha256(f"{platform}:{rel}".encode("utf-8")).hexdigest()[:16]


def compute_revision(files: list[FileEntry]) -> str:
    h = hashlib.sha256()
    for f in files:
        h.update(f"{f.path}\0{f.size}\0{f.mtime_ns}\n".encode("utf-8"))
    return h.hexdigest()[:8]


def is_finder_junk(name: str) -> bool:
    """Arquivos que o Finder cria sozinho (.DS_Store, ._*): nunca fazem parte de um item."""
    return name == ".DS_Store" or name.startswith("._")


def _list_files(path: Path) -> list[tuple[str, Path, os.stat_result]]:
    if path.is_file():
        return [(path.name, path, path.stat())]
    out = []
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for n in filenames:
            if is_finder_junk(n):
                continue
            p = Path(dirpath) / n
            rel = p.relative_to(path).as_posix()
            out.append((rel, p, p.stat()))
    out.sort(key=lambda t: t[0].encode("utf-8"))
    return out


def _has_download_marker(path: Path) -> bool:
    if path.is_file():
        return any(path.with_name(path.name + m).exists() for m in DOWNLOAD_MARKERS)
    for _, _, filenames in os.walk(path):
        if any(n.lower().endswith(DOWNLOAD_MARKERS) for n in filenames):
            return True
    return False


class Library:
    def __init__(self, root: Path, stable_seconds: float = 10.0, ps5_prefixes=DEFAULT_PS5_PREFIXES,
                 clock=time.time):
        self.root = Path(root)
        self.stable_seconds = stable_seconds
        self.ps5_prefixes = ps5_prefixes
        self.clock = clock
        self._lock = threading.RLock()
        self._items: dict[str, Item] = {}
        self.attention: list[NeedsAttention] = []
        self.archives: list[ArchiveSet] = []
        self.pending: list[Path] = []
        self._hash_cache: dict[tuple[str, int, int], str] = {}
        self.generated_at = iso_utc(self.clock())

    # ---------- varredura ----------

    def scan(self) -> None:
        items: dict[str, Item] = {}
        attention: list[NeedsAttention] = []
        archive_paths: list[Path] = []
        pending: list[Path] = []
        now = self.clock()

        def visit(directory: Path, depth: int) -> None:
            try:
                entries = sorted(directory.iterdir(), key=lambda p: p.name)
            except OSError:
                return
            for p in entries:
                name = p.name
                if name.startswith(".") or is_auxiliary(name) or name in SKIP_DIRS:
                    continue
                if p.is_file() and match_archive(name):
                    archive_paths.append(p)
                    continue
                if p.is_file() and name.lower().endswith(DOWNLOAD_MARKERS):
                    continue
                result = identify(p, self.ps5_prefixes)
                if result is None:
                    if p.is_dir() and depth < MAX_DEPTH:
                        visit(p, depth + 1)
                    continue
                if not self._is_stable(p, now):
                    pending.append(p)
                    continue
                if isinstance(result, NeedsAttention):
                    attention.append(result)
                    continue
                item = self._make_item(p, result)
                if item is not None:
                    items[item.id] = item

        visit(self.root, 1)
        self._mark_duplicates(items)
        with self._lock:
            old_ids = set(self._items)
            old_revs = {k: v.revision for k, v in self._items.items()}
            self._items = items
            self.attention = attention
            self.archives = group_archives(archive_paths)
            self.pending = pending
            if set(items) != old_ids or any(old_revs.get(k) != v.revision for k, v in items.items()):
                self.generated_at = iso_utc(now)

    def _is_stable(self, path: Path, now: float) -> bool:
        if _has_download_marker(path):
            return False
        if self.stable_seconds <= 0:
            return True
        try:
            newest = max((st.st_mtime for _, _, st in _list_files(path)), default=path.stat().st_mtime)
        except OSError:
            return False
        return now - newest >= self.stable_seconds

    def _make_item(self, path: Path, ident: Identified) -> Item | None:
        try:
            listed = _list_files(path)
        except OSError:
            return None
        files = []
        for idx, (rel, abs_p, st) in enumerate(listed):
            if path.is_dir() and is_auxiliary(abs_p.name):
                continue
            files.append(FileEntry(len(files), rel, abs_p, st.st_size, st.st_mtime_ns))
        for f in files:
            f.sha256 = self._hash_cache.get((str(f.abs_path), f.size, f.mtime_ns))
        rel = path.relative_to(self.root).as_posix()
        added = max((st.st_mtime for _, _, st in listed), default=path.stat().st_mtime)
        return Item(item_id(ident.platform, rel), compute_revision(files), rel, path, ident, files, added)

    @staticmethod
    def _mark_duplicates(items: dict[str, Item]) -> None:
        groups: dict[tuple, list[Item]] = {}
        for it in items.values():
            i = it.ident
            if not i.title_id:
                continue
            groups.setdefault((i.platform, i.title_id, i.content, i.version), []).append(it)
        for group in groups.values():
            if len(group) < 2:
                continue
            group.sort(key=lambda it: (it.added_ts, it.rel))
            for it in group[1:]:
                it.warnings.append("duplicate_title_id")

    # ---------- consultas ----------

    def items(self, platform: str | None = None) -> list[Item]:
        with self._lock:
            out = [it for it in self._items.values() if platform is None or it.platform == platform]
        out.sort(key=lambda it: ((it.ident.title or "").casefold(), it.rel))
        return out

    def get(self, item_id_: str) -> Item | None:
        with self._lock:
            return self._items.get(item_id_)

    def pending_ids(self) -> set[str]:
        """Ids de itens que existem mas ainda não estão prontos (para 409)."""
        out = set()
        for p in self.pending:
            ident = identify(p, self.ps5_prefixes)
            if isinstance(ident, Identified):
                out.add(item_id(ident.platform, p.relative_to(self.root).as_posix()))
        return out

    # ---------- hashes ----------

    def hash_pending(self, stop: threading.Event | None = None) -> int:
        """Calcula sha256 dos arquivos que ainda não têm. Retorna quantos calculou."""
        done = 0
        for it in self.items():
            for f in it.files:
                if stop is not None and stop.is_set():
                    return done
                if f.sha256 is not None:
                    continue
                key = (str(f.abs_path), f.size, f.mtime_ns)
                if key in self._hash_cache:
                    f.sha256 = self._hash_cache[key]
                    continue
                h = hashlib.sha256()
                try:
                    with f.abs_path.open("rb") as fh:
                        while chunk := fh.read(HASH_CHUNK):
                            h.update(chunk)
                except OSError:
                    continue
                st = f.abs_path.stat()
                if (st.st_size, st.st_mtime_ns) != (f.size, f.mtime_ns):
                    continue  # mudou durante o cálculo; fica para a próxima varredura
                f.sha256 = h.hexdigest()
                self._hash_cache[key] = f.sha256
                done += 1
        return done
