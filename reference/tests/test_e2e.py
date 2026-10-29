"""Servidor de referência + cliente falso, de ponta a ponta."""

import hashlib
import http.client
import json
import os
import shutil

import pytest

from conftest import REPO, age, make_nsp, make_ps4_pkg, make_ps5_folder
from fake_client import Client, Interrupted, PonteError, discover, safe_relpath
from ponte_ref.library import Library
from ponte_ref.server import PonteServer, parse_range, ApiError


@pytest.fixture
def server(lib_root, tmp_path):
    make_ps5_folder(lib_root, extra={"data/big.bin": os.urandom(3 * 1024 * 1024 + 17)})
    (lib_root / "PPSA05678.exfat").write_bytes(os.urandom(2 * 1024 * 1024))
    make_ps4_pkg(lib_root / "velho.pkg")
    make_nsp(lib_root / "Jogo [0100ABCD12340000][v0].nsp")
    age(lib_root)
    lib = Library(lib_root, stable_seconds=10)
    srv = PonteServer(lib, REPO / "profiles", tmp_path / "estado", "Mac de teste",
                      host="127.0.0.1", port=0, discovery_host="127.0.0.1", discovery_port=0,
                      rescan_interval=0.2, log=lambda *_: None)
    srv.start()
    yield srv
    srv.stop()


@pytest.fixture
def client(server):
    c = Client("127.0.0.1", server.port)
    c.pair(server.code, "PS5 de teste")
    yield c
    c.close()


def by_kind(catalog, kind):
    return next(it for it in catalog["items"] if it["kind"] == kind)


# ---------- descoberta e pareamento ----------

def test_discovery(server):
    found = discover("127.0.0.1", server.discovery_port, timeout=0.5)
    assert found == [{"ponte": 1, "name": "Mac de teste", "port": server.port, "id": server.server_id,
                      "host": "127.0.0.1"}]


def test_info_without_auth(server):
    info = Client("127.0.0.1", server.port).info()
    assert info["protocol"] == 1 and info["pairing_required"] and info["id"] == server.server_id


def test_requires_pairing(server):
    c = Client("127.0.0.1", server.port)
    with pytest.raises(PonteError) as e:
        c.catalog()
    assert e.value.status == 401


def test_pair_wrong_code_and_rate_limit(server):
    c = Client("127.0.0.1", server.port)
    wrong = "000000" if server.code != "000000" else "111111"
    for _ in range(5):
        with pytest.raises(PonteError) as e:
            c.pair(wrong)
        assert e.value.status == 401
    with pytest.raises(PonteError) as e:
        c.pair(server.code)
    assert e.value.status == 429


def test_code_rotates_and_token_persists(server, tmp_path):
    first = server.code
    c = Client("127.0.0.1", server.port)
    token = c.pair(first)
    assert server.code != first
    saved = json.loads((tmp_path / "estado" / "tokens.json").read_text())
    assert saved[token]["platform"] == "ps5"
    server.revoke(token)
    with pytest.raises(PonteError) as e:
        c.catalog()
    assert e.value.status == 401


def test_protocol_mismatch(server, client):
    with pytest.raises(PonteError) as e:
        client.request("GET", "/api/v1/info", headers={"X-Ponte-Protocol": "2"})
    assert e.value.status == 426


# ---------- catálogo ----------

def test_catalog(client):
    cat = client.catalog()
    kinds = sorted(it["kind"] for it in cat["items"])
    assert kinds == ["exfat", "folder", "pkg_ps4"]  # sem o .nsp: plataforma ps5
    folder = by_kind(cat, "folder")
    assert folder["title"] == "Jogo Cinco" and folder["icon"]
    assert folder["file_count"] == 5
    exfat = by_kind(cat, "exfat")
    assert exfat["title_id"] == "PPSA05678" and "metadata_from_filename" in exfat["warnings"]

    sw = client.request("GET", "/api/v1/catalog?platform=switch")
    assert [it["kind"] for it in sw["items"]] == ["nsp"]


def test_item_detail_and_icon(client):
    folder = by_kind(client.catalog(), "folder")
    d = client.item(folder["id"])
    paths = [f["path"] for f in d["files"]]
    assert paths == sorted(paths, key=str.encode)
    icon = client.request("GET", f"/api/v1/items/{folder['id']}/icon")
    assert icon.startswith(b"\x89PNG")


