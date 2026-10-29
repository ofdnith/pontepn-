#!/usr/bin/env python3
"""Testa o payload (versão de teste, build/ponte-host) de ponta a ponta contra um servidor do Mac.

O "console" é uma pasta temporária; o servidor é a referência em Python (padrão) ou o
ponte-server em Swift. Tudo é dirigido pela interface web do payload, como no celular:

    python3 tests/test_payload.py                 # contra a referência em Python
    python3 tests/test_payload.py --server swift  # contra mac/.build/debug/ponte-server
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
PS5 = HERE.parent
REPO = PS5.parent
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "reference"))

from conformance import ARGS, PRESETS, ServerProcess, age, build_library, free_port  # noqa: E402
from fake_client import Client  # noqa: E402

PAYLOAD = PS5 / "build" / "ponte-host"


class Payload:
    def __init__(self, root: Path, srv: ServerProcess):
        self.port = free_port()
        env = dict(os.environ, PONTE_ROOT=str(root), PONTE_UI_PORT=str(self.port),
                   PONTE_DISCOVERY_PORT=str(srv.discovery_port), PONTE_DISCOVERY_HOST="127.0.0.1")
        self.output: list[str] = []
        self.proc = subprocess.Popen([str(PAYLOAD)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()
        deadline = time.monotonic() + 10
        while True:
            try:
                self.get("/api/status")
                return
            except OSError:
                if time.monotonic() > deadline or self.proc.poll() is not None:
                    raise RuntimeError("o payload não subiu:\n" + "\n".join(self.output))
                time.sleep(0.1)

    def _pump(self):
        for line in self.proc.stdout:
            self.output.append(line.rstrip("\n"))

    def request(self, method: str, path: str, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        data = json.dumps(body).encode() if body is not None else None
        conn.request(method, path, body=data, headers={"Content-Type": "application/json"} if data else {})
        r = conn.getresponse()
        raw = r.read()
        conn.close()
        ctype = r.getheader("Content-Type", "")
        return r.status, (json.loads(raw) if ctype.startswith("application/json") and raw else raw)

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, body=None):
        return self.request("POST", path, body if body is not None else {})

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def wait_until(fn, timeout=15.0, step=0.1):
    deadline = time.monotonic() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.monotonic() > deadline:
            raise AssertionError("condição não foi atingida a tempo")
        time.sleep(step)


def job_for(p: Payload, item_id: str, after_seq: int = 0):
    _, st = p.get("/api/status")
    for j in st["jobs"]:
        if j["item_id"] == item_id and j["seq"] > after_seq:
            return j
    return None


def wait_job(p: Payload, item_id: str, after_seq: int = 0, timeout=30.0,
             states=("done", "failed", "cancelled")) -> dict:
    return wait_until(lambda: (lambda j: j if j and j["state"] in states else None)(
        job_for(p, item_id, after_seq)), timeout)


def last_seq(p: Payload) -> int:
    _, st = p.get("/api/status")
    return max([j["seq"] for j in st["jobs"]], default=0)


def by_kind(cat, kind):
    return next(it for it in cat["items"] if it["kind"] == kind)


def tree_files(root: Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}


def run(template: str, work: Path) -> list:
    lib = work / "biblioteca"
    lib.mkdir()
    build_library(lib)
    console = work / "console"
    (console / "usb0").mkdir(parents=True)
    srv = ServerProcess(template, lib, work / "estado")
    p = Payload(console, srv)
    results = []
    ctx: dict = {}

    def token() -> str:
        return json.loads((console / "data" / "ponte" / "config.json").read_text())["mac"]["token"]

    def case(fn):
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
        return fn

    @case
    def sha256_sha_ni_confere_com_o_portatil():
        exe = PS5 / "build" / "sha256-selftest"
        assert exe.exists(), "compile antes: make host"
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        assert out.returncode == 0, out.stdout + out.stderr
        print("        " + out.stdout.strip().replace("\n", "\n        "))

    @case
    def acha_o_mac_sozinho():
        st = wait_until(lambda: (lambda s: s if s["mac"] and s["mac"]["reachable"] else None)(p.get("/api/status")[1]))
        assert st["mac"]["name"] == "Mac de teste" and st["mac"]["paired"] is False, st["mac"]
        assert st["firmware"] == "13.60"
        assert [s["id"] for s in st["storages"]] == ["internal", "usb0"], st["storages"]
        status, found = p.post("/api/discover")
        assert status == 200 and any(m["port"] == srv.port for m in found), found

    @case
    def pagina_da_interface():
        status, html = p.get("/")
        assert status == 200 and b"<title>Ponte</title>" in html
        status, icon = p.get("/icon.png")
        assert status == 200 and icon == (PS5 / "assets" / "icon0.png").read_bytes()

    @case
    def icone_da_tela_inicial():
        tile = console / "data" / "ponte" / "tile" / "PNTE00001"
        assert not (console / "internal" / "homebrew" / "PNTE00001").exists()
        param = json.loads((tile / "sce_sys" / "param.json").read_text())
        assert param["titleId"] == "PNTE00001" and param["deeplinkUri"] == f"http://127.0.0.1:{p.port}/", param
        assert (tile / "sce_sys" / "icon0.png").read_bytes() == (PS5 / "assets" / "icon0.png").read_bytes()
        assert (tile / "eboot.bin").is_file()

    @case
    def pareamento():
        status, body = p.post("/api/pair", {"code": "12"})
        assert status == 400
        wrong = "000000" if srv.code() != "000000" else "111111"
        status, body = p.post("/api/pair", {"code": wrong})
        assert status == 400 and "unauthorized" in body["error"]["message"], body
        code = srv.code()
        status, body = p.post("/api/pair", {"code": code[:3] + " " + code[3:]})
        assert status == 200, body
        st = p.get("/api/status")[1]
        assert st["mac"]["paired"] is True
        saved = json.loads((console / "data" / "ponte" / "config.json").read_text())
        assert len(saved["mac"]["token"]) == 64

    @case
    def catalogo_anotado():
        status, cat = p.get("/api/catalog")
        assert status == 200, cat
        kinds = sorted(it["kind"] for it in cat["items"])
        assert kinds == ["exfat", "ffpkg", "folder", "pkg_ps4"], kinds
        assert all(it["state"] == "new" for it in cat["items"]), [it["state"] for it in cat["items"]]
        assert [s["id"] for s in cat["storages"]] == ["internal", "usb0"]
        ctx["cat"] = cat
        status, icon = p.get("/api/icon?id=" + by_kind(cat, "folder")["id"])
        assert status == 200 and icon.startswith(b"\x89PNG")

    @case
    def copia_pasta_para_o_interno():
        folder = by_kind(ctx["cat"], "folder")
        status, r = p.post("/api/copy", {"ids": [folder["id"]], "storage": "internal"})
        assert status == 200 and r["added"] == 1, r
        j = wait_job(p, folder["id"])
        assert j["state"] == "done", j
        assert "speed" in j and "eta_s" in j
        # campos novos: etapa (só durante a cópia), arquivo, início
        for k in ("phase", "phase_done", "phase_total", "phase_speed", "file", "file_index", "file_count",
                  "started_ms", "connections", "requests"):
            assert k in j, (k, j)
        assert j["phase"] is None and j["started_ms"] > 1.6e12, j
        nfiles = len(Client("127.0.0.1", srv.port, token=token()).item(folder["id"])["files"])
        assert nfiles >= 4 and j["file_count"] == nfiles and j["file_index"] == nfiles, (nfiles, j)
        # keep-alive: todos os arquivos pela mesma conexão (não uma por arquivo)
        assert j["requests"] == nfiles and j["connections"] < nfiles, j
        assert j["connections"] == 1, j
        assert f"1 conexões, {nfiles} requisições" in j["stats"], j["stats"]
        final = console / "internal" / "homebrew" / "PPSA01234"
        src = {k: v for k, v in tree_files(lib / "PPSA01234").items()
               if not k.endswith(".DS_Store") and "/._" not in "/" + k}
        assert tree_files(final) == src
        assert not (console / "internal" / "ponte" / "tmp" / folder["id"]).exists()
        # o Mac recebeu o report e o catálogo agora mostra o jogo como instalado
        wait_until(lambda: any("Jogo Cinco: copied" in line for line in srv.output), timeout=5)
        cat = p.get("/api/catalog")[1]
        assert by_kind(cat, "folder")["state"] == "installed"

    @case
    def queda_pausa_e_continua():
        exfat = by_kind(ctx["cat"], "exfat")
        p.post("/api/debug/fail_after", {"bytes": 700_000})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0"})
        # a conexão "cai": a cópia fica pausada (continua sozinha depois), não falha
        j = wait_job(p, exfat["id"], seq, states=("paused", "failed", "done"))
        assert j["state"] == "paused" and not j["manual_pause"], j
        tmp = console / "usb0" / "ponte" / "tmp" / exfat["id"]
        part = tmp / "data" / "PPSA05678.exfat"
        assert part.is_file() and 0 < part.stat().st_size < exfat["size"]
        assert 0 < j["done"] < exfat["size"]
        assert not (console / "usb0" / "homebrew").exists()  # nada pela metade em homebrew/
        # "Continuar" não espera a nova tentativa automática
        p.post("/api/resume", {"seq": j["seq"]})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done", j
        assert (console / "usb0" / "homebrew" / "PPSA05678.exfat").read_bytes() == (lib / "PPSA05678.exfat").read_bytes()
        assert not tmp.exists()

    @case
    def destino_ocupado_e_substituir():
        exfat = by_kind(ctx["cat"], "exfat")
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0"})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "failed" and j["error"] == "destination_exists", j
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0", "replace": True})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done", j

    @case
    def blocos_pequenos_e_medicoes():
        # buffers pequenos: o arquivo passa por vários buffers e dá a volta no anel
        status, st = p.post("/api/settings", {"chunk_kb": 64, "rcvbuf_kb": 256})
        assert status == 200 and st["chunk_kb"] == 64 and st["rcvbuf_kb"] == 256, st
        exfat = by_kind(ctx["cat"], "exfat")
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0", "replace": True})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done", j
        assert (console / "usb0" / "homebrew" / "PPSA05678.exfat").read_bytes() == (lib / "PPSA05678.exfat").read_bytes()
        assert j["stats"] and "gargalo" in j["stats"] and "bloco 64 KB" in j["stats"], j
        status, st = p.post("/api/settings", {"chunk_kb": 4096, "rcvbuf_kb": 4096})
        assert st["chunk_kb"] == 4096 and st["rcvbuf_kb"] == 4096

    @case
    def etapas_ao_vivo():
        # cópia lenta (blocos de 64 KB, 120 ms cada): dá para ver a etapa, o arquivo e a velocidade
        p.post("/api/settings", {"chunk_kb": 64})
        p.post("/api/debug/slow", {"ms": 120})
        try:
            exfat = by_kind(ctx["cat"], "exfat")
            seq = last_seq(p)
            p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0", "replace": True})
            seen = []

            def downloading():
                j = job_for(p, exfat["id"], seq)
                if j:
                    seen.append(j)
                return j if j and j["phase"] == "download" and j["done"] > 0 and j["speed"] > 0 else None
            j = wait_until(downloading, timeout=20)
            assert j["state"] == "running", j
            assert j["phase_total"] == exfat["size"] and 0 < j["phase_done"] <= exfat["size"], j
            assert j["phase_speed"] > 0 and j["eta_s"] is not None, j
            assert j["file"] == "PPSA05678.exfat" and j["file_index"] == 1 and j["file_count"] == 1, j
            phases = {x["phase"] for x in seen}
            assert phases <= {None, "query", "download", "verify", "verify_partial", "move"}, phases
        finally:
            p.post("/api/debug/slow", {"ms": 0})
            p.post("/api/settings", {"chunk_kb": 4096})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done" and j["phase"] is None and j["speed"] == 0, j

    @case
    def hash_errado_baixa_de_novo_uma_vez():
        exfat = by_kind(ctx["cat"], "exfat")
        # o Mac precisa conhecer o sha256 para dar para conferir
        cli = Client("127.0.0.1", srv.port, token=token())
        wait_until(lambda: cli.item(exfat["id"])["files"][0]["sha256"], timeout=15)
        dest = console / "usb0" / "homebrew" / "PPSA05678.exfat"
        # um arquivo estragado no caminho: baixa de novo sozinho e termina
        p.post("/api/debug/corrupt", {"files": 1})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0", "replace": True})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done", j
        assert dest.read_bytes() == (lib / "PPSA05678.exfat").read_bytes()
        assert any("baixando de novo" in line for line in p.output), p.output[-10:]
        # estragado duas vezes: falha com mensagem honesta e não deixa nada pela metade
        p.post("/api/debug/corrupt", {"files": 2})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "usb0", "replace": True})
        j = wait_job(p, exfat["id"], seq)
        p.post("/api/debug/corrupt", {"files": 0})
        assert j["state"] == "failed" and j["error"] == "hash_mismatch", j
        assert "duas vezes" in j["message"] and "será baixado" not in j["message"], j
        assert not (console / "usb0" / "ponte" / "tmp" / exfat["id"] / "data" / "PPSA05678.exfat").exists()
        assert dest.read_bytes() == (lib / "PPSA05678.exfat").read_bytes()  # o antigo continua lá

    @case
    def ids_estranhos_sao_recusados():
        status, r = p.post("/api/copy", {"ids": ["abc\r\nX-Evil: 1"], "storage": "internal"})
        assert status == 200 and r["added"] == 0 and r["errors"] == ["id de item inválido"], r
        status, _ = p.get("/api/icon?id=" + "a%0d%0aX:%20y")
        assert status == 404
        status, r = p.get("/api/diag/net?item=..%2f..%2fx")
        assert status == 400, r
        status, r = p.get("/api/diag/progress")
        assert status == 200 and r["running"] is False, r

    @case
    def diagnostico_de_rede_e_disco():
        status, r = p.get("/api/diag/disk?storage=usb0&mb=4&block_kb=64")
        assert status == 200 and r["bytes"] == 4 << 20 and r["mb_s"] > 0, r
        assert not (console / "usb0" / "ponte" / "tmp" / "diag.bin").exists()
        status, r = p.get("/api/diag/disk?storage=nao-existe")
        assert status == 400, r
        exfat = by_kind(ctx["cat"], "exfat")
        status, r = p.get(f"/api/diag/net?item={exfat['id']}&mb=1&read_kb=64")
        assert status == 200 and 0 < r["bytes"] <= max(1 << 20, exfat["size"]) and r["recv_calls"] > 0, r
        status, r = p.get(f"/api/diag/net?item={exfat['id']}&mb=1&rcvbuf_kb=0")
        assert status == 200 and r["bytes"] > 0 and r["rcvbuf_kb"] > 0, r
        status, r = p.get("/api/diag/net")
        assert status == 400, r

    @case
    def revisao_muda_no_meio_recomeca():
        exfat = by_kind(ctx["cat"], "exfat")
        p.post("/api/debug/fail_after", {"bytes": 300_000})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "internal"})
        j = wait_job(p, exfat["id"], seq, states=("paused", "failed"))
        assert j["state"] == "paused", j
        p.post("/api/pause", {"seq": j["seq"]})  # segura enquanto o item muda no Mac
        # a imagem é refeita no Mac: a parte já baixada não serve mais
        (lib / "PPSA05678.exfat").write_bytes(os.urandom(2 * 1024 * 1024))
        age(lib / "PPSA05678.exfat")
        wait_until(lambda: p.get("/api/catalog")[1]["items"] and
                   by_kind(p.get("/api/catalog")[1], "exfat")["revision"] != exfat["revision"])
        # copiar de novo o mesmo item retoma o trabalho pausado
        p.post("/api/copy", {"ids": [exfat["id"]], "storage": "internal"})
        j = wait_job(p, exfat["id"], seq)
        assert j["state"] == "done", j
        assert (console / "internal" / "homebrew" / "PPSA05678.exfat").read_bytes() == \
            (lib / "PPSA05678.exfat").read_bytes()

    @case
    def pkg_vai_para_o_instalador():
        pkg = by_kind(ctx["cat"], "pkg_ps4")
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [pkg["id"]], "storage": "internal"})
        j = wait_job(p, pkg["id"], seq)
        assert j["state"] == "done" and "instalação" in j["message"], j
        # o instalador (falso) baixou pela URL com ?token= e nome no fim
        assert (console / "installed" / "velho.pkg").read_bytes() == (lib / "velho.pkg").read_bytes()
        assert not (console / "internal" / "ponte" / "tmp" / pkg["id"]).exists()  # sem área temporária

    @case
    def item_fora_do_catalogo():
        status, r = p.post("/api/copy", {"ids": ["0" * 16], "storage": "internal"})
        assert status == 200 and r["added"] == 0 and r["errors"], r
        status, _ = p.post("/api/cancel", {"seq": 99999})
        assert status == 404

    @case
    def configuracao_sobrevive_ao_reinicio():
        nonlocal p
        status, st = p.post("/api/settings", {"notify_progress": False})
        assert status == 200 and st["notify_progress"] is False
        p.stop()
        p = Payload(console, srv)
        st = wait_until(lambda: (lambda s: s if s["mac"] and s["mac"]["reachable"] else None)(p.get("/api/status")[1]))
        assert st["mac"]["paired"] is True and st["settings"]["notify_progress"] is False
        status, cat = p.get("/api/catalog")
        assert status == 200 and cat["items"]
        ctx["cat"] = cat
        p.post("/api/settings", {"notify_progress": True})

    @case
    def desligou_no_meio_retoma_ao_ligar():
        nonlocal p
        folder = by_kind(ctx["cat"], "folder")
        shutil.rmtree(console / "usb0" / "homebrew" / "PPSA01234", ignore_errors=True)
        p.post("/api/debug/fail_after", {"bytes": 1_500_000})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [folder["id"]], "storage": "usb0"})
        wait_job(p, folder["id"], seq, states=("paused",))
        p.stop()  # "desligou o console" com a cópia pela metade
        saved = json.loads((console / "data" / "ponte" / "queue.json").read_text())
        assert [q["item_id"] for q in saved] == [folder["id"]], saved
        p = Payload(console, srv)
        # volta sozinho, sem abrir a página nem mandar copiar de novo
        j = wait_job(p, folder["id"], 0, timeout=40)
        assert j["state"] == "done", j
        assert any("retomando 1 cópia" in line for line in p.output), p.output[-10:]
        src = {k: v for k, v in tree_files(lib / "PPSA01234").items()
               if not k.endswith(".DS_Store") and "/._" not in "/" + k}
        assert tree_files(console / "usb0" / "homebrew" / "PPSA01234") == src
        assert json.loads((console / "data" / "ponte" / "queue.json").read_text()) == []

    @case
    def pausa_sua_continua_pausada_depois_de_ligar():
        nonlocal p
        p.get("/api/catalog")  # payload recém-ligado: carrega o catálogo
        folder = by_kind(ctx["cat"], "folder")
        shutil.rmtree(console / "internal" / "homebrew" / "PPSA01234", ignore_errors=True)
        p.post("/api/debug/fail_after", {"bytes": 1_000_000})
        seq = last_seq(p)
        p.post("/api/copy", {"ids": [folder["id"]], "storage": "internal", "replace": True})
        j = wait_job(p, folder["id"], seq, states=("paused",))
        status, _ = p.post("/api/pause", {"seq": j["seq"]})  # agora a pausa é sua
        assert status == 200
        p.stop()
        p = Payload(console, srv)
        time.sleep(4)  # tempo de sobra para o Mac ser achado e a fila andar, se fosse andar
        j = job_for(p, folder["id"], 0)
        assert j and j["state"] == "paused" and j["manual_pause"], j
        p.post("/api/resume", {"seq": j["seq"]})
        j = wait_job(p, folder["id"], 0)
        assert j["state"] == "done", j
        assert (console / "internal" / "homebrew" / "PPSA01234" / "data" / "big.bin").read_bytes() == \
            (lib / "PPSA01234" / "data" / "big.bin").read_bytes()

    @case
    def pede_varredura_ao_montador():
        # depois de mover para homebrew/, o payload pede ao ShadowMountPlus uma varredura nova
        # (POST /api/v1/scan com reset_attempts), no endereço dos ajustes.
        import http.server
        import threading
        got = []

        class Smp(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                got.append((self.path, json.loads(body or b"{}")))
                out = b'{"status":0,"scan_queued":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def log_message(self, *a):
                pass

        smp = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Smp)
        threading.Thread(target=smp.serve_forever, daemon=True).start()
        try:
            addr = f"127.0.0.1:{smp.server_address[1]}/api/v1/scan"
            status, st = p.post("/api/settings", {"mounter_rescan": addr})
            assert status == 200 and st["mounter_rescan"] == addr, st
            cat = p.get("/api/catalog")[1]
            folder = by_kind(cat, "folder")
            seq = last_seq(p)
            status, r = p.post("/api/copy", {"ids": [folder["id"]], "storage": "usb0", "replace": True})
            assert status == 200 and r["added"] == 1, r
            j = wait_job(p, folder["id"], seq)
            assert j["state"] == "done", j
            wait_until(lambda: got or None, timeout=10)
            assert got == [("/api/v1/scan", {"reset_attempts": True})], got
        finally:
            p.post("/api/settings", {"mounter_rescan": ""})
            smp.shutdown()

    @case
    def copia_sem_conferencia_sha256():
        # Mac com a conferência desligada (o padrão no app): sem hashes, o console confere só o
        # tamanho e não passa pela fase de conferência.
        p.stop()
        srv2 = ServerProcess(template + " --no-hashes", lib, work / "estado-sem-hash")
        console2 = work / "console-sem-hash"
        (console2 / "usb0").mkdir(parents=True)
        p2 = Payload(console2, srv2)
        try:
            wait_until(lambda: (lambda s: s if s["mac"] and s["mac"]["reachable"] else None)(p2.get("/api/status")[1]))
            status, body = p2.post("/api/pair", {"code": srv2.code()})
            assert status == 200, body
            cat = p2.get("/api/catalog")[1]
            folder = by_kind(cat, "folder")
            tok = json.loads((console2 / "data" / "ponte" / "config.json").read_text())["mac"]["token"]
            files = Client("127.0.0.1", srv2.port, token=tok).item(folder["id"])["files"]
            assert all(f.get("sha256") is None for f in files), files
            phases = set()
            status, r = p2.post("/api/copy", {"ids": [folder["id"]], "storage": "internal"})
            assert status == 200 and r["added"] == 1, r

            def done():
                j = job_for(p2, folder["id"], 0)
                if j and j.get("phase"):
                    phases.add(j["phase"])
                return j if j and j["state"] in ("done", "failed", "cancelled") else None
            j = wait_until(done, timeout=30)
            assert j["state"] == "done", j
            assert "verify" not in phases and "verify_partial" not in phases, phases
            src = {k: v for k, v in tree_files(lib / "PPSA01234").items()
                   if not k.endswith(".DS_Store") and "/._" not in "/" + k}
            assert tree_files(console2 / "internal" / "homebrew" / "PPSA01234") == src
        finally:
            p2.stop()
            srv2.stop()

    try:
        pass
    finally:
        p.stop()
        srv.stop()
    return results


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", choices=sorted(PRESETS), default="python")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not PAYLOAD.exists():
        print("compile antes: make host", file=sys.stderr)
        return 2
    work = Path(tempfile.mkdtemp(prefix="ponte-ps5-"))
    print(f"Payload de teste contra o servidor {args.server}")
    try:
        results = run(f"{PRESETS[args.server]} {ARGS}", work)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    failed = [n for n, e in results if e]
    print(f"\n{len(results) - len(failed)}/{len(results)} casos passaram" + (f"; falharam: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
