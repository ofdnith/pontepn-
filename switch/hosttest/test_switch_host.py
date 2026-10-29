"""Cliente do Switch (núcleo C++ do fork do Sphaira) contra o servidor de referência.

Roda o binário `build/ponte_host` (ver Makefile). Sem ele, os testes são pulados.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "reference"))
sys.path.insert(0, str(REPO / "reference" / "tests"))

from conftest import age, make_nsp, make_ps4_pkg  # noqa: E402
from ponte_ref.library import Library  # noqa: E402
from ponte_ref.server import PonteServer  # noqa: E402

BIN = HERE / "build" / "ponte_host"
pytestmark = pytest.mark.skipif(not BIN.exists(), reason="rode `make -C switch/hosttest` antes")

BASE = "0100ABCD12340000"
UPD = "0100ABCD12340800"
DLC = "0100ABCD12341001"


def run(*args):
    out = subprocess.run([str(BIN), *map(str, args)], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stdout + out.stderr
    return out.stdout.splitlines()


@pytest.fixture
def server(tmp_path):
    lib_root = tmp_path / "biblioteca"
    lib_root.mkdir()
    make_nsp(lib_root / f"Jogo Legal [{BASE}][v0].nsp", tid=BASE)
    make_nsp(lib_root / f"Jogo Legal [{UPD}][v65536].nsp", tid=UPD)
    make_nsp(lib_root / f"Jogo Legal [{UPD}][v131072].nsp", tid=UPD)
    make_nsp(lib_root / f"Jogo Legal DLC [{DLC}][v0].nsp", tid=DLC)
    big = lib_root / "Abc [0100000000AAA000][v0].xci"
    big.write_bytes(os.urandom(3 * 1024 * 1024 + 17))
    make_ps4_pkg(lib_root / "de-ps4.pkg")  # não pode aparecer no catálogo do Switch
    age(lib_root)
    srv = PonteServer(Library(lib_root, stable_seconds=10), REPO / "profiles", tmp_path / "estado",
                      "Mac de teste", host="127.0.0.1", port=0, discovery_host="127.0.0.1",
                      discovery_port=0, rescan_interval=0.2, log=lambda *_: None)
    srv.start()
    srv.lib_root = lib_root
    yield srv
    srv.stop()


def test_unit():
    assert run("unit") == ["UNIT OK"]


def test_discover(server):
    assert run("discover", "127.0.0.1", server.discovery_port) == [
        f"DISCOVER 127.0.0.1 {server.port} {server.server_id} Mac de teste"]


def test_flow_fresh_console(server, tmp_path):
    dest = tmp_path / "switch"
    dest.mkdir()
    out = run("flow", "127.0.0.1", server.port, server.code, dest)

    assert out[0].startswith(f"INFO {server.server_id} pairing=1")
    assert out[1] == "PAIR_ERRADO 401 unauthorized"
    assert out[2] == "PAIR ok"
    assert "CHANGES 200 0 0" in out  # nada novo desde o catálogo que acabou de chegar

    rows = [line for line in out if line.startswith("ROW ")]
    # agrupado por jogo, base primeiro, update mais novo antes do velho, PS4 fora
    assert rows == [
        "ROW 0 0100000000AAA000 base 0 new xci",
        f"ROW 0 {BASE} base 0 new nsp",
        f"ROW 1 {UPD} update 131072 new nsp",
        f"ROW 1 {UPD} update 65536 new nsp",
        f"ROW 1 {DLC} dlc 0 new nsp",
    ]

    # cada arquivo: 412 com revisão velha, depois 206 + retomada 206 até o fim
    files = [line for line in out if line.startswith("FILE ")]
    assert len(files) == 5
    for line in files:
        _, kind, first, second, got, crange = line.split(" ", 5)
        assert first == "206" and second == "206"
        have, total = got.split("/")
        assert have == total and crange.endswith(f"/{total}")
    assert [line for line in out if line.startswith("STALE ")] == ["STALE 412 revision_changed"] * 5

    # ordem de instalação: jogo Abc, depois base → updates (velho → novo) → dlc
    reports = [line.split()[1] for line in out if line.startswith("REPORT ")]
    reported = [server.library.get(r["item"]).ident for r in server.reports]
    assert [(i.title_id, i.version) for i in reported] == [
        ("0100000000AAA000", "0"), (BASE, "0"), (UPD, "65536"), (UPD, "131072"), (DLC, "0")]
    assert all(line.endswith(" 204") for line in out if line.startswith("REPORT "))
    assert [r["result"] for r in server.reports] == ["installed"] * 5
    assert len(set(reports)) == 5

    # bytes idênticos aos da biblioteca
    lib = {p.read_bytes() for p in server.lib_root.iterdir() if p.suffix in (".nsp", ".xci")}
    got = {p.read_bytes() for p in dest.iterdir()}
    assert got == lib

    assert out[-1] == "MISMATCH 426 protocol_mismatch"


def test_flow_console_with_content(server, tmp_path):
    dest = tmp_path / "switch"
    dest.mkdir()
    out = run("flow", "127.0.0.1", server.port, server.code, dest,
              f"{BASE}:0:base", f"{UPD}:65536:update", f"{DLC}:0:dlc")
    rows = [line for line in out if line.startswith("ROW ")]
    assert rows == [
        "ROW 0 0100000000AAA000 base 0 new xci",
        f"ROW 0 {BASE} base 0 installed nsp",
        f"ROW 1 {UPD} update 131072 update_available nsp",
        f"ROW 1 {UPD} update 65536 installed nsp",
        f"ROW 1 {DLC} dlc 0 installed nsp",
    ]
    # só o que falta vai para o console
    reported = [server.library.get(r["item"]).ident for r in server.reports]
    assert [(i.title_id, i.version) for i in reported] == [("0100000000AAA000", "0"), (UPD, "131072")]