def test_not_found_and_bad_range(client):
    with pytest.raises(PonteError) as e:
        client.item("0" * 16)
    assert e.value.status == 404
    pkg = by_kind(client.catalog(), "pkg_ps4")
    with pytest.raises(PonteError) as e:
        client.request("GET", f"/api/v1/items/{pkg['id']}/files/0", headers={"Range": "bytes=999999999-"})
    assert e.value.status == 416
    with pytest.raises(PonteError) as e:
        client.request("GET", f"/api/v1/items/{pkg['id']}/files/9")
    assert e.value.status == 404


def test_head_and_range(server, client):
    pkg = by_kind(client.catalog(), "pkg_ps4")
    conn = http.client.HTTPConnection("127.0.0.1", server.port)
    hdr = {"Authorization": f"Bearer {client.token}"}
    conn.request("HEAD", f"/api/v1/items/{pkg['id']}/files/0", headers=hdr)
    r = conn.getresponse()
    r.read()
    assert r.status == 200 and int(r.getheader("Content-Length")) == pkg["size"]
    assert r.getheader("Accept-Ranges") == "bytes"
    conn.request("GET", f"/api/v1/items/{pkg['id']}/files/0", headers={**hdr, "Range": "bytes=10-19"})
    r = conn.getresponse()
    body = r.read()
    assert r.status == 206 and len(body) == 10
    assert r.getheader("Content-Range") == f"bytes 10-19/{pkg['size']}"
    conn.close()


def test_parse_range_rules():
    assert parse_range("bytes=0-", 10) == (0, 9)
    assert parse_range("bytes=5-100", 10) == (5, 9)
    assert parse_range("bytes=-3", 10) == (7, 9)
    for bad in ("bytes=10-", "bytes=5-2", "bytes=-", "items=0-1", "bytes=0-1,3-4"):
        with pytest.raises(ApiError):
            parse_range(bad, 10)


# ---------- estado do console ----------

def test_console_state(client):
    installed = [{"title_id": "CUSA00001", "version": "01.00", "content": "base", "storage": "internal"}]
    res = client.console_state(installed, [{"id": "internal", "label": "Interno", "free": 1, "total": 2}])
    states = {it["kind"]: it["state"] for it in res["items"]}
    assert states == {"pkg_ps4": "installed", "folder": "new", "exfat": "new"}


def test_console_state_bad_body(client):
    with pytest.raises(PonteError) as e:
        client.request("POST", "/api/v1/console/state", {"platform": "ps5", "installed": "x"})
    assert e.value.status == 400


# ---------- cópia ----------

def test_download_folder_to_homebrew(server, client, tmp_path):
    folder = by_kind(client.catalog(), "folder")
    dest = tmp_path / "console"
    r = client.download(folder["id"], dest)
    final = dest / "homebrew" / "PPSA01234"
    assert r["result"] == "copied" and r["final"] == str(final)
    src = server.library.get(folder["id"]).abs_path
    for f in client.item(folder["id"])["files"]:
        assert (final / f["path"]).read_bytes() == (src / f["path"]).read_bytes()
    assert not (dest / "ponte" / "tmp" / folder["id"]).exists()
    assert server.reports[-1]["result"] == "copied"


def test_download_pkg_is_installed(client, tmp_path):
    pkg = by_kind(client.catalog(), "pkg_ps4")
    r = client.download(pkg["id"], tmp_path / "console")
    assert r["result"] == "installed" and r["final"].endswith("installed/velho.pkg")


def test_resume_after_interruption(server, client, tmp_path):
    folder = by_kind(client.catalog(), "folder")
    dest = tmp_path / "console"
    with pytest.raises(Interrupted):
        client.download(folder["id"], dest, max_bytes=1_500_000)
    tmp = dest / "ponte" / "tmp" / folder["id"]
    assert tmp.exists()
    # o "montador" não pode ver nada pela metade em homebrew/
    assert not (dest / "homebrew").exists()
    partial = sum(p.stat().st_size for p in tmp.rglob("*") if p.is_file() and p.name != ".revision")
    assert 0 < partial < folder["size"]

    with pytest.raises(Interrupted):
        client.download(folder["id"], dest, max_bytes=1_000_000)
    r = client.download(folder["id"], dest)
    big = (dest / "homebrew" / "PPSA01234" / "data" / "big.bin").read_bytes()
    src = (server.library.get(folder["id"]).abs_path / "data" / "big.bin").read_bytes()
    assert hashlib.sha256(big).digest() == hashlib.sha256(src).digest()
    assert r["result"] == "copied"


