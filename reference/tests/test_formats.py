import io

import pytest

from conftest import PNG, make_nsp, make_ps4_pkg
from ponte_ref.exfat_plan import CLUSTER_LARGE, CLUSTER_SMALL, MIB, plan_exfat
from ponte_ref.formats import archives, pfs0, pkg, switch_tid
from ponte_ref.formats.ps5_param import parse_param_json
from ponte_ref.formats.sfo import SfoError, build_sfo, parse_sfo
from ponte_ref.versions import (compare_firmware, compare_versions, firmware_from_bcd,
                                firmware_from_hex_text)


# ---------- versões ----------

def test_firmware_bcd():
    assert firmware_from_bcd(0x05050000, 4) == "5.05"
    assert firmware_from_bcd(0x10010000, 4) == "10.01"
    assert firmware_from_hex_text("0x0403000000000000") == "4.03"
    assert firmware_from_hex_text("0x1360000000000000") == "13.60"
    assert firmware_from_hex_text("lixo") is None
    assert firmware_from_bcd(0, 4) is None


def test_compare_versions():
    assert compare_versions("01.000.000", "1.0.0", "ps5") == 0
    assert compare_versions("01.05", "01.4", "ps5") == 1
    assert compare_versions("01.02", "01.10", "ps5") == -1
    assert compare_versions("65536", "131072", "switch") == -1
    assert compare_versions(None, "1.0", "ps5") == 0
    assert compare_firmware("13.60", "9.60") == 1
    assert compare_firmware("4.03", "4.03") == 0


# ---------- param.sfo ----------

def test_sfo_roundtrip():
    data = build_sfo({"TITLE": "Ação", "CATEGORY": "gp", "SYSTEM_VER": 0x09000000})
    assert parse_sfo(data) == {"TITLE": "Ação", "CATEGORY": "gp", "SYSTEM_VER": 0x09000000}


def test_sfo_rejects_garbage():
    with pytest.raises(SfoError):
        parse_sfo(b"not an sfo at all....")


# ---------- .pkg ----------

def test_pkg_ps4(tmp_path):
    p = make_ps4_pkg(tmp_path / "x.pkg", category="gp", version="01.05")
    with p.open("rb") as f:
        info = pkg.read_pkg(f, p.stat().st_size)
    assert info.title_id == "CUSA00001"
    assert info.platform_kind == "pkg_ps4"
    assert info.sfo["CATEGORY"] == "gp"
    assert info.icon == PNG


def test_pkg_ps5_by_prefix(tmp_path):
    data = pkg.build_pkg("UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP")
    info = pkg.read_pkg(io.BytesIO(data), len(data))
    assert info.platform_kind == "pkg_ps5"
    assert info.sfo is None


def test_pkg_bad_magic():
    data = b"\x00" * 0x200
    with pytest.raises(pkg.PkgError):
        pkg.read_pkg(io.BytesIO(data), len(data))


def test_pkg_entry_out_of_bounds_is_ignored():
    data = bytearray(pkg.build_pkg("UP0000-CUSA00001_00-ABCDEFGHIJKLMNOP", build_sfo({"CATEGORY": "gd"})))
    # tamanho da entrada do param.sfo apontando para fora do arquivo
    import struct
    struct.pack_into(">I", data, 0x1000 + 20, 10_000_000)
    info = pkg.read_pkg(io.BytesIO(bytes(data)), len(data))
    assert info.sfo is None


# ---------- param.json ----------

def test_param_json_fallback_language():
    raw = b'{"titleId":"PPSA1","localizedParameters":{"defaultLanguage":"xx","en-US":{"titleName":"Game"}}}'
    meta = parse_param_json(raw)
    assert meta["title"] == "Game"
    assert meta["version"] is None
    assert meta["required_firmware"] is None


def test_param_json_bom():
    assert parse_param_json(b'\xef\xbb\xbf{"titleId":"PPSA00001"}')["title_id"] == "PPSA00001"


