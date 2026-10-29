# Ponte

**English** · [Português](README.pt-BR.md)

A game library on your Mac, copied over the local network to a PS5 and a Switch.

Your games live in a folder on an external SSD connected to the Mac. Whenever you want, you turn on the console, open the Ponte client, see what is on the Mac and what is already on the console, and pick what to copy and where. The game is copied and stays on the console; nothing runs over the network while you play.

Formats: PS5 as a folder, `.ffpkg`, `.exfat`, `.ffpfs`, `.ffpfsc` and `.pkg` (PS5 and PS4, including updates and DLC); Switch as `.nsp`, `.nsz`, `.xci` and `.xcz`. Archives (`.zip`, `.rar`, `.7z`) you extract yourself inside the library; automatic extraction and conversion of PS5 folders to `.exfat` are available as options in the Mac app's settings, off by default. Full list in [`protocol/SPEC.en.md`](protocol/SPEC.en.md#1-supported-formats).

> Ponte only copies files that are already in your library. It does not include games, keys, firmware or tools to unlock consoles, and it is not affiliated with Sony or Nintendo. Use it with games and content you have the right to use. Details in [`LEGAL.md`](LEGAL.md).

## How it works

```
 Mac: SSD ─► watch ─► extract ─► identify ─► convert ─► catalog
                                                          ▲
                          HTTP, port 47101 (the console pulls)
                                                          │
   PS5 (Ponte payload) ── copies to homebrew/ or installs .pkg ┤
   Switch (Ponte app) ───────── installs nsp/nsz/xci/xcz ───────┘
```

The console always opens the connection; the Mac only serves. Ports: `47100` (UDP discovery), `47101` (Mac HTTP), `47102` (web interface of the PS5 payload). Details in [`protocol/SPEC.en.md`](protocol/SPEC.en.md).

## The parts

| Part | Folder | What it is | How to install / use |
|---|---|---|---|
| **Mac app** | [`mac/`](mac/README.md) | Swift/SwiftUI app (window + menu bar) that watches the SSD, extracts, identifies, converts and serves the catalog | [`mac/README.md`](mac/README.md) |
| **PS5 payload** | [`ps5/`](ps5/README.md) | ELF in C with a web interface; copies to `homebrew/` or installs `.pkg` | [`ps5/README.md`](ps5/README.md) |
| **Switch app** | [`switch/`](switch/README.md) | `ponte.nro`, built on the Sphaira engine; installs straight from the network | [`switch/README.md`](switch/README.md) |
| Protocol | [`protocol/`](protocol/SPEC.en.md) | v1 specification, the source of truth (official version in [Portuguese](protocol/SPEC.md); this is a translation) | — |
| Reference | [`reference/`](reference/README.md) | Python server and identifier, the executable reference of the spec | [`reference/README.md`](reference/README.md) |
| Tools | [`tools/`](tools/README.md) | Fake client (pretends to be a console), protocol conformance test and documentation check | [`tools/README.md`](tools/README.md) |
| Profiles | `profiles/` | Compatibility per firmware (e.g. `ps5-13.60.json`) | — |
| Docs | [`docs/`](docs/README.md) | Documentation index, visual guidelines and release notes | — |

## Download (no building needed)

Versions: [Releases](https://github.com/souzaq001/pontepn-/releases). Each release has `Ponte-Mac.zip`, `ponte.elf`, `ponte.nro`, the hashes and the licenses.

CI also builds everything on every push. In the repository's **Actions** tab, open the latest run on `main` of the workflow below and download it from **Artifacts**:

| Artifact | Workflow | Contents |
|---|---|---|
| `Ponte-Mac` | **Mac e PS5** | `Ponte-Mac.zip` with `Ponte.app` (unsigned: the first time, **System Settings → Privacy & Security → Open Anyway**) |
| `Ponte-PS5` | **Mac e PS5** | `ponte.elf`, the PS5 payload |
| `Ponte-Switch` | **Switch** | `ponte.nro`, the Switch app (goes in `/switch/ponte/ponte.nro`) |

To release a new version, write the notes in `docs/releases/vX.Y.Z.md` (English and Portuguese in the same file) and push to `main`: the **Release** workflow builds the three apps, creates the tag and publishes the files.

## Building and testing

Everything from the repository root:

```sh
# Python reference (stdlib + pytest only, Python 3.10+)
python3 -m pytest reference/tests -q

# Mac app (macOS 13+; PonteCore and PonteServer also build on Linux)
(cd mac && swift build && swift test)
(cd mac && ./scripts/build-app.sh)                     # creates mac/build/Ponte.app

# Protocol, black-box, against both servers
python3 tools/conformance.py --server python
python3 tools/conformance.py --server swift            # needs swift build first

# PS5 payload (test build for the computer)
make -C ps5 host && python3 ps5/tests/test_payload.py

# Switch client (protocol core, on the PC)
make -C switch/hosttest test

# Documentation: English/Portuguese pairs, links and anchors
python3 tools/check_docs.py
```

Building for real consoles: [`ps5/README.md`](ps5/README.md#building) (PS5 payload SDK) and [`switch/README.md`](switch/README.md#building) (devkitPro).

## Status

- [x] Protocol v1 (draft 4)
- [x] Format identifier (every format in spec §1)
- [x] Reference server + fake client + conformance test, tested end to end
- [x] Mac server in Swift: core, server and `ponte-server`, passing the conformance test
- [x] Mac app (SwiftUI): extraction, conversion → `.exfat`, pairing, copies in progress, history, SSD eject
- [ ] Mac app tested on a real Mac (conversion with `hdiutil`, local network permission, eject)
- [x] PS5 payload: web interface, resumable copies to `homebrew/`, `.pkg` through the system installer, queue that resumes on its own, speed diagnostics; tested against both servers (test build)
- [x] PS5 payload tested on a real console (folder and `.exfat` copied to `homebrew/`, game launched through ShadowMountPlus)
- [x] Ponte Switch app: written and tested on the PC against the reference
- [x] Switch app tested on a real console (screen, reading what is installed via ns/ncm, installing through yati)
- [ ] DLC for PS5 games as a folder or image ([spec §9](protocol/SPEC.en.md#9-open-items))
- [x] Documentation in English and Portuguese

Open items in the specification: [spec §9](protocol/SPEC.en.md#9-open-items).

## Contributing

- All work happens on `main`. For something larger, open a short branch and merge it back as soon as CI passes.
- Changed the protocol? First update `protocol/SPEC.md` (official, in Portuguese) and its history (§10), then the translation `protocol/SPEC.en.md`, the reference, the tests, the Swift server and the clients.
- Code in English; code comments and UI texts in Portuguese.
- Documentation in both languages: `NAME.md` in English and `NAME.pt-BR.md` in Portuguese. Change one, change the other; `tools/check_docs.py` checks it.
- Full rules (also used by AI assistants, in Portuguese): [`CLAUDE.md`](CLAUDE.md).

## Licenses

- Ponte is released under the **0BSD** license ([`LICENSE`](LICENSE)): use, copy, modify and redistribute it as you like, no conditions.
- Exception: `switch/app/` is based on [Sphaira](https://github.com/ITotalJustice/sphaira) and is therefore **GPLv3** ([`switch/app/LICENSE`](switch/app/LICENSE)), as is `ponte.nro`.
- `ps5/src/vendor/cJSON.*` is [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Legal notice and trademarks: [`LEGAL.md`](LEGAL.md).