def test_revision_change_restarts(server, client, tmp_path, lib_root):
    exfat = by_kind(client.catalog(), "exfat")
    dest = tmp_path / "console"
    with pytest.raises(Interrupted):
        client.download(exfat["id"], dest, max_bytes=500_000)

    # a imagem muda no Mac (ex.: foi refeita); a revisão antiga não vale mais
    (lib_root / "PPSA05678.exfat").write_bytes(os.urandom(2 * 1024 * 1024))
    age(lib_root)
    server.library.scan()
    old_rev = (dest / "ponte" / "tmp" / exfat["id"] / ".revision").read_text()
    with pytest.raises(PonteError) as e:
        client.request("GET", f"/api/v1/items/{exfat['id']}/files/0",
                       headers={"X-Ponte-Revision": old_rev, "Range": "bytes=10-"})
    assert e.value.status == 412

    r = client.download(exfat["id"], dest)  # detecta a revisão nova e recomeça
    assert (dest / "homebrew" / "PPSA05678.exfat").read_bytes() == (lib_root / "PPSA05678.exfat").read_bytes()
    assert r["result"] == "copied"


def test_file_changed_on_disk_without_rescan(server, client, lib_root):
    exfat = by_kind(client.catalog(), "exfat")
    (lib_root / "PPSA05678.exfat").write_bytes(b"x")
    # vale tanto antes da próxima varredura (arquivo mudou no disco) quanto depois (revisão nova)
    with pytest.raises(PonteError) as e:
        client.request("GET", f"/api/v1/items/{exfat['id']}/files/0",
                       headers={"X-Ponte-Revision": exfat["revision"]})
    assert e.value.status == 412


def test_destination_exists(client, tmp_path):
    pkg = by_kind(client.catalog(), "pkg_ps4")
    dest = tmp_path / "console"
    client.download(pkg["id"], dest)
    with pytest.raises(PonteError) as e:
        client.download(pkg["id"], dest)
    assert e.value.code == "destination_exists"
    assert client.download(pkg["id"], dest, replace=True)["result"] == "installed"


def test_hash_mismatch_is_caught(server, client, tmp_path, monkeypatch):
    pkg = by_kind(client.catalog(), "pkg_ps4")
    item = server.library.get(pkg["id"])
    f = item.files[0]
    # servidor anuncia um hash errado (também no cache, para sobreviver às varreduras de fundo)
    f.sha256 = "0" * 64
    server.library._hash_cache[(str(f.abs_path), f.size, f.mtime_ns)] = "0" * 64
    with pytest.raises(PonteError) as e:
        client.download(pkg["id"], tmp_path / "console")
    assert e.value.code == "hash_mismatch"
    assert server.reports[-1]["result"] == "failed"


def test_changes(client):
    assert client.changes("2000-01-01T00:00:00Z")["new_items"] == 3
    assert client.changes("2999-01-01T00:00:00Z") == {
        "changed": False, "new_items": 0, "generated_at": client.catalog()["generated_at"]}
    with pytest.raises(PonteError) as e:
        client.changes("ontem")
    assert e.value.status == 400


def test_item_not_ready_409(server, client, lib_root):
    make_nsp(lib_root / "Novo [0100ABCD55550000].nsp")  # recém-chegado: instável
    server.library.scan()
    from ponte_ref.library import item_id
    with pytest.raises(PonteError) as e:
        client.item(item_id("switch", "Novo [0100ABCD55550000].nsp"))
    assert e.value.status == 409


def test_unsafe_paths_rejected():
    for bad in ("/etc/passwd", "../x", "a/../b", "a//b", "a\\b", "", "./a"):
        with pytest.raises(PonteError):
            safe_relpath(bad)
    assert str(safe_relpath("sce_sys/param.json")) == os.path.join("sce_sys", "param.json")


def test_no_pairing_mode(lib_root, tmp_path):
    make_ps4_pkg(lib_root / "p.pkg")
    age(lib_root)
    srv = PonteServer(Library(lib_root), REPO / "profiles", tmp_path / "e", "Mac", host="127.0.0.1",
                      port=0, discovery_port=None, pairing=False, log=lambda *_: None)
    srv.start()
    try:
        c = Client("127.0.0.1", srv.port)
        assert c.info()["pairing_required"] is False
        assert len(c.catalog()["items"]) == 1
    finally:
        srv.stop()
