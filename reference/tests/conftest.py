import json
import os
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "reference"))
sys.path.insert(0, str(REPO / "tools"))

from ponte_ref.formats.pfs0 import build_pfs0  # noqa: E402
from ponte_ref.formats.pkg import build_pkg  # noqa: E402
from ponte_ref.formats.sfo import build_sfo  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24


def make_ps5_folder(parent: Path, name="PPSA01234", title="Jogo Cinco", eboot=True, extra=None) -> Path:
    root = parent / name
    (root / "sce_sys").mkdir(parents=True)
    (root / "sce_sys" / "param.json").write_text(json.dumps({
        "titleId": "PPSA01234",
        "contentId": "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP",
        "contentVersion": "01.000.000",
        "requiredSystemSoftwareVersion": "0x0403000000000000",
        "localizedParameters": {"defaultLanguage": "pt-BR", "pt-BR": {"titleName": title}},
    }))
    (root / "sce_sys" / "icon0.png").write_bytes(PNG)
    if eboot:
        (root / "eboot.bin").write_bytes(b"E" * 5000)
    (root / "data").mkdir()
    (root / "data" / "a.bin").write_bytes(b"A" * 1234)
    for rel, content in (extra or {}).items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(content)
    return root


def make_ps4_pkg(path: Path, title_id="CUSA00001", category="gd", app_ver="01.00", version="01.00",
                 system_ver=0x05050000, title="Jogo Quatro", icon=True, body=b"B" * 4096) -> Path:
    sfo = build_sfo({"CATEGORY": category, "TITLE": title, "TITLE_ID": title_id,
                     "APP_VER": app_ver, "VERSION": version, "SYSTEM_VER": system_ver,
                     "CONTENT_ID": f"UP0000-{title_id}_00-ABCDEFGHIJKLMNOP"})
    data = build_pkg(f"UP0000-{title_id}_00-ABCDEFGHIJKLMNOP", sfo, PNG if icon else None, body)
    path.write_bytes(data)
    return path


def make_nsp(path: Path, tid="0100ABCD12340000", with_ticket=True) -> Path:
    files = {"abcdef0123456789abcdef0123456789.nca": b"N" * 3000}
    if with_ticket:
        files[f"{tid.lower()}0000000000000000.tik"] = b"T" * 0x2C0
    path.write_bytes(build_pfs0(files))
    return path


def age(path: Path, seconds: float = 3600) -> None:
    """Deixa o arquivo/pasta 'velho', para passar na regra de estabilidade."""
    t = time.time() - seconds
    targets = [path] if path.is_file() else [Path(d) / f for d, _, fs in os.walk(path) for f in fs]
    for p in targets:
        os.utime(p, (t, t))


@pytest.fixture
def lib_root(tmp_path):
    root = tmp_path / "biblioteca"
    root.mkdir()
    return root
