"""Servidor de referência do protocolo Ponte (SPEC §2 a §6).

Uso:
    python -m ponte_ref.server --library /Volumes/SSD/Jogos

Só biblioteca padrão do Python. Serve de gabarito para o servidor Swift do Mac.
"""

from __future__ import annotations

import argparse
import json
import re
import secrets
import socket
import threading
import time
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from . import PROTOCOL_VERSION, __version__
from .library import Library
from .profiles import load_profiles, select_profile
from .state import annotate, profile_warnings

API = "/api/v1"
MAX_BODY = 1 << 20
SEND_CHUNK = 1 << 20
PAIR_MAX_FAILS = 5
PAIR_WINDOW = 60.0
DEFAULT_PORT = 47101
DEFAULT_DISCOVERY_PORT = 47100
DISCOVERY_QUERY = b"PONTE?1"

ROUTE_ITEM = re.compile(rf"^{API}/items/([0-9a-f]{{16}})$")
# o nome no fim (opcional) é só para quem baixa enxergar a extensão, ex.: o instalador do PS5
ROUTE_FILE = re.compile(rf"^{API}/items/([0-9a-f]{{16}})/files/(\d+)(?:/[^/]+)?$")
ROUTE_ICON = re.compile(rf"^{API}/items/([0-9a-f]{{16}})/icon$")
ROUTE_REPORT = re.compile(rf"^{API}/items/([0-9a-f]{{16}})/report$")
RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status, self.code, self.message = status, code, message


def parse_range(header: str, size: int) -> tuple[int, int]:
    m = RANGE_RE.match(header.strip())
    if not m or (not m.group(1) and not m.group(2)):
        raise ApiError(416, "bad_range", "Range inválido")
    if m.group(1):
        start = int(m.group(1))
        end = int(m.group(2)) if m.group(2) else size - 1
    else:  # sufixo: bytes=-N
        n = int(m.group(2))
        start, end = max(0, size - n), size - 1
    end = min(end, size - 1)
    if start >= size or start > end:
        raise ApiError(416, "bad_range", "Range fora do arquivo")
    return start, end


def parse_iso(text: str) -> float:
    try:
        return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()
    except (TypeError, ValueError):
        raise ApiError(400, "bad_request", "data inválida; use AAAA-MM-DDTHH:MM:SSZ") from None


