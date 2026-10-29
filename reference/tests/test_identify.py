import json

from conftest import PNG, make_nsp, make_ps4_pkg, make_ps5_folder
from ponte_ref.formats.pfs0 import build_pfs0
from ponte_ref.formats.pkg import build_pkg
from ponte_ref.formats.sfo import build_sfo
from ponte_ref.identify import Identified, NeedsAttention, identify, sidecar_path, to_sidecar


def test_ps5_folder(tmp_path):
    r = identify(make_ps5_folder(tmp_path))
    assert isinstance(r, Identified)
    assert (r.platform, r.kind, r.content) == ("ps5", "folder", "base")
    assert r.title == "Jogo Cinco"
    assert r.title_id == "PPSA01234"
    assert r.version == "01.000.000"
    assert r.required_firmware == "4.03"
    assert r.icon_path.name == "icon0.png"
    assert r.warnings == []


def test_ps5_folder_without_eboot(tmp_path):
    r = identify(make_ps5_folder(tmp_path, eboot=False))
    assert isinstance(r, NeedsAttention)
    assert "eboot.bin" in r.reason


def test_ps5_folder_broken_json(tmp_path):
    root = make_ps5_folder(tmp_path)
    (root / "sce_sys" / "param.json").write_text("{quebrado")
    assert isinstance(identify(root), NeedsAttention)


def test_ps4_folder(tmp_path):
    root = tmp_path / "CUSA00002"
    (root / "sce_sys").mkdir(parents=True)
    (root / "sce_sys" / "param.sfo").write_bytes(build_sfo({
        "CATEGORY": "gd", "TITLE": "Velho", "TITLE_ID": "CUSA00002", "APP_VER": "01.02",
        "SYSTEM_VER": 0x04550000}))
    r = identify(root)
    assert (r.kind, r.content, r.title, r.version, r.required_firmware) == (
        "folder_ps4", "base", "Velho", "01.02", "4.55")


def test_plain_folder_is_not_an_item(tmp_path):
    (tmp_path / "Downloads").mkdir()
    assert identify(tmp_path / "Downloads") is None


def test_random_file_is_not_an_item(tmp_path):
    (tmp_path / "leia.txt").write_text("oi")
    assert identify(tmp_path / "leia.txt") is None


def test_ps5_images(tmp_path):
    for ext, kind in [(".exfat", "exfat"), (".ffpkg", "ffpkg"), (".ffpfs", "ffpfs"), (".FFPFSC", "ffpfsc")]:
        p = tmp_path / f"Jogo Bom PPSA05678{ext}"
        p.write_bytes(b"\x00" * 100)
        r = identify(p)
        assert (r.platform, r.kind, r.title_id) == ("ps5", kind, "PPSA05678")
        assert r.title == f"Jogo Bom PPSA05678"
        assert "metadata_from_filename" in r.warnings


def test_image_without_title_id(tmp_path):
    p = tmp_path / "qualquer.exfat"
    p.write_bytes(b"\x00")
    r = identify(p)
    assert r.title_id is None and r.title == "qualquer"


def test_pkg_ps4_base_update_dlc(tmp_path):
    base = identify(make_ps4_pkg(tmp_path / "b.pkg", category="gd", app_ver="01.00"))
    upd = identify(make_ps4_pkg(tmp_path / "u.pkg", category="gp", app_ver="01.00", version="01.07"))
    dlc = identify(make_ps4_pkg(tmp_path / "d.pkg", category="ac"))
    assert (base.kind, base.content, base.version, base.parent_title_id) == ("pkg_ps4", "base", "01.00", None)
    assert (upd.content, upd.version, upd.parent_title_id) == ("update", "01.07", "CUSA00001")
    assert dlc.content == "dlc"
    assert base.required_firmware == "5.05"
    assert base.icon_bytes == PNG
    assert base.title == "Jogo Quatro"


def test_pkg_ps5(tmp_path):
    p = tmp_path / "jogo.pkg"
    p.write_bytes(build_pkg("UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP"))
    r = identify(p)
    assert (r.kind, r.title_id, r.title) == ("pkg_ps5", "PPSA01234", "jogo")
    assert "metadata_from_filename" in r.warnings


