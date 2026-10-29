# Review of the three apps: speed, progress, visuals and good practices

**English** · [Português](REVISAO.pt-BR.md)

Survey done in October 2026, covering the Mac app, the PS5 payload and the Switch app. Each item says **what was found**, **what changed** and, when still open, **what is left**. The visual standard that came out of this review is in [`DESIGN.md`](DESIGN.md).

Legend: ✅ done · 🔶 open (proposed)

## 1. Transfer speed

A copy goes through four stages, and the slowest one sets the speed: **Mac disk → Mac sending → network → receiving, verification and writing on the console**. During a copy the Mac gives it priority over everything else it does.

### Mac (server)

| Finding | Impact | Status |
|---|---|---|
| Sending read 1 MiB from the SSD, waited, sent it over the network, waited. Disk and network never worked together, and each chunk created a new buffer. | high | ✅ `sendfile`: the kernel reads from disk and writes to the socket without going through the app, with read-ahead (`F_RDAHEAD`/`F_RDADVISE`). Outside macOS, `pread` into a fixed 4 MiB buffer. |
| Socket send buffer at the system default. | medium | ✅ 4 MiB `SO_SNDBUF`. |
| The Mac could go to sleep in the middle of a copy (`userInitiatedAllowingIdleSystemSleep`), dropping the console. | high (long copies) | ✅ While there is an open send, the app holds off idle sleep and keeps interactive priority. Closing the lid still puts it to sleep, as it should. |
| Extraction (7-Zip) and conversion to `.exfat` competed with sending for the SSD. | high (during preparation) | ✅ Preparation yields: the extractor is **suspended** while a console is copying and resumed afterwards; conversion pauses between blocks. All preparation runs at low priority (QoS `utility`). |
| Sending threads at `userInitiated`. | low | ✅ `userInteractive`: the highest priority for CPU and disk. |
| Hash computation read in 1 MiB blocks. | low | ✅ 4 MiB (and it already waited for copies to finish). |

### PS5 (payload)

| Finding | Impact | Status |
|---|---|---|
| A new TCP connection **for each file** (`Connection: close`), with a thread created per file. Game folders have thousands of files. | very high (folders) | ✅ Keep-alive: one connection and one network thread per whole copy (test: 6 files, 1 connection); folders already created are remembered. |
| SHA-256 in plain C (~210 MB/s), in series with writing: with a 200 MB/s disk, the combination drops to ~120 MB/s. The PS5 processor has its own SHA instruction. | high | ✅ The processor's SHA instruction when available: **~1.2 GB/s** (5.5×), checked against the C version by a dedicated test. |
| Resuming re-read the whole partial file to verify it, with no progress and without responding to cancel/pause. | high (resume) | ✅ With bar and speed, responding to pause and cancel, and 5.5× faster. 🔶 Store verification checkpoints so it doesn't even need to re-read. |
| A fixed `SO_RCVBUF` disables FreeBSD's auto-tuning; the effective value was overwritten by any connection (queries, icons). | low to medium | ✅ Per-connection value; queries and icons without the large buffer. 🔶 Compare on the console `rcvbuf_kb=0` (automatic) with the default and keep the better one. |
| A Mac going offline was only noticed after 60 s. | medium | ✅ 20 s. |

### Switch (app)

| Finding | Impact | Status |
|---|---|---|
| Between the download (curl) and the installer there is a **64 KB** buffer. When full, it **pauses** curl, which only resumes at the next progress callback; in the meantime the TCP window closes and the Mac stops sending. | very high | ✅ 8 MB only for the Ponte download; writing waits for space instead of pausing; 512 KB socket reads; no deleting the start of the buffer on every read. |
| TCP receive buffer starting at 64 KB. | high | ✅ 1 MB (maximum stays 4 MB). |
| When opened from the Album, the system limits the app's networking (256 KB maximum). | high | ✅ Notice at the top of the Ponte screen saying how to open it. |
| Installing reads the `.cnmt.nca` (at the end of the NSP) first: each jump reopens the connection. | medium | 🔶 Reading in order would disable "skip what is already installed"; left as it is. |
| NSZ decompression on a single thread. | low to medium | 🔶 Only matters with networking over a USB cable. |

## 2. Progress and speed everywhere

