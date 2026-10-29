"""Identificador de formatos (SPEC §1 e §7.2).

`identify(path)` olha um arquivo ou pasta da biblioteca e devolve:
  - `Identified`: plataforma, tipo e metadados;
  - `NeedsAttention`: parece um jogo mas não dá para identificar com segurança;
  - `None`: não é um item (arquivo qualquer, pasta comum).

Compactados são tratados à parte (`formats.archives`), porque um conjunto
tem várias partes e não é um item do catálogo.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from .formats import pfs0, pkg, switch_tid
from .formats.ps5_param import parse_param_json
from .formats.sfo import SfoError, parse_sfo
from .versions import firmware_from_bcd

PS5_IMAGE_KINDS = {".exfat": "exfat", ".ffpkg": "ffpkg", ".ffpfs": "ffpfs", ".ffpfsc": "ffpfsc"}
SWITCH_KINDS = {".nsp": "nsp", ".nsz": "nsz", ".xci": "xci", ".xcz": "xcz"}
PS_TITLE_ID_RE = re.compile(r"(?<![A-Z0-9])([A-Z]{4}\d{5})(?!\d)")

SIDECAR_SUFFIX = ".ponte.json"
SIDECAR_ICON_SUFFIX = ".ponte.png"
TMP_SUFFIX = ".ponte-tmp"

KINDS = {
    "ps5": {"folder", "exfat", "ffpkg", "ffpfs", "ffpfsc", "pkg_ps4", "pkg_ps5", "folder_ps4"},
    "switch": {"nsp", "nsz", "xci", "xcz"},
}
CONTENTS = {"base", "update", "dlc", "unknown"}
SFO_CATEGORY = {"gd": "base", "gp": "update", "ac": "dlc"}


@dataclass
class Identified:
    platform: str
    kind: str
    content: str = "unknown"
    title: str | None = None
    title_id: str | None = None
    content_id: str | None = None
    version: str | None = None
    required_firmware: str | None = None
    parent_title_id: str | None = None
    warnings: list[str] = field(default_factory=list)
    icon_bytes: bytes | None = None
    icon_path: Path | None = None


@dataclass
class NeedsAttention:
    path: Path
    reason: str


SIDECAR_FIELDS = ("platform", "kind", "content", "title", "title_id", "content_id", "version",
                  "required_firmware", "parent_title_id")


def sidecar_path(item: Path) -> Path:
    return item.with_name(item.name + SIDECAR_SUFFIX)


def sidecar_icon_path(item: Path) -> Path:
    return item.with_name(item.name + SIDECAR_ICON_SUFFIX)


def is_auxiliary(name: str) -> bool:
    """Arquivos do próprio Ponte, que nunca são itens."""
    return name.endswith((SIDECAR_SUFFIX, SIDECAR_ICON_SUFFIX, TMP_SUFFIX))


def _read_sidecar(item: Path) -> dict | None:
    sp = sidecar_path(item)
    if not sp.is_file():
        return None
    try:
        data = json.loads(sp.read_text("utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    return {k: data[k] for k in SIDECAR_FIELDS if k in data}


def _apply_sidecar(item: Path, auto: Identified | NeedsAttention | None) -> Identified | NeedsAttention | None:
    side = _read_sidecar(item)
    icon = sidecar_icon_path(item)
    if side is None:
        if isinstance(auto, Identified) and icon.is_file() and auto.icon_path is None and auto.icon_bytes is None:
            auto.icon_path = icon
        return auto

    if isinstance(auto, Identified):
        result = auto
    else:
        platform, kind = side.get("platform"), side.get("kind")
        if platform not in KINDS or kind not in KINDS[platform]:
            return auto if auto is not None else NeedsAttention(item, "arquivo .ponte.json sem platform/kind válidos")
        result = Identified(platform=platform, kind=kind)

    for k, v in side.items():
        if k == "platform" and (v not in KINDS):
            continue
        if k == "kind" and v not in KINDS.get(side.get("platform", result.platform), set()):
            continue
        if k == "content" and v not in CONTENTS:
            continue
        setattr(result, k, v)
    # metadados vieram de uma fonte confiável: o aviso de "só pelo nome" não vale mais
    result.warnings = [w for w in result.warnings if w != "metadata_from_filename"]
    if icon.is_file():
        result.icon_path = icon
        result.icon_bytes = None
    return result


def _sfo_fields(sfo: dict) -> dict:
    content = SFO_CATEGORY.get(str(sfo.get("CATEGORY", "")).strip(), "unknown")
    sys_ver = sfo.get("SYSTEM_VER")
    fw = firmware_from_bcd(sys_ver, 4) if isinstance(sys_ver, int) else None
    version = sfo.get("APP_VER") if content == "base" else sfo.get("VERSION")
    version = version or sfo.get("APP_VER") or sfo.get("VERSION")
    return {
        "content": content,
        "title": sfo.get("TITLE") or None,
        "title_id": sfo.get("TITLE_ID") or None,
        "content_id": sfo.get("CONTENT_ID") or None,
        "version": version or None,
        "required_firmware": fw,
    }


def _identify_folder(path: Path) -> Identified | NeedsAttention | None:
    pj = path / "sce_sys" / "param.json"
    ps = path / "sce_sys" / "param.sfo"
    icon = path / "sce_sys" / "icon0.png"
    if pj.is_file():
        if not (path / "eboot.bin").is_file():
            return NeedsAttention(path, "pasta de PS5 sem eboot.bin na raiz")
        try:
            meta = parse_param_json(pj.read_bytes())
        except (OSError, ValueError) as e:
            return NeedsAttention(path, f"param.json ilegível: {e}")
        return Identified(
            platform="ps5", kind="folder", content="base",
            title=meta["title"] or path.name, title_id=meta["title_id"], content_id=meta["content_id"],
            version=meta["version"], required_firmware=meta["required_firmware"],
            icon_path=icon if icon.is_file() else None,
        )
    if ps.is_file():
        try:
            f = _sfo_fields(parse_sfo(ps.read_bytes()))
        except (OSError, SfoError) as e:
            return NeedsAttention(path, f"param.sfo ilegível: {e}")
        parent = f["title_id"] if f["content"] in ("update", "dlc") else None
        return Identified(
            platform="ps5", kind="folder_ps4", content=f["content"], title=f["title"] or path.name,
            title_id=f["title_id"], content_id=f["content_id"], version=f["version"],
            required_firmware=f["required_firmware"], parent_title_id=parent,
            icon_path=icon if icon.is_file() else None,
        )
    return None


def _identify_ps5_image(path: Path, kind: str) -> Identified:
    stem = path.name[: -len(path.suffix)]
    m = PS_TITLE_ID_RE.search(stem.upper())
    return Identified(
        platform="ps5", kind=kind, content="base", title=stem,
        title_id=m.group(1) if m else None, warnings=["metadata_from_filename"],
    )


def _identify_pkg(path: Path, ps5_prefixes) -> Identified | NeedsAttention:
    try:
        size = path.stat().st_size
        with path.open("rb") as f:
            info = pkg.read_pkg(f, size, ps5_prefixes)
    except (OSError, pkg.PkgError) as e:
        return NeedsAttention(path, f".pkg não reconhecido: {e}")
    stem = path.name[: -len(path.suffix)]
    result = Identified(platform="ps5", kind=info.platform_kind, title=stem,
                        title_id=info.title_id, content_id=info.content_id, icon_bytes=info.icon)
    if info.sfo:
        f = _sfo_fields(info.sfo)
        result.content = f["content"]
        result.title = f["title"] or stem
        result.version = f["version"]
        result.required_firmware = f["required_firmware"]
        if f["content"] in ("update", "dlc"):
            result.parent_title_id = info.title_id
    else:
        result.warnings.append("metadata_from_filename")
    return result


def _identify_switch(path: Path, kind: str) -> Identified:
    name = path.name
    stem = name[: -len(path.suffix)]
    tid = None
    warnings = []
    if kind in ("nsp", "nsz"):
        try:
            with path.open("rb") as f:
                names = [e.name for e in pfs0.list_pfs0(f)]
            tids = switch_tid.tids_from_tickets(names)
            if len(tids) == 1:
                tid = tids.pop()
        except (OSError, pfs0.Pfs0Error):
            pass
    if tid is None:
        tid = switch_tid.tid_from_name(name)
        warnings.append("metadata_from_filename")
    content, parent = switch_tid.classify(tid) if tid else ("unknown", None)
    return Identified(
        platform="switch", kind=kind, content=content, title=switch_tid.clean_title(stem),
        title_id=tid, version=switch_tid.version_from_name(name), parent_title_id=parent,
        warnings=warnings,
    )


def identify(path: Path, ps5_prefixes=pkg.DEFAULT_PS5_PREFIXES) -> Identified | NeedsAttention | None:
    if is_auxiliary(path.name):
        return None
    auto: Identified | NeedsAttention | None
    if path.is_dir():
        auto = _identify_folder(path)
    elif path.is_file():
        ext = path.suffix.lower()
        if ext in PS5_IMAGE_KINDS:
            auto = _identify_ps5_image(path, PS5_IMAGE_KINDS[ext])
        elif ext == ".pkg":
            auto = _identify_pkg(path, ps5_prefixes)
        elif ext in SWITCH_KINDS:
            auto = _identify_switch(path, SWITCH_KINDS[ext])
        else:
            auto = None
    else:
        return None
    if auto is None and not sidecar_path(path).is_file():
        return None
    result = _apply_sidecar(path, auto)
    if isinstance(result, Identified) and not result.title:
        result.title = path.name
    return result


def to_sidecar(ident: Identified) -> dict:
    """Conteúdo do .ponte.json para um item identificado (SPEC §1.6)."""
    return {k: getattr(ident, k) for k in SIDECAR_FIELDS}


__all__ = ["Identified", "NeedsAttention", "identify", "to_sidecar", "sidecar_path",
           "sidecar_icon_path", "is_auxiliary"]