def test_pkg_garbage_needs_attention(tmp_path):
    p = tmp_path / "x.pkg"
    p.write_bytes(b"lixo" * 100)
    assert isinstance(identify(p), NeedsAttention)


def test_nsp_ticket_wins_over_name(tmp_path):
    p = make_nsp(tmp_path / "Nome Errado [0100FFFF00000000][v0].nsp", tid="0100ABCD12340800")
    r = identify(p)
    assert (r.platform, r.kind, r.title_id, r.content) == ("switch", "nsp", "0100ABCD12340800", "update")
    assert r.parent_title_id == "0100ABCD12340000"
    assert r.version == "0"
    assert r.title == "Nome Errado"
    assert "metadata_from_filename" not in r.warnings


def test_nsp_without_ticket_uses_name(tmp_path):
    p = make_nsp(tmp_path / "Jogo [0100ABCD12341003][v65536].nsz", with_ticket=False)
    r = identify(p)
    assert (r.kind, r.content, r.title_id, r.version) == ("nsz", "dlc", "0100ABCD12341003", "65536")
    assert "metadata_from_filename" in r.warnings


def test_nsp_two_tickets_falls_back_to_name(tmp_path):
    p = tmp_path / "Pacote [0100ABCD12340000].nsp"
    p.write_bytes(build_pfs0({"0100abcd123400000000000000000000.tik": b"t",
                              "0100abcd123408000000000000000000.tik": b"t"}))
    assert identify(p).title_id == "0100ABCD12340000"


def test_xci_by_name(tmp_path):
    p = tmp_path / "Cartucho [0100AAAA00000000][v0].xci"
    p.write_bytes(b"\x00" * 512)
    r = identify(p)
    assert (r.kind, r.content, r.title_id) == ("xci", "base", "0100AAAA00000000")


def test_switch_without_any_id(tmp_path):
    p = tmp_path / "sem id.xcz"
    p.write_bytes(b"\x00")
    r = identify(p)
    assert (r.title_id, r.content) == (None, "unknown")


def test_sidecar_overrides_and_rescues(tmp_path):
    # imagem sem Title ID no nome, completada pelo arquivo lateral
    img = tmp_path / "PPSA01234.exfat"
    img.write_bytes(b"\x00")
    sidecar_path(img).write_text(json.dumps({"title": "Nome Real", "version": "01.000.000",
                                             "required_firmware": "4.03", "kind": "não-existe"}))
    (tmp_path / "PPSA01234.exfat.ponte.png").write_bytes(PNG)
    r = identify(img)
    assert (r.kind, r.title, r.version, r.required_firmware) == ("exfat", "Nome Real", "01.000.000", "4.03")
    assert r.warnings == []
    assert r.icon_path.name == "PPSA01234.exfat.ponte.png"


def test_sidecar_makes_unknown_file_an_item(tmp_path):
    f = tmp_path / "misterio.bin"
    f.write_bytes(b"\x00")
    sidecar_path(f).write_text(json.dumps({"platform": "ps5", "kind": "ffpkg", "title_id": "PPSA00009"}))
    r = identify(f)
    assert (r.platform, r.kind, r.title_id) == ("ps5", "ffpkg", "PPSA00009")


def test_sidecar_invalid_keeps_attention(tmp_path):
    f = tmp_path / "misterio.bin"
    f.write_bytes(b"\x00")
    sidecar_path(f).write_text(json.dumps({"platform": "xbox"}))
    assert isinstance(identify(f), NeedsAttention)


def test_sidecar_files_are_not_items(tmp_path):
    p = tmp_path / "a.exfat.ponte.json"
    p.write_text("{}")
    assert identify(p) is None


def test_to_sidecar_roundtrip(tmp_path):
    r = identify(make_ps5_folder(tmp_path))
    side = to_sidecar(r)
    assert side["kind"] == "folder" and side["title"] == "Jogo Cinco"
    json.dumps(side)