| Operation | Before | Now |
|---|---|---|
| Mac: copy to a console | bar, speed, time remaining | ✅ same, in the shared component |
| Mac: archive extraction | just a spinner, no numbers | ✅ bar (7-Zip percentage), bytes extracted, speed, time remaining, "pausado" (paused) when it yields |
| Mac: folder → `.exfat` conversion | bar | ✅ bar + bytes + speed + time remaining |
| Mac: verification (sha256) | invisible | ✅ bar, speed, files in queue |
| Mac: menu bar | copies only | ✅ copies and preparation |
| PS5: queue | bar only while copying | ✅ queue summary (items, how much is left, speed, time) and a bar in every state |
| PS5: verification on resume and at the end | invisible (stalled bar) | ✅ its own phase with bar and speed |
| PS5: moving to `homebrew/`, handing off `.pkg` | text only | ✅ phase with indeterminate bar |
| PS5: speed diagnostics | text only | ✅ bar, live MB/s and results table |
| Switch: installation | bar, raw speed (jumping every second), "0 seconds remaining" with zero speed | ✅ bytes done and total, speed as a moving average, time remaining only when there is a speed |
| Switch: connecting, querying the Mac | text only | ✅ spinning indicator |
| Switch: whole queue | — | 🔶 bar per item only |

## 3. Visuals

- ✅ One palette for the three apps (the PS5's, which was the most complete: teal `#3FB6C4`), with a light version for the Mac. Table in [`DESIGN.md`](DESIGN.md).
- ✅ Same progress anatomy in all three: title and percentage, bar, `done of total · speed · left`, detail.
- ✅ Same size (base 10, decimal comma), speed and time texts.

## 4. Names

- ✅ Workflows: **Mac e PS5** (`mac-ps5.yml`) and **Switch** (`switch.yml`), instead of "CI" and "switch-app".
- ✅ Artifacts: `Ponte-Mac` (`Ponte-Mac.zip`), `Ponte-PS5` (`ponte.elf`), `Ponte-Switch` (`ponte.nro`).

## 5. Good code practices

### Switch
- ✅ `toupper` with `unsigned char` (undefined behavior with accented characters).
- 🔶 Queries to the Mac have no timeout (only the report has 5 s): a Mac that disappears without responding leaves the screen waiting until B.

### PS5
- ✅ `queue.json` written under its own lock (before, two threads could corrupt the file).
- ✅ `fsync` before renaming: a power loss does not leave an empty file with the final name.
- ✅ Report and notification no longer read the job after it is freed (it could read garbage).
- ✅ Size and paths coming from the Mac are validated; ids validated against header injection.
- ✅ A wrong hash downloads the file again once before failing (the message already promised this).
- ✅ Times from the monotonic clock (the PS5 clock can jump at boot).
- ✅ 4 new tests (keep-alive, phases, redone hash, invalid ids) + SHA-256 test.
- 🔶 The payload's web server has no limit on simultaneous connections.

### Mac
- ✅ New tests for the speed meter, hash progress and pausing preparation during copies.
- ✅ Output of external programs read on a separate thread (the pipe never fills up) and only the tail kept.
- 🔶 The app updates the whole snapshot every 1 s via `Timer`; it could publish only what changed (less redrawing with the app open).
- 🔶 `PonteServer` and `Library` use `@unchecked Sendable` with manual locks; migrate to `actor` when the project moves to Swift 6.
- 🔶 No signing or notarization: macOS blocks it on first launch. With an Apple developer account, sign in CI.

## 6. UX

### PS5
- ✅ Controller focus is no longer lost every 1.5 s (the list is not rebuilt).
- ✅ Larger letters and 48 px buttons on the TV; two columns on wide screens; works on a phone.
- ✅ Confirmation before canceling, replacing or switching Macs; Copiar (Copy) button locked while sending.
- ✅ Errors say what happened and what to do; bars accessible to screen readers.
- ✅ IP field with a keyboard that has a period on phones; polling pauses while the page is hidden.

### Switch
- ✅ Theme with the brand colors as default (anyone who had Sphaira's default theme saved is switched to it).
- ✅ Notice when opened from the Album (slow networking) saying how to open it properly.
- 🔶 Installation error messages still come out as codes (`yati 0x…`).
- 🔶 Unselecting a game does not unselect the update and DLC that were selected along with it.

### Mac
- ✅ Settings also in **Ponte → Ajustes… (⌘,)** (Ponte → Settings…), as in any Mac app.
- ✅ State never shown by color alone: a paused bar comes with the reason written out.
- 🔶 System notification when a copy finishes or fails with the app in the background.
- 🔶 Show the Mac's network speed (Wi-Fi or cable) next to the address: Wi-Fi is the most common cause of slow copies.
