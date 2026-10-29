# Reference implementation (Python)

**English** · [Português](README.pt-BR.md)

Executable answer key for the [protocol](../protocol/SPEC.en.md). The Swift Mac app must behave the same as this server; the fake client in [`tools/fake_client.py`](../tools/fake_client.py) validates both.

It uses only the Python standard library (3.10+). The tests use `pytest`.

## What is here

| Module | What | SPEC |
|---|---|---|
| `ponte_ref/identify.py` | identifies platform, type and metadata of each item | §1, §7.2 |
| `ponte_ref/formats/` | readers for `param.sfo`, `.pkg`, `param.json`, PFS0 (NSP/NSZ), Switch Title ID, archives | §1.2–§1.5 |
| `ponte_ref/library.py` | library scan, stability, `id`/`revision`, duplicates, hashes | §5.3, §5.4, §7 |
| `ponte_ref/profiles.py` | compatibility profiles per firmware (`../profiles/`) | §1.1.1 |
| `ponte_ref/state.py` | matching against what is already on the console | §5.7 |
| `ponte_ref/exfat_plan.py` | size and cluster of the `.exfat` image | §7.4 |
| `ponte_ref/server.py` | HTTP server + UDP discovery + pairing | §2–§6 |

What is **not** here, because it depends on macOS and lives in the Swift app: extracting archives and creating the `.exfat` image (the reference only computes the plan and lists the commands).

## Running the server

```sh
cd reference
python3 -m ponte_ref.server --library /Volumes/SSD/Jogos
```

It shows the pairing code. Options: `--port` (47101), `--discovery-port` (47100), `--no-pairing`, `--stable-seconds` (10), `--rescan` (5), `--state-dir` (`~/.ponte-ref`).

## Testing with the fake client

In another terminal, at the repository root:

```sh
python3 tools/fake_client.py discover          # finds the server on the network
python3 tools/fake_client.py pair 123456       # code shown by the server
python3 tools/fake_client.py catalog
python3 tools/fake_client.py download <id> --dest ./console-falso
python3 tools/fake_client.py state --installed instalados.json
```

`./console-falso` imitates the console: `ponte/tmp/` is the temporary area, `homebrew/` is where the PS5 mounter would look for games, `installed/` simulates installed packages.

## Tests

```sh
python3 -m pytest reference/tests -q
```

They cover each format with synthetic files, the library scan and the protocol end to end: pairing and attempt limit, catalog, `Range`, resuming after a drop, revision changed mid-copy (`412`), wrong hash, destination already taken, item not ready yet (`409`) and malicious paths.
