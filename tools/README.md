# Tools

**English** · [Português](README.pt-BR.md)

## `fake_client.py`

Pretends to be a console: discovers the server, pairs, lists the catalog, reports the console state and copies items following the client rules (SPEC §8): temporary area outside `homebrew/`, resume with `Range`, revision, size and `sha256` check, and `report` at the end.

It does not depend on the reference implementation, so it can validate any Ponte server, including the Mac app. Usage in [`reference/README.md`](../reference/README.md#testing-with-the-fake-client).

## `conformance.py`

Black-box protocol test: starts a Ponte server as a separate process, on a synthetic library, and checks the SPEC over the network only (discovery, pairing and attempt limit, catalog, details, icon, `HEAD`/`Range`/`416`, revision `412`, `409`, hashes, `console/state` with profiles, `changes`, full copy and resume with the fake client, token that survives a restart, no-pairing mode).

```sh
python3 tools/conformance.py --server python     # reference
python3 tools/conformance.py --server swift      # mac/.build/debug/ponte-server
python3 tools/conformance.py --cmd "meu-servidor {extra} --library {library} --port {port} ..."
```

Any server that accepts the reference's options (`--library`, `--port`, `--discovery-port`, `--state-dir`, `--profiles`, `--rescan`, `--stable-seconds`, `--name`, `--no-pairing`) and prints `Código de pareamento: NNNNNN` on startup can be tested.

## `check_docs.py`

Checks the documentation: each `NAME.md` (English) has its `NAME.pt-BR.md` (Portuguese) and vice versa, both have the language selector, and every relative link and every anchor exist. Outside the pair rule: the SPEC (official in `protocol/SPEC.md`, translation in `protocol/SPEC.en.md`), `CLAUDE.md` and the notes in `docs/releases/`. CI runs it on every push.

```sh
python3 tools/check_docs.py
```
