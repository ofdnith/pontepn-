import os
import time
from pathlib import Path

from conftest import age, make_nsp, make_ps4_pkg, make_ps5_folder
from ponte_ref.library import Library, item_id
from ponte_ref.profiles import Profile, load_profiles, select_profile
from ponte_ref.state import annotate

REPO = Path(__file__).resolve().parents[2]


def build_library(root: Path) -> None:
    make_ps5_folder(root)
    sub = root / "Switch"
    sub.mkdir()
    make_nsp(sub / "Jogo [0100ABCD12340000][v0].nsp")
    make_nsp(sub / "Jogo [0100ABCD12340800][v65536].nsp", tid="0100ABCD12340800")
    make_ps4_pkg(root / "velho.pkg")
    (root / "PPSA09999.exfat").write_bytes(b"\x00" * 64)
    (root / "Jogo.part1.rar").write_bytes(b"r")
    (root / "Jogo.part2.rar").write_bytes(b"r")
    (root / "leia.txt").write_text("oi")
    age(root)


def test_scan_finds_everything(lib_root):
    build_library(lib_root)
    lib = Library(lib_root)
    lib.scan()
    kinds = sorted((it.platform, it.ident.kind) for it in lib.items())
    assert kinds == [("ps5", "exfat"), ("ps5", "folder"), ("ps5", "pkg_ps4"),
                     ("switch", "nsp"), ("switch", "nsp")]
    assert [a.key for a in lib.archives] == ["Jogo.rar"]
    assert lib.attention == [] and lib.pending == []


def test_ids_are_stable_and_revision_tracks_content(lib_root):
    build_library(lib_root)
    lib = Library(lib_root)
    lib.scan()
    folder = next(it for it in lib.items() if it.ident.kind == "folder")
    assert folder.id == item_id("ps5", "PPSA01234")
    rev = folder.revision

    lib.scan()
    assert lib.get(folder.id).revision == rev

    (lib_root / "PPSA01234" / "eboot.bin").write_bytes(b"X" * 10)
    age(lib_root)
    lib.scan()
    assert lib.get(folder.id).revision != rev


def test_folder_files_sorted_and_relative(lib_root):
    make_ps5_folder(lib_root, extra={"Z/z.bin": b"z", "B.bin": b"b"})
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    files = [f["path"] for f in lib.items()[0].detail()["files"]]
    assert files == sorted(files, key=lambda s: s.encode())
    assert "sce_sys/param.json" in files
    assert all(not p.startswith("/") and ".." not in p for p in files)
    assert [f["index"] for f in lib.items()[0].detail()["files"]] == list(range(len(files)))


def test_unstable_and_partial_downloads_wait(lib_root):
    make_ps5_folder(lib_root)  # recém-criada: instável
    nsp = make_nsp(lib_root / "A [0100ABCD12340000].nsp")
    age(nsp)
    (lib_root / "A [0100ABCD12340000].nsp.crdownload").write_bytes(b"")
    lib = Library(lib_root, stable_seconds=10)
    lib.scan()
    assert lib.items() == []
    assert len(lib.pending) == 2


def test_attention_for_bad_items(lib_root):
    make_ps5_folder(lib_root, eboot=False)
    (lib_root / "ruim.pkg").write_bytes(b"x" * 200)
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    assert lib.items() == []
    assert sorted(a.path.name for a in lib.attention) == ["PPSA01234", "ruim.pkg"]


def test_depth_limit_and_game_folder_not_descended(lib_root):
    deep = lib_root / "a" / "b"
    deep.mkdir(parents=True)
    make_nsp(deep / "Fundo [0100ABCD12340000].nsp")
    game = make_ps5_folder(lib_root / "a")
    make_nsp(game / "dentro [0100ABCD99990000].nsp")
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    assert [it.rel for it in lib.items()] == ["a/PPSA01234"]