# ---------- Switch ----------

@pytest.mark.parametrize("tid,content,parent", [
    ("0100ABCD12340000", "base", None),
    ("0100ABCD12340800", "update", "0100ABCD12340000"),
    ("0100ABCD12341001", "dlc", "0100ABCD12340000"),
    ("0100ABCD12341FFF", "dlc", "0100ABCD12340000"),
    ("0100ABCD12341000", "unknown", None),
    ("0100ABCD12340123", "unknown", None),
])
def test_switch_classify(tid, content, parent):
    assert switch_tid.classify(tid) == (content, parent)


def test_switch_names():
    name = "Jogo Legal [0100ABCD12340800][v131072] (BR).nsp"
    assert switch_tid.tid_from_name(name) == "0100ABCD12340800"
    assert switch_tid.version_from_name(name) == "131072"
    assert switch_tid.clean_title("Jogo Legal [0100ABCD12340800][v131072] (BR)") == "Jogo Legal (BR)"


def test_pfs0_ticket(tmp_path):
    p = make_nsp(tmp_path / "a.nsp")
    with p.open("rb") as f:
        names = [e.name for e in pfs0.list_pfs0(f)]
    assert switch_tid.tids_from_tickets(names) == {"0100ABCD12340000"}


def test_pfs0_bad():
    with pytest.raises(pfs0.Pfs0Error):
        pfs0.list_pfs0(io.BytesIO(b"PFS0" + b"\xff" * 12))


# ---------- compactados ----------

def _names(*names):
    from pathlib import Path
    return [Path("/lib") / n for n in names]


def test_archive_rar_parts_complete():
    sets = archives.group_archives(_names("Jogo.part1.rar", "Jogo.part2.rar", "Jogo.part3.rar"))
    assert len(sets) == 1 and sets[0].key == "Jogo.rar" and sets[0].complete


def test_archive_gap_detected():
    sets = archives.group_archives(_names("Jogo.7z.001", "Jogo.7z.003"))
    assert sets[0].missing == [2]


def test_archive_old_rar():
    sets = archives.group_archives(_names("Jogo.rar", "Jogo.r00", "Jogo.r01"))
    assert len(sets) == 1 and sets[0].complete
    sets = archives.group_archives(_names("Jogo.rar", "Jogo.r01"))
    assert sets[0].missing == [1]


def test_archive_split_zip_and_singles():
    sets = archives.group_archives(_names("A.zip", "A.z01", "A.z02", "B.7z", "C.tar.gz", "leia.txt"))
    assert [s.key for s in sets] == ["A.zip", "B.7z", "C.tar.gz"]
    assert all(s.complete for s in sets)


# ---------- plano do .exfat ----------

def test_exfat_cluster_choice():
    assert plan_exfat([2 * MIB, 2 * MIB], 1).cluster == CLUSTER_LARGE
    assert plan_exfat([1000, 2000], 1).cluster == CLUSTER_SMALL
    assert plan_exfat([1000], 1, force_64k=True).cluster == CLUSTER_LARGE
    assert plan_exfat([], 0).cluster == CLUSTER_SMALL


def test_exfat_size_small_game_uses_minimum_spare():
    # 10 arquivos de 1 KiB, 2 pastas, cluster 32 KiB
    p = plan_exfat([1024] * 10, 2)
    allocated = 10 * 32 * 1024
    clusters = 10
    base = allocated + clusters * 4 + 2 + 12 * 256 + 32 * MIB
    expected = -(-(base + 64 * MIB) // MIB)
    assert p.size_mib == expected


def test_exfat_size_large_game_spare_capped():
    sizes = [1 << 30] * 200  # 200 GiB
    p = plan_exfat(sizes, 10)
    assert p.cluster == CLUSTER_LARGE
    raw = sum(sizes)
    assert p.size_bytes >= raw + 64 * MIB
    assert p.size_bytes <= raw * 1.001 + 600 * MIB  # folga limitada a 512 MiB
