# Mac server

**English** · [Português](README.pt-BR.md)

Menu bar app in Swift/SwiftUI that watches the library on the SSD, prepares the games and serves the catalog to the consoles (see [`../protocol/SPEC.en.md`](../protocol/SPEC.en.md)). It does the same as the [Python reference](../reference/), validated by the same [conformance test](../tools/conformance.py).

Requirements: macOS 13 or newer, with the Xcode command line tools (`xcode-select --install`). For split or password-protected archives: `brew install sevenzip`.

## Structure

| Target | What |
|---|---|
| `PonteCore` | format identifier, library (scan, stability, `id`/`revision`, duplicates, hashes), profiles, console state, `.exfat` plan. Foundation only. |
| `PonteServer` | HTTP/1.1 (POSIX sockets, keep-alive, `Range`), UDP discovery, pairing, the SPEC routes and preparation: extracting archives and converting folder → `.exfat`. |
| `ponte-server` | command-line server, with the same options as the Python reference. |
| `PonteMac` | the menu bar app. |

`PonteCore` and `PonteServer` also build on Linux, where the tests run in CI or on any machine without a Mac. Conversion to `.exfat` uses `mkfile`, `hdiutil`, `newfs_exfat` and `diskutil`, so it only really runs on macOS; the flow is tested with fake tools.

## Building and testing

In the `mac/` folder:

```sh
swift build                 # everything
swift test                  # core and server tests
./scripts/build-app.sh      # builds build/Ponte.app (drag it to /Applications)
```

Protocol conformance test, at the repository root (needs `swift build` first):

```sh
python3 tools/conformance.py --server swift
python3 tools/conformance.py --server python    # the reference, for comparison
```

## Using the app

Easiest way, without building: download the `Ponte-Mac` artifact from the latest run (GitHub **Actions** tab → **Mac e PS5** workflow → most recent run on `main` → **Artifacts** → `Ponte-Mac`); inside is `Ponte-Mac.zip`, unzip it and drag `Ponte.app` to **Applications**. Since the app is not signed by Apple, macOS blocks it the first time: open **System Settings → Privacy & Security** and click **Open Anyway** (or right-click the app → **Open**).

1. Open `Ponte.app` like any app: it opens a window and shows up in the Dock. An icon also stays in the menu bar.
2. In **Ajustes** (settings, the gear), choose the library folder on the SSD.
3. The pairing code appears in the window. Type it on the console the first time.

Closing the window does not stop the server (the consoles keep copying); to reopen it, click the Dock icon or **Abrir o Ponte** (open Ponte) in the menu bar. To stop for good: **Sair** (quit).

The first time a console looks for the Mac, macOS asks for **local network** permission for Ponte: accept it. If the macOS firewall is on, allow incoming connections too.

What the window shows:

- **Código de pareamento** (pairing code) and the address (`IP:47101`), to type on the console if automatic discovery does not work.
- **Biblioteca** (library): how many PS5 and Switch games are ready, how many are still arriving and what is being prepared (extracting or converting, with progress).
- **Precisa de atenção** (needs attention): items that could not be identified (you choose platform, type and Title ID, and the app writes the `.ponte.json`), password-protected archives, incomplete sets and failures, with "tentar de novo" (try again).
- **Copiando agora** (copying now): each copy in progress, with a progress bar, how much is done ("1,2 GB de 4,5 GB"), speed, time remaining, the console and the current file. On resume, what the console already had counts toward the progress. A stalled copy shows how long it has been stalled.
- **Consoles pareados** (paired consoles), with an option to revoke.
- **Histórico** (history): the latest copies with their result (installed, copied, cancelled, failed or interrupted), console, size, duration and average speed. **Ver tudo** (see all) (or **Histórico de cópias…** (copy history) in the menu bar) opens a window with the full table, search by game, console or result, and **Limpar histórico** (clear history). It is kept between app launches (last 500 copies) and stays visible with the SSD ejected.

A copy becomes "interrupted" when the console stops downloading for 2 minutes without saying how it ended (network drop, console turned off, SSD ejected). If the console resumes later, it shows up as a new copy. For a PS5 `.pkg`, the console says "installed" as soon as it hands the package to the system installer; the download the installer does next shows up in **Copiando agora** ("o console está baixando o pacote para instalar", the console is downloading the package to install) and completes the same history entry.

**Ejecting the SSD**: the **Ejetar** (eject) button next to the library path (and **Ejetar …** in the menu bar), when the library is on an external disk. Ejecting from the Finder (or Disk Utility) also works: Ponte receives the request from macOS before unmounting, lets go of the files (closes connections, stops preparation, scanning and hashing) and lets the disk go, without "disk in use". If another program prevents it, Ponte goes back to serving on its own after 20 s. The app's button has the advantage of asking first when a console is copying.

While the app is open, scanning and hashing read the SSD at low priority (they give way to you in the Finder), and the temporary image used for conversion to `.exfat` does not appear in the Finder.

- With nothing going on, it ejects right away.
- With a console copying (sending now or in the last 30 s), it asks: **Esperar terminar** (wait until done: ejects on its own when the copies finish; you can choose **Ejetar agora** (eject now) or **Não ejetar** (don't eject) while waiting), **Ejetar agora** (the copy stops and the console resumes where it left off when the SSD comes back) or **Cancelar** (cancel).
- An extraction or conversion in progress is interrupted and starts over from scratch when the SSD comes back; the original folder or archive is still there.
- While the SSD is out, the server is off (to the console it is as if the Mac were off). When you connect the SSD again, it comes back on its own.
- If another program is using the disk, the app says which one and keeps serving.

Settings: network name, convert PS5 folders to `.exfat` (**off**: conversion takes time and uses space), extract archives (**off**: extract them yourself inside the library), keep originals in `_originais/` (on; off deletes them), verify copies with SHA-256 (**off**: copies start right away and the console checks only the size), require pairing (on) and open at login. Also in **Ponte → Ajustes… (⌘,)**.

Where things live: state (server id, paired consoles, already computed hashes, copy history) and firmware profiles in `~/Library/Application Support/Ponte/`. The profiles can be edited there ("Abrir pasta de perfis de firmware", open firmware profiles folder, in the settings); the ones that ship with the app are only copied when they do not exist.

## Command-line server

```sh
.build/debug/ponte-server --library /Volumes/SSD/Jogos
.build/debug/ponte-server --library /Volumes/SSD/Jogos --convert-folders --extract
```

`--help` lists the options. Without `--convert-folders`/`--extract` it behaves exactly like the Python reference (it only serves what is already ready).

## How preparation works

- **Scan** every 5 s (folders on an external SSD do not always produce reliable events; scanning is cheap once games become single files). `sha256` hashes in the background, kept between runs.
- **PS5 folder** stable → stays out of the catalog (the console gets `409` if it asks for it) → becomes `<TITLE_ID>.exfat` with the sidecar file and the icon → the folder goes to `_originais/`.
- **Archive** complete and stable → extracted to a temporary `.ponte-tmp` folder → what came out of it enters the library → the parts go to `_originais/`.
- A failure becomes "needs attention" and is not retried on its own.
