# PS5 payload

**English** · [Português](README.pt-BR.md)

C payload (ELF) for the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). It talks to the Mac using the [Ponte protocol](../protocol/SPEC.en.md) and serves a **web interface** where you choose what to copy:

```
http://<ip-do-ps5>:47102
```

On the PS5, the easiest way is the **"Ponte" icon on the home screen**: on startup, the payload creates a minimal folder in `/data/ponte/tile/PNTE00001/` and registers it with the system (copies `sce_sys` to `/user/app` and `/user/appmeta`, links the folder into `/system_ex/app` and calls the installer, as other 13.60 payloads do). Opening the icon opens the PS5 browser straight on the interface. You can also open it on your phone or computer, at the address the notification shows when the payload starts.

The icon only works while the payload is running (it is what serves the page).

## What it does

- **Finds the Mac on its own** (UDP discovery on port 47100). With more than one Mac, or if the network blocks discovery, you pick one or type the IP in the interface.
- **Pairing** with the 6-digit code from the Mac app, only once (the token is kept in `/data/ponte/config.json`).
- **Catalog** from the Mac with what is already on the console: `novo` (new), `no console` (on the console), `atualização disponível` (update available), `incompatível` (incompatible, per firmware profile). Grouped by game (base, update, DLC), with icons.
- **Copy** to the chosen storage (internal, M.2/extended or USB), one at a time:
  - folders and images (`.exfat`, `.ffpkg`, `.ffpfs`, `.ffpfsc`) first go to `<armazenamento>/ponte/tmp/`, which ShadowMountPlus does not watch, and only at the end are moved to `<armazenamento>/homebrew/`, from where it mounts and registers them;
  - connection drop or cancellation: trying again continues where it left off (`Range`), as long as the item has not changed on the Mac;
  - each file is checked by size and `sha256` before going to `homebrew/`;
  - destination already taken: the interface asks whether to replace it;
  - `.pkg`: handed to the system installer via the Mac's URL, which downloads it directly (without taking double the space). Installation continues in the background on the console.
- **Pause and automatic resume:** if the Mac goes offline (turned off, went to sleep, the network dropped) or the destination USB is not mounted yet, the copy is **paused** and continues on its own (every 15 s). The queue is saved in `/data/ponte/queue.json`: if you turn the console off midway, the next time the payload starts it resumes whatever was pending, without you opening the page. **Pausar** (pause) and **Continuar** (continue) on the page; what you paused stays paused until you tell it to continue. **Cancelar** (cancel) removes it from the queue (what already arrived is kept: copying again continues where it left off).
- **Progress of each stage**, in Ponte's visual style ([docs/DESIGN.md](../docs/DESIGN.md)): title and percentage, bar, `1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min` and a line with the stage and the file (`copiando · arquivo 3 de 120: data/big.bin`). The stages are: querying the Mac, checking what was already there (on a resume), copying, verifying, moving to `homebrew/` (including deleting the old copy when replacing) and handing to the installer. Stages without a size show an indeterminate bar. Verification also shows speed and responds to Pause/Cancel. At the top, a summary of the queue: items, how much is left, combined speed and time.
- **Speed**: average of the last ~5 s; stalled for more than 2 s shows 0. A discreet on-screen notice every 25% (with the speed), which can be turned off on the page.
- **One connection for the whole item (keep-alive):** all files of an item come over the same connection to the Mac, and the network thread stays alive for the whole copy. In folders with many small files this avoids opening one TCP connection per file. If the connection drops between one file and the next, the payload reconnects on its own.
- **Network and disk in parallel:** one thread reads from the Mac and fills whole buffers (4 × 4 MB by default); another checks the `sha256` and writes in large blocks. The `sha256` uses the PS5 processor's SHA instructions (SHA-NI) when available, about 5× faster than the C code (~1.2 GB/s versus ~220 MB/s on a test machine), so it no longer holds back the copy. Each file gets an `fsync` before going to `homebrew/`.
- **Measurements:** each copy shows network, disk and hash speed (and whether it used SHA-NI), time spent waiting on each, effective TCP buffer, what the bottleneck was and how many connections and requests it used. The same text goes to the log.
- **File that arrives different from the Mac's** (`sha256` does not match): it is deleted and downloaded again from scratch, once, on its own. If it fails again, the copy stops and says to check the network and the disk.
- **Mac that disappears midway:** 20 s without data and the copy pauses and retries on its own.
- **Report** of each copy to the Mac (it shows up in "Últimas cópias" (latest copies) in the app).

## Speed diagnostics

On the page, the **Diagnóstico de velocidade** (speed diagnostics) section measures each part separately (up to 15 s per test), with a bar and live speed while it runs. The result appears in a table:

- **Testar rede** (test network): downloads the largest item in the Mac's catalog and discards it (no hash, no disk), with the TCP buffer from the settings, with the system default and with 64 KB reads.
- **Testar disco** (test disk): writes to the storage chosen in "Copiar para" (copy to) (in `ponte/tmp/`, deleted at the end) with 16 KB, 256 KB and 4 MB blocks.

The same tests by URL, to use from another device:

```
http://<ip-do-ps5>:47102/api/diag/net?item=<id>&mb=1024&read_kb=4096&rcvbuf_kb=4096
http://<ip-do-ps5>:47102/api/diag/disk?storage=usb0&mb=1024&block_kb=4096
```

Progress of the test in progress (what the page uses for the bar): `GET /api/diag/progress` → `{running, kind, label, done, total, speed, elapsed_s}`.

Settings for testing without rebuilding (in the same section, kept in `/data/ponte/settings.json`): **Buffer TCP** (TCP buffer, `rcvbuf_kb`, 0 = system default; if the system refuses the value, the payload tries half, down to 256 KB) and **Bloco da cópia** (copy block, `chunk_kb`).

