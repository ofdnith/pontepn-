#!/usr/bin/env python3
"""Teste de conformidade caixa-preta do protocolo Ponte.

Sobe um servidor Ponte qualquer (a referência em Python, o `ponte-server` em
Swift ou outro) como processo separado, sobre uma biblioteca sintética, e
confere o comportamento da SPEC só pela rede: descoberta, pareamento, rotas,
erros, Range, retomada, revisão e cópia completa com o cliente falso.

Uso, na raiz do repositório:
    python3 tools/conformance.py --server python
    python3 tools/conformance.py --server swift          # mac/.build/debug/ponte-server
    python3 tools/conformance.py --cmd "<comando com {library} {port} {discovery_port} {state} {profiles} {extra}>"

Contrato do servidor (o mesmo da referência):
    --library DIR --port N --discovery-port N --state-dir DIR --profiles DIR
    --rescan SEGUNDOS --stable-seconds SEGUNDOS --name NOME [--no-pairing]
    imprime "Código de pareamento: NNNNNN" ao subir e "Novo código: NNNNNN" após cada pareamento.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "reference"))

from fake_client import Client, Interrupted, PonteError, discover  # noqa: E402
from ponte_ref.formats.pfs0 import build_pfs0  # noqa: E402  (só geradores de dados sintéticos)
from ponte_ref.formats.pkg import build_pkg  # noqa: E402
from ponte_ref.formats.sfo import build_sfo  # noqa: E402

PRESETS = {
    "python": ("cd reference && PYTHONUNBUFFERED=1 exec python3 -m ponte_ref.server"),
    "swift": ("exec mac/.build/debug/ponte-server"),
}
ARGS = ("--library {library} --port {port} --discovery-port {discovery_port} --state-dir {state} "
        "--profiles {profiles} --rescan 0.2 --stable-seconds 2 --name 'Mac de teste' {extra}")

CODE_RE = re.compile(r"(?:Código de pareamento|Novo código):\s*(\d{6})")
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24


# ---------- biblioteca sintética ----------

def age(path: Path, seconds: float = 3600) -> None:
    t = time.time() - seconds
    targets = [path] if path.is_file() else [Path(d) / f for d, _, fs in os.walk(path) for f in fs]
    for p in targets:
        os.utime(p, (t, t))


def write(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def nsp_bytes(tid: str) -> bytes:
    return build_pfs0({"abcdef0123456789abcdef0123456789.nca": b"N" * 3000,
                       f"{tid.lower()}0000000000000000.tik": b"T" * 0x2C0})


def build_library(root: Path) -> None:
    game = root / "PPSA01234"
    write(game / "sce_sys" / "param.json", json.dumps({
        "titleId": "PPSA01234", "contentId": "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP",
        "contentVersion": "01.000.000", "requiredSystemSoftwareVersion": "0x0403000000000000",
        "localizedParameters": {"defaultLanguage": "pt-BR", "pt-BR": {"titleName": "Jogo Cinco"}},
    }).encode())
    write(game / "sce_sys" / "icon0.png", PNG)
    write(game / "eboot.bin", b"E" * 5000)
    write(game / "data" / "a.bin", b"A" * 1234)
    write(game / "data" / "big.bin", os.urandom(3 * 1024 * 1024 + 17))
    write(game / "Zeta.bin", b"z")
    write(game / ".DS_Store", b"finder")  # lixo do Finder: fica fora do item
    write(game / "data" / "._a.bin", b"appledouble")
    write(root / "PPSA05678.exfat", os.urandom(2 * 1024 * 1024))
    sfo = build_sfo({"CATEGORY": "gd", "TITLE": "Jogo Quatro", "TITLE_ID": "CUSA00001", "APP_VER": "01.00",
                     "VERSION": "01.00", "SYSTEM_VER": 0x05050000,
                     "CONTENT_ID": "UP0000-CUSA00001_00-ABCDEFGHIJKLMNOP"})
    write(root / "velho.pkg", build_pkg("UP0000-CUSA00001_00-ABCDEFGHIJKLMNOP", sfo, PNG, b"B" * 4096))
    # .ffpkg com arquivo lateral: o lateral manda (SPEC §1.6)
    write(root / "Imagem.ffpkg", b"\x00" * 4096)
    write(root / "Imagem.ffpkg.ponte.json", json.dumps({
        "platform": "ps5", "kind": "ffpkg", "content": "base", "title": "Imagem Lateral",
        "title_id": "PPSA07777", "version": "01.000.000"}).encode())
    write(root / "Switch" / "Jogo [0100ABCD12340000][v0].nsp", nsp_bytes("0100ABCD12340000"))
    write(root / "Switch" / "Jogo [0100ABCD12340800][v65536].nsp", nsp_bytes("0100ABCD12340800"))
    write(root / "Jogo.part1.rar", b"r")  # compactado: nunca entra no catálogo
    write(root / "leia.txt", b"oi")
    age(root)


def item_id(platform: str, rel: str) -> str:
    return hashlib.sha256(f"{platform}:{rel}".encode()).hexdigest()[:16]


# ---------- servidor como processo ----------

def free_port(kind=socket.SOCK_STREAM) -> int:
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class ServerProcess:
    def __init__(self, template: str, library: Path, state: Path, extra: str = ""):
        self.port = free_port()
        self.discovery_port = free_port(socket.SOCK_DGRAM)
        cmd = template.format(library=shlex.quote(str(library)), port=self.port,
                              discovery_port=self.discovery_port, state=shlex.quote(str(state)),
                              profiles=shlex.quote(str(REPO / "profiles")), extra=extra)
        self.output: list[str] = []
        self.codes: list[str] = []
        self._cv = threading.Condition()
        self.proc = subprocess.Popen(cmd, shell=True, cwd=REPO, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()
        self._wait_ready()

    def _pump(self) -> None:
        for line in self.proc.stdout:
            with self._cv:
                self.output.append(line.rstrip("\n"))
                m = CODE_RE.search(line)
                if m:
                    self.codes.append(m.group(1))
                self._cv.notify_all()

    def _wait_ready(self) -> None:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("o servidor terminou ao subir:\n" + "\n".join(self.output))
            try:
                Client("127.0.0.1", self.port, timeout=1).info()
                return
            except (OSError, PonteError):
                time.sleep(0.1)
        raise RuntimeError("o servidor não respondeu em 30 s:\n" + "\n".join(self.output))

    def code(self, after: int = 0, timeout: float = 5) -> str:
        """Código de pareamento atual; `after` = quantos códigos já foram vistos antes."""
        with self._cv:
            self._cv.wait_for(lambda: len(self.codes) > after, timeout)
            if len(self.codes) <= after:
                raise AssertionError("o servidor não imprimiu o código de pareamento")
            return self.codes[-1]

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# ---------- utilidades de teste ----------

def raw(port: int, method: str, path: str, headers=None, body: bytes | None = None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request(method, path, body=body, headers=headers or {})
    r = conn.getresponse()
    data = r.read()
    conn.close()
    return r, data


def expect_error(status: int, fn, code: str | None = None) -> PonteError:
    try:
        fn()
    except PonteError as e:
        assert e.status == status, f"esperava {status}, veio {e.status} {e.code}"
        if code:
            assert e.code == code, f"esperava código {code}, veio {e.code}"
        return e
    raise AssertionError(f"esperava erro {status}")


def by_kind(catalog: dict, kind: str) -> dict:
    return next(it for it in catalog["items"] if it["kind"] == kind)


def wait_until(fn, timeout=8.0, step=0.1):
    deadline = time.monotonic() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.monotonic() > deadline:
            raise AssertionError("condição não foi atingida a tempo")
        time.sleep(step)


ITEM_FIELDS = {"id", "revision", "platform", "kind", "content", "title", "title_id", "content_id", "version",
               "required_firmware", "size", "file_count", "parent_title_id", "warnings", "added_at", "icon"}


# ---------- casos ----------

def run_main_suite(template: str, work: Path, results: list) -> None:
    lib = work / "biblioteca"
    lib.mkdir()
    build_library(lib)
    srv = ServerProcess(template, lib, work / "estado")
    tests = []

    def case(fn):
        tests.append(fn)
        return fn

    ctx: dict = {}

    @case
    def info_sem_autenticacao():
        info = Client("127.0.0.1", srv.port).info()
        assert info["protocol"] == 1 and info["pairing_required"] is True
        assert re.fullmatch(r"[0-9a-f]{16}", info["id"]) and info["name"] == "Mac de teste"
        assert isinstance(info["server"], str) and info["server"]
        ctx["server_id"] = info["id"]

    @case
    def descoberta_udp():
        found = discover("127.0.0.1", srv.discovery_port, timeout=1.0)
        assert len(found) == 1, found
        f = found[0]
        assert f["ponte"] == 1 and f["port"] == srv.port and f["id"] == ctx["server_id"], f
        assert f["name"] == "Mac de teste"

    @case
    def descoberta_ignora_lixo():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.5)
        s.sendto(b"OUTRA COISA", ("127.0.0.1", srv.discovery_port))
        try:
            s.recvfrom(1024)
            raise AssertionError("respondeu a uma consulta inválida")
        except socket.timeout:
            pass
        finally:
            s.close()

    @case
    def exige_pareamento():
        expect_error(401, Client("127.0.0.1", srv.port).catalog, "unauthorized")
        expect_error(401, lambda: Client("127.0.0.1", srv.port, token="f" * 64).catalog(), "unauthorized")

    @case
    def formato_de_erro_e_content_length():
        r, data = raw(srv.port, "GET", "/api/v1/catalog")
        assert r.status == 401 and r.getheader("Content-Length") == str(len(data))
        assert r.getheader("Content-Type", "").startswith("application/json")
        err = json.loads(data)["error"]
        assert err["code"] == "unauthorized" and isinstance(err["message"], str)

    @case
    def protocolo_diferente_426():
        expect_error(426, lambda: Client("127.0.0.1", srv.port).request(
            "GET", "/api/v1/info", headers={"X-Ponte-Protocol": "2"}), "protocol_mismatch")

    @case
    def pareamento_e_codigo_novo():
        first = srv.code()
        c = Client("127.0.0.1", srv.port)
        expect_error(400, lambda: c.request("POST", "/api/v1/pair", {"code": first, "client": {"platform": "xbox"}}),
                     "bad_request")
        token = c.pair(first, "PS5 de teste")
        assert re.fullmatch(r"[0-9a-f]{64}", token), token
        second = wait_until(lambda: srv.codes[-1] if srv.codes[-1] != first else None, timeout=5)
        assert second != first
        expect_error(401, lambda: Client("127.0.0.1", srv.port).pair(first), "unauthorized")
        ctx["client"] = c

    @case
    def json_invalido_400():
        c = ctx["client"]
        r, data = raw(srv.port, "POST", "/api/v1/console/state",
                      {"Authorization": f"Bearer {c.token}", "Content-Type": "application/json"}, b"{nao json")
        assert r.status == 400 and json.loads(data)["error"]["code"] == "bad_request"
        r, data = raw(srv.port, "POST", "/api/v1/console/state",
                      {"Authorization": f"Bearer {c.token}"}, b"[1, 2]")
        assert r.status == 400

    @case
    def catalogo_ps5():
        cat = ctx["client"].catalog()
        assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", cat["generated_at"])
        kinds = sorted(it["kind"] for it in cat["items"])
        assert kinds == ["exfat", "ffpkg", "folder", "pkg_ps4"], kinds
        for it in cat["items"]:
            assert ITEM_FIELDS <= set(it), set(ITEM_FIELDS) - set(it)
            assert it["platform"] == "ps5" and re.fullmatch(r"[0-9a-f]{8}", it["revision"])
            assert isinstance(it["size"], int) and it["title"]
        titles = [it["title"].casefold() for it in cat["items"]]
        assert titles == sorted(titles), titles
        folder = by_kind(cat, "folder")
        assert folder["id"] == item_id("ps5", "PPSA01234")
        assert folder["title"] == "Jogo Cinco" and folder["title_id"] == "PPSA01234"
        assert folder["required_firmware"] == "4.03" and folder["version"] == "01.000.000"
        assert folder["file_count"] == 6 and folder["content"] == "base"
        assert folder["icon"] == f"/api/v1/items/{folder['id']}/icon"
        exfat = by_kind(cat, "exfat")
        assert exfat["title_id"] == "PPSA05678" and "metadata_from_filename" in exfat["warnings"]
        assert exfat["icon"] is None
        pkg = by_kind(cat, "pkg_ps4")
        assert (pkg["title"], pkg["content"], pkg["version"], pkg["required_firmware"]) == \
            ("Jogo Quatro", "base", "01.00", "5.05")
        side = by_kind(cat, "ffpkg")
        assert side["title"] == "Imagem Lateral" and side["title_id"] == "PPSA07777"
        assert "metadata_from_filename" not in side["warnings"]
        ctx["catalog"] = cat

    @case
    def catalogo_switch():
        sw = ctx["client"].request("GET", "/api/v1/catalog?platform=switch")
        got = sorted((it["content"], it["title_id"], it["version"], it["parent_title_id"]) for it in sw["items"])
        assert got == [("base", "0100ABCD12340000", "0", None),
                       ("update", "0100ABCD12340800", "65536", "0100ABCD12340000")], got
        assert all(it["title"] == "Jogo" for it in sw["items"])
        assert sw["items"][0]["id"] in (item_id("switch", "Switch/Jogo [0100ABCD12340000][v0].nsp"),
                                        item_id("switch", "Switch/Jogo [0100ABCD12340800][v65536].nsp"))
        expect_error(400, lambda: ctx["client"].request("GET", "/api/v1/catalog?platform=xbox"), "bad_request")

    @case
    def detalhe_do_item():
        folder = by_kind(ctx["catalog"], "folder")
        d = ctx["client"].item(folder["id"])
        paths = [f["path"] for f in d["files"]]
        assert paths == sorted(paths, key=str.encode), paths
        assert paths == ["Zeta.bin", "data/a.bin", "data/big.bin", "eboot.bin",
                         "sce_sys/icon0.png", "sce_sys/param.json"], paths
        assert [f["index"] for f in d["files"]] == list(range(len(paths)))
        assert sum(f["size"] for f in d["files"]) == folder["size"]
        side = ctx["client"].item(by_kind(ctx["catalog"], "ffpkg")["id"])
        assert [f["path"] for f in side["files"]] == ["Imagem.ffpkg"]

    @case
    def icone():
        folder = by_kind(ctx["catalog"], "folder")
        r, data = raw(srv.port, "GET", folder["icon"], {"Authorization": f"Bearer {ctx['client'].token}"})
        assert r.status == 200 and data == PNG and r.getheader("Content-Type") == "image/png"
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        assert pkg["icon"]
        exfat = by_kind(ctx["catalog"], "exfat")
        expect_error(404, lambda: ctx["client"].request("GET", f"/api/v1/items/{exfat['id']}/icon"), "not_found")

    @case
    def nao_encontrado():
        c = ctx["client"]
        expect_error(404, lambda: c.item("0" * 16), "not_found")
        expect_error(404, lambda: c.request("GET", "/api/v1/nada"), "not_found")
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        expect_error(404, lambda: c.request("GET", f"/api/v1/items/{pkg['id']}/files/9"), "not_found")

    @case
    def head_e_range():
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        hdr = {"Authorization": f"Bearer {ctx['client'].token}"}
        url = f"/api/v1/items/{pkg['id']}/files/0"
        conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
        conn.request("HEAD", url, headers=hdr)
        r = conn.getresponse()
        assert r.read() == b""
        assert r.status == 200 and int(r.getheader("Content-Length")) == pkg["size"]
        assert r.getheader("Accept-Ranges") == "bytes" and r.getheader("X-Ponte-Revision") == pkg["revision"]
        # mesma conexão (keep-alive)
        conn.request("GET", url, headers={**hdr, "Range": "bytes=10-19"})
        r = conn.getresponse()
        body = r.read()
        assert r.status == 206 and len(body) == 10
        assert r.getheader("Content-Range") == f"bytes 10-19/{pkg['size']}"
        conn.request("GET", url, headers={**hdr, "Range": "bytes=-5"})
        r = conn.getresponse()
        tail = r.read()
        assert r.status == 206 and len(tail) == 5
        conn.request("GET", url, headers=hdr)
        r = conn.getresponse()
        whole = r.read()
        assert r.status == 200 and len(whole) == pkg["size"] and whole[10:20] == body and whole[-5:] == tail
        conn.close()
        for bad in (f"bytes={pkg['size']}-", "bytes=999999999-", "bytes=5-2", "items=0-1"):
            r, data = raw(srv.port, "GET", url, {**hdr, "Range": bad})
            assert r.status == 416 and json.loads(data)["error"]["code"] == "bad_range", bad

    @case
    def token_na_query_e_nome_no_fim():
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        token = ctx["client"].token
        url = f"/api/v1/items/{pkg['id']}/files/0/velho.pkg?token={token}"
        r, data = raw(srv.port, "GET", url)
        assert r.status == 200 and len(data) == pkg["size"], r.status
        r, _ = raw(srv.port, "HEAD", url, {"Range": "bytes=0-9"})
        assert r.status == 206 and r.getheader("Content-Range") == f"bytes 0-9/{pkg['size']}"
        r, data = raw(srv.port, "GET", f"/api/v1/items/{pkg['id']}/files/0/velho.pkg?token={'0' * 64}")
        assert r.status == 401
        # só nas rotas de arquivo
        r, _ = raw(srv.port, "GET", f"/api/v1/catalog?token={token}")
        assert r.status == 401

    @case
    def revisao_errada_412():
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        expect_error(412, lambda: ctx["client"].request(
            "GET", f"/api/v1/items/{pkg['id']}/files/0",
            headers={"X-Ponte-Revision": "00000000", "Range": "bytes=0-"}), "revision_changed")

    @case
    def hashes_calculados():
        folder = by_kind(ctx["catalog"], "folder")

        def all_hashed():
            d = ctx["client"].item(folder["id"])
            return d if all(f["sha256"] for f in d["files"]) else None

        d = wait_until(all_hashed, timeout=20)
        for f in d["files"]:
            assert f["sha256"] == hashlib.sha256((lib / "PPSA01234" / f["path"]).read_bytes()).hexdigest()
        r, _ = raw(srv.port, "HEAD", f"/api/v1/items/{folder['id']}/files/0",
                   {"Authorization": f"Bearer {ctx['client'].token}"})
        assert r.getheader("X-Ponte-Sha256") == d["files"][0]["sha256"]

    @case
    def estado_do_console():
        c = ctx["client"]
        installed = [{"title_id": "CUSA00001", "version": "01.00", "content": "base", "storage": "internal"},
                     {"title_id": "PPSA07777", "version": "00.900", "content": "base", "storage": "ext0"}]
        res = c.console_state(installed, [{"id": "internal", "label": "Interno", "free": 1, "total": 2}])
        states = {it["kind"]: (it["state"], it["state_reason"]) for it in res["items"]}
        assert states == {"pkg_ps4": ("installed", None), "folder": ("new", None), "exfat": ("new", None),
                          "ffpkg": ("update_available", None)}, states
        expect_error(400, lambda: c.request("POST", "/api/v1/console/state",
                                            {"platform": "ps5", "installed": "x"}), "bad_request")

    @case
    def perfil_e_firmware():
        c = ctx["client"]
        body = {"platform": "ps5", "firmware": "4.00", "storages": [], "installed": []}
        res = c.request("POST", "/api/v1/console/state", body)
        folder = next(it for it in res["items"] if it["kind"] == "folder")
        assert folder["state"] == "incompatible" and "4.03" in folder["state_reason"]
        assert "profile_guessed" in folder["warnings"]
        body["firmware"] = "13.60"
        res = c.request("POST", "/api/v1/console/state", body)
        assert all("profile_guessed" not in it["warnings"] for it in res["items"])

    @case
    def mudancas():
        c = ctx["client"]
        r = c.changes("2000-01-01T00:00:00Z")
        assert r["changed"] is True and r["new_items"] == 4, r
        r = c.changes("2999-01-01T00:00:00Z")
        assert r == {"changed": False, "new_items": 0, "generated_at": c.catalog()["generated_at"]}, r
        expect_error(400, lambda: c.changes("ontem"), "bad_request")

    @case
    def copia_de_pasta():
        folder = by_kind(ctx["catalog"], "folder")
        dest = work / "console"
        r = ctx["client"].download(folder["id"], dest)
        final = dest / "homebrew" / "PPSA01234"
        assert r["result"] == "copied" and Path(r["final"]) == final
        assert not (final / ".DS_Store").exists() and not (final / "data" / "._a.bin").exists()
        for p in (lib / "PPSA01234").rglob("*"):
            if p.is_file() and p.name != ".DS_Store" and not p.name.startswith("._"):
                assert (final / p.relative_to(lib / "PPSA01234")).read_bytes() == p.read_bytes()
        assert not (dest / "ponte" / "tmp" / folder["id"]).exists()

    @case
    def retomada():
        folder = by_kind(ctx["catalog"], "folder")
        dest = work / "console-retomada"
        for budget in (1_500_000, 1_000_000):
            try:
                ctx["client"].download(folder["id"], dest, max_bytes=budget)
                raise AssertionError("deveria ter sido interrompido")
            except Interrupted:
                pass
        assert not (dest / "homebrew").exists()
        r = ctx["client"].download(folder["id"], dest)
        assert r["result"] == "copied"
        assert (dest / "homebrew" / "PPSA01234" / "data" / "big.bin").read_bytes() == \
            (lib / "PPSA01234" / "data" / "big.bin").read_bytes()

    @case
    def copia_de_pkg_e_destino_ocupado():
        pkg = by_kind(ctx["catalog"], "pkg_ps4")
        dest = work / "console-pkg"
        assert ctx["client"].download(pkg["id"], dest)["result"] == "installed"
        expect_error(0, lambda: ctx["client"].download(pkg["id"], dest), "destination_exists")
        expect_error(400, lambda: ctx["client"].request("POST", f"/api/v1/items/{pkg['id']}/report",
                                                        {"result": "talvez"}), "bad_request")
        hdr = {"Authorization": f"Bearer {ctx['client'].token}", "Content-Type": "application/json"}
        r, data = raw(srv.port, "POST", f"/api/v1/items/{pkg['id']}/report", hdr,
                      json.dumps({"result": "installed", "storage": "internal"}).encode())
        assert r.status == 204 and data == b""

    @case
    def revisao_muda_no_meio():
        exfat = by_kind(ctx["catalog"], "exfat")
        dest = work / "console-rev"
        try:
            ctx["client"].download(exfat["id"], dest, max_bytes=500_000)
            raise AssertionError("deveria ter sido interrompido")
        except Interrupted:
            pass
        (lib / "PPSA05678.exfat").write_bytes(os.urandom(2 * 1024 * 1024))
        # antes da próxima varredura, o arquivo mudou no disco: 412
        expect_error(412, lambda: ctx["client"].request(
            "GET", f"/api/v1/items/{exfat['id']}/files/0",
            headers={"X-Ponte-Revision": exfat["revision"], "Range": "bytes=10-"}), "revision_changed")
        age(lib / "PPSA05678.exfat")
        new = wait_until(lambda: (lambda d: d if d["revision"] != exfat["revision"] else None)(
            ctx["client"].item(exfat["id"])))
        assert ctx["client"].catalog()["generated_at"] >= ctx["catalog"]["generated_at"]
        r = ctx["client"].download(exfat["id"], dest)
        assert r["result"] == "copied" and new["revision"]
        assert (dest / "homebrew" / "PPSA05678.exfat").read_bytes() == (lib / "PPSA05678.exfat").read_bytes()

    @case
    def item_ainda_nao_pronto_409():
        rel = "Novo [0100ABCD55550000].nsp"
        (lib / rel).write_bytes(nsp_bytes("0100ABCD55550000"))  # recém-chegado: instável
        iid = item_id("switch", rel)
        wait_until(lambda: _status_of(lambda: ctx["client"].item(iid)) == 409, timeout=5)
        age(lib / rel)
        wait_until(lambda: _status_of(lambda: ctx["client"].item(iid)) == 200, timeout=8)

    @case
    def token_persiste_ao_reiniciar():
        token = ctx["client"].token
        srv.stop()
        srv2 = ServerProcess(template, lib, work / "estado")
        try:
            info = Client("127.0.0.1", srv2.port).info()
            assert info["id"] == ctx["server_id"], "o id do servidor deve sobreviver ao reinício"
            c = Client("127.0.0.1", srv2.port, token=token)
            assert c.catalog()["items"]
        finally:
            srv2.stop()

    try:
        for fn in tests:
            _run(fn, results)
    finally:
        srv.stop()


def _status_of(fn) -> int:
    try:
        fn()
        return 200
    except PonteError as e:
        return e.status


def run_side_suite(template: str, work: Path, results: list) -> None:
    lib = work / "biblioteca2"
    lib.mkdir()
    write(lib / "Jogo [0100ABCD12340000][v0].nsp", nsp_bytes("0100ABCD12340000"))
    age(lib)

    srv = ServerProcess(template, lib, work / "estado2")

    def limite_de_tentativas():
        c = Client("127.0.0.1", srv.port, platform="switch")
        code = srv.code()
        wrong = "000000" if code != "000000" else "111111"
        for _ in range(5):
            expect_error(401, lambda: c.pair(wrong), "unauthorized")
        expect_error(429, lambda: c.pair(code), "too_many_attempts")

    try:
        _run(limite_de_tentativas, results)
    finally:
        srv.stop()

    srv = ServerProcess(template, lib, work / "estado3", extra="--no-pairing")

    def sem_pareamento():
        c = Client("127.0.0.1", srv.port, platform="switch")
        assert c.info()["pairing_required"] is False
        cat = c.request("GET", "/api/v1/catalog?platform=switch")
        assert [it["kind"] for it in cat["items"]] == ["nsp"]
        res = c.request("POST", "/api/v1/console/state", {"platform": "switch", "firmware": "19.0.1",
                                                          "storages": [], "installed": [
                                                              {"title_id": "0100abcd12340000", "version": "0",
                                                               "content": "base", "storage": "sd"}]})
        assert res["items"][0]["state"] == "installed"

    try:
        _run(sem_pareamento, results)
    finally:
        srv.stop()


def _run(fn, results: list) -> None:
    name = fn.__name__
    t = time.monotonic()
    try:
        fn()
        results.append((name, None))
        print(f"  ok    {name} ({time.monotonic() - t:.1f}s)")
    except Exception as e:  # noqa: BLE001
        results.append((name, e))
        print(f"  FALHA {name}: {e}")
        traceback.print_exc(limit=3)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Teste de conformidade do protocolo Ponte")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--server", choices=sorted(PRESETS))
    g.add_argument("--cmd", help="comando com {library} {port} {discovery_port} {state} {profiles} {extra}")
    ap.add_argument("--keep", action="store_true", help="não apaga a pasta de trabalho")
    args = ap.parse_args(argv)
    template = args.cmd or f"{PRESETS[args.server]} {ARGS}"

    work = Path(tempfile.mkdtemp(prefix="ponte-conf-"))
    results: list = []
    print(f"Servidor: {template.split(' --')[0]}")
    try:
        run_main_suite(template, work, results)
        run_side_suite(template, work, results)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    failed = [n for n, e in results if e]
    print(f"\n{len(results) - len(failed)}/{len(results)} casos passaram" + (f"; falharam: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