class PonteServer:
    def __init__(self, library: Library, profiles_dir: Path, state_dir: Path, name: str,
                 host: str = "0.0.0.0", port: int = DEFAULT_PORT,
                 discovery_host: str = "0.0.0.0", discovery_port: int | None = DEFAULT_DISCOVERY_PORT,
                 pairing: bool = True, rescan_interval: float = 5.0, log=print, hashes: bool = True):
        self.library = library
        self.profiles = load_profiles(profiles_dir)
        self.state_dir = Path(state_dir)
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.name = name
        self.pairing = pairing
        self.rescan_interval = rescan_interval
        # sha256 é opcional (SPEC §7.5): sem ele, os consoles conferem só o tamanho
        self.hashes = hashes
        self.log = log
        self._lock = threading.Lock()
        self._fails: dict[str, list[float]] = {}
        self._stop = threading.Event()
        self.reports: list[dict] = []

        self.server_id = self._load_server_id()
        self.tokens: dict[str, dict] = self._load_json("tokens.json", {})
        self.code = self._new_code()

        self.httpd = ThreadingHTTPServer((host, port), _Handler)
        self.httpd.daemon_threads = True
        self.httpd.ponte = self
        self.port = self.httpd.server_address[1]

        self.udp = None
        if discovery_port is not None:
            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.udp.bind((discovery_host, discovery_port))
            self.udp.settimeout(0.5)
        self.discovery_port = self.udp.getsockname()[1] if self.udp else None
        self._threads: list[threading.Thread] = []

    # ---------- estado persistente ----------

    def _load_json(self, name, default):
        try:
            return json.loads((self.state_dir / name).read_text("utf-8"))
        except (OSError, ValueError):
            return default

    def _save_json(self, name, data):
        tmp = self.state_dir / (name + ".tmp")
        tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False), "utf-8")
        tmp.replace(self.state_dir / name)

    def _load_server_id(self) -> str:
        data = self._load_json("server.json", {})
        sid = data.get("id")
        if not (isinstance(sid, str) and re.fullmatch(r"[0-9a-f]{16}", sid)):
            sid = secrets.token_hex(8)
            self._save_json("server.json", {"id": sid})
        return sid

    @staticmethod
    def _new_code() -> str:
        return f"{secrets.randbelow(1_000_000):06d}"

    # ---------- ciclo de vida ----------

    def start(self) -> None:
        self.library.scan()
        targets = [self.httpd.serve_forever, self._background]
        if self.udp:
            targets.append(self._discovery_loop)
        for t in targets:
            th = threading.Thread(target=t, daemon=True)
            th.start()
            self._threads.append(th)

    def stop(self) -> None:
        self._stop.set()
        self.httpd.shutdown()
        self.httpd.server_close()
        if self.udp:
            self.udp.close()
        for th in self._threads:
            th.join(timeout=2)

    def _background(self) -> None:
        while not self._stop.is_set():
            try:
                if self.hashes:
                    self.library.hash_pending(self._stop)
            except Exception as e:  # nunca derrubar o servidor por causa de um arquivo
                self.log(f"[hash] erro: {e}")
            if self._stop.wait(self.rescan_interval):
                return
            try:
                self.library.scan()
            except Exception as e:
                self.log(f"[scan] erro: {e}")

    def _discovery_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data, addr = self.udp.recvfrom(512)
            except (socket.timeout, OSError):
                continue
            if data.strip() != DISCOVERY_QUERY:
                continue
            reply = json.dumps({"ponte": PROTOCOL_VERSION, "name": self.name, "port": self.port,
                                "id": self.server_id}).encode()
            try:
                self.udp.sendto(reply, addr)
            except OSError:
                pass

    # ---------- pareamento ----------

    def pair(self, ip: str, body: dict) -> dict:
        now = time.time()
        with self._lock:
            fails = [t for t in self._fails.get(ip, []) if now - t < PAIR_WINDOW]
            self._fails[ip] = fails
            if len(fails) >= PAIR_MAX_FAILS:
                raise ApiError(429, "too_many_attempts", "tentativas demais; aguarde 1 minuto")
            code = body.get("code")
            if not isinstance(code, str) or not secrets.compare_digest(code, self.code):
                fails.append(now)
                raise ApiError(401, "unauthorized", "código de pareamento errado")
            client = body.get("client") if isinstance(body.get("client"), dict) else {}
            platform = client.get("platform")
            if platform not in ("ps5", "switch"):
                raise ApiError(400, "bad_request", "client.platform deve ser ps5 ou switch")
            token = secrets.token_hex(32)
            self.tokens[token] = {
                "platform": platform,
                "name": str(client.get("name", ""))[:64],
                "version": str(client.get("version", ""))[:32],
                "firmware": client.get("firmware") if isinstance(client.get("firmware"), str) else None,
                "paired_at": int(now),
            }
            self._save_json("tokens.json", self.tokens)
            self._fails.pop(ip, None)
            self.code = self._new_code()
            self.log(f"[pair] {self.tokens[token]['name'] or ip} pareado ({platform}). Novo código: {self.code}")
            return {"client_token": token}

    def revoke(self, token: str) -> None:
        with self._lock:
            self.tokens.pop(token, None)
            self._save_json("tokens.json", self.tokens)

    def client_for(self, auth_header: str | None) -> dict | None:
        if not self.pairing:
            return {}
        if not auth_header or not auth_header.startswith("Bearer "):
            return None
        return self.tokens.get(auth_header[7:].strip())

    def update_client_firmware(self, auth_header: str | None, firmware: str | None) -> None:
        if not self.pairing or not auth_header or not isinstance(firmware, str):
            return
        with self._lock:
            info = self.tokens.get(auth_header[7:].strip())
            if info is not None and info.get("firmware") != firmware:
                info["firmware"] = firmware
                self._save_json("tokens.json", self.tokens)


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = f"ponte-ref/{__version__}"

    @property
    def ponte(self) -> PonteServer:
        return self.server.ponte

    def log_message(self, fmt, *args):
        pass

    # ---------- utilidades ----------

    def _send_json(self, status: int, payload: dict | None, head_only=False) -> None:
        body = b"" if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        if payload is not None:
            self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head_only and body:
            self.wfile.write(body)

    def _error(self, err: ApiError) -> None:
        self._send_json(err.status, {"error": {"code": err.code, "message": err.message}},
                        head_only=self.command == "HEAD")

    def _read_body(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ApiError(400, "bad_request", "Content-Length inválido") from None
        if length > MAX_BODY:
            self.close_connection = True
            raise ApiError(400, "bad_request", "corpo grande demais")
        raw = self.rfile.read(length) if length else b""
        if not raw:
            return {}
        try:
            data = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ApiError(400, "bad_request", "JSON inválido") from None
        if not isinstance(data, dict):
            raise ApiError(400, "bad_request", "o corpo deve ser um objeto JSON")
        return data

    def _require_client(self) -> dict:
        client = self.ponte.client_for(self.headers.get("Authorization"))
        if client is None:
            raise ApiError(401, "unauthorized", "pareamento necessário")
        return client

    def _item(self, iid: str):
        item = self.ponte.library.get(iid)
        if item is None:
            if iid in self.ponte.library.pending_ids():
                raise ApiError(409, "item_not_ready", "item ainda sendo preparado")
            raise ApiError(404, "not_found", "item não encontrado")
        return item

    def _check_protocol(self) -> None:
        v = self.headers.get("X-Ponte-Protocol")
        if v is not None and v.strip() != str(PROTOCOL_VERSION):
            raise ApiError(426, "protocol_mismatch", f"servidor fala o protocolo {PROTOCOL_VERSION}")

    # ---------- entrada ----------

    def do_GET(self):
        self._dispatch()

    def do_HEAD(self):
        self._dispatch()

    def do_POST(self):
        self._dispatch()

    def _dispatch(self):
        url = urlsplit(self.path)
        path, query = url.path.rstrip("/") or "/", parse_qs(url.query)
        body = None
        try:
            if self.command == "POST":
                body = self._read_body()
            self._check_protocol()
            self._route(self.command, path, query, body)
        except ApiError as e:
            self._error(e)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def _route(self, method: str, path: str, query: dict, body: dict | None) -> None:
        get = method in ("GET", "HEAD")
        head = method == "HEAD"
        p = self.ponte

        if path == f"{API}/info" and get:
            return self._send_json(200, {"protocol": PROTOCOL_VERSION, "server": f"ponte-ref/{__version__}",
                                         "name": p.name, "id": p.server_id,
                                         "pairing_required": p.pairing}, head)
        if path == f"{API}/pair" and method == "POST":
            if not p.pairing:
                return self._send_json(200, {"client_token": ""})
            return self._send_json(200, p.pair(self.client_address[0], body))

        auth = self.headers.get("Authorization")
        if not auth and get and ROUTE_FILE.match(path) and query.get("token"):
            # rotas de arquivo também aceitam ?token= (SPEC §5.5): para quem baixa não manda cabeçalho
            auth = "Bearer " + query["token"][0]
        client = self.ponte.client_for(auth)
        if client is None:
            raise ApiError(401, "unauthorized", "pareamento necessário")

        if path == f"{API}/catalog" and get:
            platform = (query.get("platform") or [client.get("platform")])[0]
            if platform not in ("ps5", "switch"):
                raise ApiError(400, "bad_request", "informe platform=ps5 ou platform=switch")
            profile, guessed = select_profile(p.profiles, platform, client.get("firmware"))
            items = []
            for it in p.library.items(platform):
                d = it.summary()
                d["warnings"] = list(dict.fromkeys(d["warnings"] + profile_warnings(it.ident.kind, profile, guessed)))
                items.append(d)
            return self._send_json(200, {"generated_at": p.library.generated_at, "items": items}, head)

        if path == f"{API}/catalog/changes" and get:
            since_text = (query.get("since") or [None])[0]
            since = parse_iso(since_text)
            platform = (query.get("platform") or [client.get("platform")])[0]
            new = sum(1 for it in p.library.items(platform if platform in ("ps5", "switch") else None)
                      if it.added_ts > since)
            return self._send_json(200, {"changed": new > 0, "new_items": new,
                                         "generated_at": p.library.generated_at}, head)

        if path == f"{API}/console/state" and method == "POST":
            platform = body.get("platform") or client.get("platform")
            if platform not in ("ps5", "switch"):
                raise ApiError(400, "bad_request", "platform deve ser ps5 ou switch")
            if not isinstance(body.get("installed", []), list) or not isinstance(body.get("storages", []), list):
                raise ApiError(400, "bad_request", "installed e storages devem ser listas")
            console = dict(body, platform=platform)
            p.update_client_firmware(self.headers.get("Authorization"), body.get("firmware"))
            items = annotate(p.library.items(platform), console, p.profiles)
            return self._send_json(200, {"generated_at": p.library.generated_at, "items": items})

        m = ROUTE_ITEM.match(path)
        if m and get:
            return self._send_json(200, self._item(m.group(1)).detail(), head)

        m = ROUTE_FILE.match(path)
        if m and get:
            return self._send_file(self._item(m.group(1)), int(m.group(2)), head)

        m = ROUTE_ICON.match(path)
        if m and get:
            return self._send_icon(self._item(m.group(1)), head)

        m = ROUTE_REPORT.match(path)
        if m and method == "POST":
            item = self._item(m.group(1))
            result = body.get("result")
            if result not in ("installed", "copied", "failed", "cancelled"):
                raise ApiError(400, "bad_request", "result inválido")
            entry = {"item": item.id, "title": item.ident.title, "result": result,
                     "storage": body.get("storage"), "detail": body.get("detail"),
                     "duration_s": body.get("duration_s"), "client": client.get("name"),
                     "at": int(time.time())}
            p.reports.append(entry)
            p.log(f"[report] {entry['title']}: {result}" + (f" ({entry['detail']})" if entry["detail"] else ""))
            return self._send_json(204, None)

        raise ApiError(404, "not_found", "rota não encontrada")

    def _send_file(self, item, index: int, head: bool) -> None:
        if index >= len(item.files):
            raise ApiError(404, "not_found", "arquivo não encontrado")
        f = item.files[index]
        sent_rev = self.headers.get("X-Ponte-Revision")
        if sent_rev is not None and sent_rev.strip() != item.revision:
            raise ApiError(412, "revision_changed", "o item mudou; recomece a cópia")
        try:
            st = f.abs_path.stat()
        except OSError:
            raise ApiError(404, "not_found", "arquivo sumiu do disco") from None
        if (st.st_size, st.st_mtime_ns) != (f.size, f.mtime_ns):
            raise ApiError(412, "revision_changed", "o arquivo mudou no disco; recomece a cópia")

        size = f.size
        rng = self.headers.get("Range")
        if rng:
            start, end = parse_range(rng, size)
            status = 206
        else:
            start, end, status = 0, size - 1, 200
        length = end - start + 1 if size else 0

        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("X-Ponte-Revision", item.revision)
        if f.sha256:
            self.send_header("X-Ponte-Sha256", f.sha256)
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head or length == 0:
            return
        with f.abs_path.open("rb") as fh:
            fh.seek(start)
            remaining = length
            while remaining > 0:
                chunk = fh.read(min(SEND_CHUNK, remaining))
                if not chunk:
                    self.close_connection = True  # arquivo encolheu no meio do envio
                    return
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def _send_icon(self, item, head: bool) -> None:
        data = item.ident.icon_bytes
        if data is None and item.ident.icon_path is not None:
            try:
                data = item.ident.icon_path.read_bytes()
            except OSError:
                data = None
        if not data:
            raise ApiError(404, "not_found", "item sem ícone")
        ctype = "image/png" if data.startswith(b"\x89PNG") else "image/jpeg"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if not head:
            self.wfile.write(data)


def main(argv=None) -> None:
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description="Servidor de referência do protocolo Ponte")
    ap.add_argument("--library", required=True, type=Path, help="pasta da biblioteca de jogos")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--discovery-port", type=int, default=DEFAULT_DISCOVERY_PORT)
    ap.add_argument("--name", default=socket.gethostname())
    ap.add_argument("--no-pairing", action="store_true", help='modo "confiar na rede local"')
    ap.add_argument("--state-dir", type=Path, default=Path.home() / ".ponte-ref")
    ap.add_argument("--profiles", type=Path, default=repo / "profiles")
    ap.add_argument("--stable-seconds", type=float, default=10.0)
    ap.add_argument("--rescan", type=float, default=5.0, help="intervalo entre varreduras (s)")
    ap.add_argument("--no-hashes", action="store_true", help="não calcular sha256 (consoles conferem só o tamanho)")
    args = ap.parse_args(argv)

    lib = Library(args.library, stable_seconds=args.stable_seconds)
    srv = PonteServer(lib, args.profiles, args.state_dir, args.name, port=args.port,
                      discovery_port=args.discovery_port, pairing=not args.no_pairing,
                      rescan_interval=args.rescan, hashes=not args.no_hashes)
    srv.start()
    print(f"Ponte (referência) em http://0.0.0.0:{srv.port}  — descoberta UDP {srv.discovery_port}")
    print(f"Biblioteca: {args.library}  ({len(lib.items())} itens prontos)")
    if srv.pairing:
        print(f"Código de pareamento: {srv.code}")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.stop()


if __name__ == "__main__":
    main()