Mounter: after moving a game to `homebrew/`, the payload asks ShadowMountPlus for a fresh scan (`POST /api/v1/scan` with `{"reset_attempts":true}`, on its local API, which is on by default in ShadowMountPlus 1.7). It already scans on its own every ~15 s; the request only brings that forward and clears earlier failed attempts (useful when replacing a game). The address is in `mounter_rescan` (default `127.0.0.1:10101/api/v1/scan`; empty = don't ask), editable under **Cópias → Montador (ShadowMountPlus)**. If the mounter doesn't answer, the copy still stands and the reason goes to `/data/ponte/ponte.log`.

## Interface

Made for the TV (1920×1080, PS5 controller) and for the phone: bigger text and buttons on the TV (buttons at least 48 px tall), library and copies side by side on wide screens, a single column on the phone. The page updates the list of copies without rebuilding it, so the controller focus does not jump away from Pause/Cancel. It asks before Cancelar, **Substituir** (replace) and "Outro Mac" (another Mac, which deletes the pairing), and stops polling the payload while it is in the background. Errors say what happened and what to do.

### Copies in `/api/status`

Each item in `jobs` has the earlier fields (`seq`, `item_id`, `title`, `kind`, `content`, `storage`, `state`, `done`, `total`, `speed`, `eta_s`, `message`, `manual_pause`, `stats`, `error`) plus:

| Field | What |
|---|---|
| `phase` | stage in progress: `query`, `verify_partial`, `download`, `verify`, `move`, `install`; `null` when nothing is in progress |
| `phase_done`, `phase_total` | bytes of the stage (during download, the same as `done`/`total`); `null` when the stage has no size |
| `phase_speed` | stage speed in B/s (hash during verification, network during download) |
| `file`, `file_index`, `file_count` | current file and its position in the item (1 to n) |
| `started_ms` | when the copy started (epoch, ms) |
| `connections`, `requests` | TCP connections and requests to the Mac in this copy |

`speed` and `eta_s` use the average of the last ~5 s (0/`null` if nothing arrived in the last 2 s). Durations, speed and waits use the monotonic clock, which does not change if the console clock is adjusted.

## Installing on the PS5

1. Download `ponte.elf` from the latest CI run (GitHub **Actions** tab → **Mac e PS5** workflow → most recent run on `main` → **Artifacts** → `Ponte-PS5`), or build it (below).
2. To test once: send it to the etaHEN ELF loader (port 9021):
   ```sh
   socat -t 99999999 - TCP:<ip-do-ps5>:9021 < ponte.elf
   ```
3. To keep it permanently: put `ponte.elf` in autoload, **after etaHEN**, the same way as the other payloads in your chain (kstuff → ShadowMountPlus → etaHEN → Ponte). It runs in the background until the console turns off; if it is already running, a second copy warns and exits.

The payload needs nothing beyond what the homebrew environment already loads. The configuration lives in `/data/ponte/`.

## Building

Needs the SDK installed (`PS5_PAYLOAD_SDK`) and `clang`/`lld` 15 or newer:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make                     # build/ponte.elf
make send PS5_HOST=192.168.0.20
```

## Testing without a console

`make host` builds the same code for Linux/macOS, swapping only the console layer (`src/platform_host.c`): the "console" becomes a folder, and the fake `.pkg` installer downloads the URL as the PS5's would. The test starts the Mac server and the payload, and does everything through the web interface: finding the Mac, pairing, catalog, folder and image copy, drop and resume, destination taken, item that changed midway, `.pkg` and restart.

`make host` also builds `build/sha256-selftest`, which checks the SHA-NI `sha256` against the C code (known vectors and hundreds of random sizes, whole and in pieces) and benchmarks both. The end-to-end test runs it first.

```sh
make host
make selftest                                   # sha256 only (checks and benchmarks)
python3 tests/test_payload.py                   # against the Python reference
python3 tests/test_payload.py --server swift    # against the Swift server (build mac/ first)
```

## Layout

| File | What |
|---|---|
| `src/main.c` | startup, web interface (`/api/...` routes), Mac watcher |
| `src/ponte.c` | protocol client: discovery, pairing, JSON calls, configuration |
| `src/jobs.c` | copy queue, resume, verification, moving to `homebrew/`, installing `.pkg`, report; network and disk in separate threads, with measurements |
| `src/diag.c` | speed diagnostics: network and disk separately |
| `src/http.c` | minimal HTTP/1.1 (client with persistent connection, and server) |
| `src/platform_ps5.c` | notifications, storage devices, installed games, firmware, `sceAppInstUtil` |
| `src/platform_host.c` | the same, for tests on a computer |
| `src/util.c` | files, clock, speed, `sha256` (C and SHA-NI) |
| `ui/index.html` | the interface (embedded in the payload at build time) |
| `tests/sha256_selftest.c` | checks and benchmarks `sha256` |
| `src/vendor/cJSON.*` | [cJSON](https://github.com/DaveGamble/cJSON) (MIT) |

## Known limitations

- What is installed comes from `/user/app`, `/user/appmeta`, `/user/patch` and `/user/addcont`, plus whatever is in `homebrew/`. DLC shows up without a version (it counts as installed if the title has any DLC).
- Images in `homebrew/` have no version on the console; a new image of the same game shows up as "no console" (on the console).
- The SHA-NI path was checked on a PC with SHA-NI, not on a PS5. It is only used if `CPUID` says the processor has the instructions; otherwise the C code is used.
- Not yet tested on a real PS5: the system calls (notification, `sceAppInstUtilInstallByPackage`) follow what other 13.60 payloads use.