def test_duplicates_marked_on_newest(lib_root):
    a = make_nsp(lib_root / "A [0100ABCD12340000][v0].nsp")
    b = make_nsp(lib_root / "B [0100ABCD12340000][v0].nsp")
    age(a, 7200)
    age(b, 3600)
    lib = Library(lib_root)
    lib.scan()
    marks = {it.abs_path.name: it.summary()["warnings"] for it in lib.items()}
    assert "duplicate_title_id" not in marks[a.name]
    assert "duplicate_title_id" in marks[b.name]


def test_hashes(lib_root):
    make_ps4_pkg(lib_root / "p.pkg")
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    assert lib.items()[0].files[0].sha256 is None
    assert lib.hash_pending() == 1
    import hashlib
    assert lib.items()[0].files[0].sha256 == hashlib.sha256((lib_root / "p.pkg").read_bytes()).hexdigest()
    lib.scan()  # o cache sobrevive à nova varredura
    assert lib.items()[0].files[0].sha256 is not None


def test_generated_at_changes_only_on_change(lib_root):
    make_ps4_pkg(lib_root / "p.pkg")
    age(lib_root)
    clock = [1_800_000_000.0]
    lib = Library(lib_root, clock=lambda: clock[0])
    lib.scan()
    first = lib.generated_at
    clock[0] += 100
    lib.scan()
    assert lib.generated_at == first
    make_ps4_pkg(lib_root / "q.pkg", title_id="CUSA00002")
    age(lib_root)
    clock[0] += 100
    lib.scan()
    assert lib.generated_at != first


# ---------- perfis e estado ----------

def test_profile_selection():
    profiles = load_profiles(REPO / "profiles")
    p, guessed = select_profile(profiles, "ps5", "13.60")
    assert p.firmware == "13.60" and not guessed
    p, guessed = select_profile(profiles, "ps5", "14.00")
    assert p.firmware == "13.60" and not guessed
    p, guessed = select_profile(profiles, "ps5", "9.60")
    assert p.firmware == "13.60" and guessed
    assert select_profile(profiles, "ps5", None) == (None, False)
    assert select_profile(profiles, "switch", "19.0") == (None, False)


def _console(installed, firmware="13.60"):
    return {"platform": "ps5", "firmware": firmware, "storages": [], "installed": installed}


def test_states(lib_root):
    make_ps4_pkg(lib_root / "base.pkg", app_ver="01.00")
    make_ps4_pkg(lib_root / "upd.pkg", category="gp", version="01.07")
    make_ps4_pkg(lib_root / "novo.pkg", title_id="CUSA00077")
    make_ps4_pkg(lib_root / "exige.pkg", title_id="CUSA00088", system_ver=0x14000000)
    from ponte_ref.formats.pkg import build_pkg
    (lib_root / "ps5.pkg").write_bytes(build_pkg("UP0000-PPSA00001_00-ABCDEFGHIJKLMNOP"))
    (lib_root / "PPSA00002.ffpfs").write_bytes(b"\x00")
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    profiles = load_profiles(REPO / "profiles")
    installed = [
        {"title_id": "CUSA00001", "version": "01.00", "content": "base"},
        {"title_id": "CUSA00001", "version": "01.05", "content": "update"},
    ]
    out = {d["title_id"] + ":" + d["content"]: d for d in annotate(lib.items("ps5"), _console(installed), profiles)}
    assert out["CUSA00001:base"]["state"] == "installed"
    assert out["CUSA00001:update"]["state"] == "update_available"
    assert out["CUSA00077:base"]["state"] == "new"
    assert out["CUSA00088:base"]["state"] == "incompatible"
    assert "14.00" in out["CUSA00088:base"]["state_reason"]
    assert out["PPSA00001:unknown"]["state"] == "incompatible"
    assert ".pkg de PS5" in out["PPSA00001:unknown"]["state_reason"]
    assert out["PPSA00002:base"]["state"] == "new"
    assert "experimental_format" in out["PPSA00002:base"]["warnings"]


def test_state_without_profile_or_firmware(lib_root):
    make_ps4_pkg(lib_root / "base.pkg")
    age(lib_root)
    lib = Library(lib_root)
    lib.scan()
    d = annotate(lib.items(), {"platform": "ps5", "installed": []}, [])[0]
    assert d["state"] == "new" and d["state_reason"] is None
