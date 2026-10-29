#!/usr/bin/env python3
"""Cliente falso do protocolo Ponte: finge ser um console (SPEC §3, §4, §5, §8).

Independente da implementação de referência (só biblioteca padrão), para
validar qualquer servidor — o de referência ou o app do Mac.

Exemplos:
    python tools/fake_client.py discover
    python tools/fake_client.py --host 192.168.0.10 pair 482913
    python tools/fake_client.py catalog
    python tools/fake_client.py state --installed installed.json
    python tools/fake_client.py download <id> --dest ./console-falso

O "console" é uma pasta: <dest>/ponte/tmp/ (área temporária), <dest>/homebrew/
(o que o montador do PS5 veria) e <dest>/installed/ (pacotes "instalados").
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import shutil
import socket
import sys
import time
from pathlib import Path

PROTOCOL = 1
API = "/api/v1"
CHUNK = 1 << 20
CONFIG = Path.home() / ".ponte-fake-client.json"

IMAGE_KINDS = {"exfat", "ffpkg", "ffpfs", "ffpfsc"}
FOLDER_KINDS = {"folder", "folder_ps4"}
INSTALL_KINDS = {"pkg_ps4", "pkg_ps5", "nsp", "nsz", "xci", "xcz"}
CONTENT_ORDER = {"base": 0, "update": 1, "dlc": 2, "unknown": 3}


class PonteError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(f"{status} {code}: {message}")
        self.status, self.code, self.message = status, code, message


class Interrupted(Exception):
    """Simula a conexão caindo no meio da cópia (para testar a retomada)."""


# ---------- descoberta ----------

def discover(target: str = "255.255.255.255", port: int = 47100, timeout: float = 1.0) -> list[dict]:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.2)
    found: dict[str, dict] = {}
    try:
        s.sendto(b"PONTE?1", (target, port))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data, addr = s.recvfrom(1024)
            except socket.timeout:
                continue
            try:
                info = json.loads(data)
            except ValueError:
                continue
            if info.get("ponte") == PROTOCOL and isinstance(info.get("port"), int):
                info["host"] = addr[0]
                found[info.get("id") or f"{addr[0]}:{info['port']}"] = info
    finally:
        s.close()
    return list(found.values())


# ---------- caminhos seguros ----------

def safe_relpath(path: str) -> Path:
    """Rejeita caminhos absolutos, com '..', vazios ou com barra invertida (SPEC §5.4)."""
    if not path or path.startswith("/") or "\\" in path or "\x00" in path:
        raise PonteError(0, "unsafe_path", f"caminho recusado: {path!r}")
    parts = path.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise PonteError(0, "unsafe_path", f"caminho recusado: {path!r}")
    return Path(*parts)


def safe_name(text: str) -> str:
    keep = "".join(c if c.isalnum() or c in " ._-[]()" else "_" for c in text).strip(" .")
    return keep or "item"


# ---------- cliente ----------

class Client:
    def __init__(self, host: str, port: int = 47101, token: str | None = None,
                 platform: str = "ps5", firmware: str = "13.60", timeout: float = 30.0):
        self.host, self.port, self.token = host, port, token
        self.platform, self.firmware = platform, firmware
        self.timeout = timeout
        self._conn: http.client.HTTPConnection | None = None

    def close(self) -> None:
        if self._conn:
            self._conn.close()
            self._conn = None

    def _connection(self) -> http.client.HTTPConnection:
        if self._conn is None:
            self._conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        return self._conn

    def _headers(self, extra=None) -> dict:
        h = {"X-Ponte-Protocol": str(PROTOCOL)}
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        h.update(extra or {})
        return h

    def request(self, method: str, path: str, body=None, headers=None, stream=False):
        data = None
        hdrs = self._headers(headers)
        if body is not None:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
        for attempt in (1, 2):  # uma nova tentativa se a conexão keep-alive tiver caído
            conn = self._connection()
            try:
                conn.request(method, path, body=data, headers=hdrs)
                resp = conn.getresponse()
                break
            except (ConnectionError, http.client.RemoteDisconnected, BrokenPipeError):
                self.close()
                if attempt == 2:
                    raise
        if stream and resp.status in (200, 206):
            return resp
        raw = resp.read()
        if resp.status >= 400:
            try:
                err = json.loads(raw)["error"]
                raise PonteError(resp.status, err["code"], err["message"])
            except (ValueError, KeyError, TypeError):
                raise PonteError(resp.status, "http_error", raw[:200].decode("utf-8", "replace")) from None
        if not raw:
            return None
        return json.loads(raw) if resp.getheader("Content-Type", "").startswith("application/json") else raw

    # rotas
    def info(self) -> dict:
        return self.request("GET", f"{API}/info")

    def pair(self, code: str, name: str = "Console falso") -> str:
        r = self.request("POST", f"{API}/pair", {"code": code, "client": {
            "platform": self.platform, "name": name, "version": "0.1.0", "firmware": self.firmware}})
        self.token = r["client_token"] or None
        return r["client_token"]

    def catalog(self) -> dict:
        return self.request("GET", f"{API}/catalog?platform={self.platform}")

    def item(self, item_id: str) -> dict:
        return self.request("GET", f"{API}/items/{item_id}")

    def changes(self, since: str) -> dict:
        return self.request("GET", f"{API}/catalog/changes?since={since}")

    def console_state(self, installed: list[dict], storages: list[dict]) -> dict:
        return self.request("POST", f"{API}/console/state", {
            "platform": self.platform, "firmware": self.firmware,
            "storages": storages, "installed": installed})

    def report(self, item_id: str, result: str, storage: str | None = None,
               detail: str | None = None, duration_s: int | None = None) -> None:
        self.request("POST", f"{API}/items/{item_id}/report", {
            "result": result, "storage": storage, "detail": detail, "duration_s": duration_s})

    # ---------- cópia (SPEC §8) ----------

    def download(self, item_id: str, dest: Path, storage: str = "internal", replace: bool = False,
                 max_bytes: int | None = None, progress=None) -> dict:
        """Copia um item para o "console" em `dest`.

        `max_bytes` interrompe depois de N bytes baixados nesta chamada (simula
        queda de conexão); chamar de novo retoma de onde parou.
        """
        started = time.monotonic()
        try:
            return self._download(item_id, Path(dest), storage, replace, max_bytes, progress, started)
        except Interrupted:
            raise
        except PonteError as e:
            self._safe_report(item_id, "failed", storage, f"{e.code}: {e.message}", started)
            raise
        except OSError as e:
            self._safe_report(item_id, "failed", storage, str(e), started)
            raise

    def _safe_report(self, item_id, result, storage, detail, started):
        try:
            self.report(item_id, result, storage, detail, int(time.monotonic() - started))
        except (PonteError, OSError):
            pass

    def _download(self, item_id, dest, storage, replace, max_bytes, progress, started):
        detail = self.item(item_id)
        kind = detail["kind"]
        files = detail["files"]
        rels = [safe_relpath(f["path"]) for f in files]

        tmp = dest / "ponte" / "tmp" / item_id
        rev_file = tmp / ".revision"
        data_dir = tmp / "data"
        if tmp.exists() and (not rev_file.is_file() or rev_file.read_text().strip() != detail["revision"]):
            shutil.rmtree(tmp)
        data_dir.mkdir(parents=True, exist_ok=True)
        rev_file.write_text(detail["revision"])

        final = self._final_path(dest, detail, rels)
        if final.exists() and not replace:
            raise PonteError(0, "destination_exists", f"já existe: {final}")

        have = sum((data_dir / r).stat().st_size for r in rels if (data_dir / r).is_file())
        remaining = max(0, detail["size"] - have)
        free = shutil.disk_usage(dest).free
        if free < remaining * 1.05:
            raise PonteError(0, "no_space", f"espaço insuficiente: precisa {remaining}, livre {free}")

        budget = [max_bytes]
        for f, rel in zip(files, rels):
            target = data_dir / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            self._fetch_file(item_id, detail["revision"], f, target, budget, progress)

        # tudo conferido: move para o lugar final (renomeação no mesmo disco)
        final.parent.mkdir(parents=True, exist_ok=True)
        if final.exists():
            shutil.rmtree(final) if final.is_dir() else final.unlink()
        if kind in FOLDER_KINDS:
            data_dir.rename(final)
        else:
            (data_dir / rels[0]).rename(final)
        shutil.rmtree(tmp, ignore_errors=True)

        result = "copied" if kind in IMAGE_KINDS | FOLDER_KINDS else "installed"
        self.report(item_id, result, storage, None, int(time.monotonic() - started))
        return {"result": result, "final": str(final), "kind": kind, "size": detail["size"]}

    @staticmethod
    def _final_path(dest: Path, detail: dict, rels: list[Path]) -> Path:
        kind = detail["kind"]
        if kind in FOLDER_KINDS:
            return dest / "homebrew" / safe_name(detail.get("title_id") or detail["title"])
        if kind in IMAGE_KINDS:
            return dest / "homebrew" / safe_name(rels[0].name)
        return dest / "installed" / safe_name(rels[0].name)

    def _fetch_file(self, item_id, revision, f, target: Path, budget, progress) -> None:
        size = f["size"]
        have = target.stat().st_size if target.is_file() else 0
        if have > size:
            target.unlink()
            have = 0
        expected_sha = f.get("sha256")
        if have < size:
            headers = {"X-Ponte-Revision": revision}
            if have:
                headers["Range"] = f"bytes={have}-"
            resp = self.request("GET", f"{API}/items/{item_id}/files/{f['index']}", headers=headers, stream=True)
            if have and resp.status != 206:
                resp.read()
                raise PonteError(resp.status, "no_range", "servidor ignorou o Range")
            expected_sha = expected_sha or resp.getheader("X-Ponte-Sha256")
            with target.open("ab") as out:
                while True:
                    want = CHUNK
                    if budget[0] is not None:
                        if budget[0] <= 0:
                            self.close()  # derruba a conexão no meio, como uma queda real
                            raise Interrupted(f"interrompido em {target.name} ({out.tell()} bytes)")
                        want = min(want, budget[0])
                    chunk = resp.read(want)
                    if not chunk:
                        break
                    out.write(chunk)
                    if budget[0] is not None:
                        budget[0] -= len(chunk)
                    if progress:
                        progress(f["path"], out.tell(), size)
        if target.stat().st_size != size:
            raise PonteError(0, "size_mismatch", f"{f['path']}: tamanho {target.stat().st_size} != {size}")
        if expected_sha:
            h = hashlib.sha256()
            with target.open("rb") as fh:
                while chunk := fh.read(CHUNK):
                    h.update(chunk)
            if h.hexdigest() != expected_sha:
                target.unlink()
                raise PonteError(0, "hash_mismatch", f"{f['path']}: sha256 não confere")


# ---------- linha de comando ----------

def _load_config() -> dict:
    try:
        return json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        return {}


def _save_config(cfg: dict) -> None:
    CONFIG.write_text(json.dumps(cfg, indent=2))


def _human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return str(n)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Cliente falso do protocolo Ponte")
    ap.add_argument("--host")
    ap.add_argument("--port", type=int)
    ap.add_argument("--platform", choices=["ps5", "switch"])
    ap.add_argument("--firmware")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("discover")
    p = sub.add_parser("pair")
    p.add_argument("code")
    sub.add_parser("catalog")
    p = sub.add_parser("state")
    p.add_argument("--installed", type=Path, help="JSON com a lista 'installed'")
    p = sub.add_parser("download")
    p.add_argument("item_id")
    p.add_argument("--dest", type=Path, default=Path("console-falso"))
    p.add_argument("--replace", action="store_true")
    args = ap.parse_args(argv)

    cfg = _load_config()
    for k in ("host", "port", "platform", "firmware"):
        if getattr(args, k) is not None:
            cfg[k] = getattr(args, k)
    cfg.setdefault("port", 47101)
    cfg.setdefault("platform", "ps5")
    cfg.setdefault("firmware", "13.60")

    if args.cmd == "discover":
        found = discover()
        if not found:
            print("Nenhum servidor Ponte encontrado na rede.")
            return 1
        for s in found:
            print(f"{s['name']}  {s['host']}:{s['port']}  (id {s['id']})")
        if len(found) == 1:
            cfg["host"], cfg["port"] = found[0]["host"], found[0]["port"]
            _save_config(cfg)
            print("Servidor salvo na configuração.")
        return 0

    if not cfg.get("host"):
        print("Informe --host ou rode 'discover' antes.", file=sys.stderr)
        return 2
    c = Client(cfg["host"], cfg["port"], cfg.get("token"), cfg["platform"], cfg["firmware"])
    try:
        if args.cmd == "pair":
            cfg["token"] = c.pair(args.code)
            _save_config(cfg)
            print("Pareado.")
        elif args.cmd == "catalog":
            cat = c.catalog()
            for it in cat["items"]:
                warn = f"  ⚠ {', '.join(it['warnings'])}" if it["warnings"] else ""
                print(f"{it['id']}  {it['kind']:<10} {it['content']:<7} {_human(it['size']):>9}  "
                      f"{it['title']} ({it['title_id'] or '?'} {it['version'] or ''}){warn}")
        elif args.cmd == "state":
            installed = json.loads(args.installed.read_text()) if args.installed else []
            res = c.console_state(installed, [])
            for it in res["items"]:
                reason = f" — {it['state_reason']}" if it["state_reason"] else ""
                print(f"{it['state']:<17} {it['title']} ({it['kind']}){reason}")
        elif args.cmd == "download":
            last = [0.0]

            def prog(path, done, total):
                now = time.monotonic()
                if now - last[0] > 0.5 or done == total:
                    last[0] = now
                    line = f"{path}: {_human(done)} / {_human(total)}"
                    print(f"\r{line[:78]:<78}", end="\n" if done == total else "", flush=True)

            r = c.download(args.item_id, args.dest, replace=args.replace, progress=prog)
            print(f"{r['result']}: {r['final']}")
    except PonteError as e:
        print(f"Erro: {e}", file=sys.stderr)
        return 1
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
