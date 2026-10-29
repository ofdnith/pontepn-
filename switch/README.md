# Ponte on the Switch

**English** · [Português](README.pt-BR.md)

**Ponte** is a dedicated app for the Switch (`ponte.nro`). It opens straight into the list of Macs, pairs with the 6-digit code, shows the library next to what is already on the console, and installs games, updates and DLC directly over the network (`.nsp`, `.nsz`, `.xci`, `.xcz`).

Switch-specific rules: [SPEC §8.1](../protocol/SPEC.en.md#81-switch-client-ponte-app).

## Installing on the console

1. Download `ponte.nro`: this repository's **Actions** tab → **Switch** workflow → most recent run on `main` → **Ponte-Switch** artifact. Unzip it.
2. Copy it to the SD card at `/switch/ponte/ponte.nro`.
3. On the Switch with Atmosphère, open the homebrew menu **by holding R while launching a game** (from the Album, homebrew gets little memory) and choose **Ponte**.

If the screen just flashes and goes back, the app never opened: check that you copied the `.nro` (not the zip) and that Atmosphère is up to date.

## What it looks like

```
Ponte                                             João's MacBook · 192.168.0.12:47101
──────────────────────────────────────────────────────────────────────────────────────
 [■] [icon] Cool Game                                                             New
            Game • NSP • 14.2 GB
   [■] Cool Game                                                                  New
       Update v131072 • NSP • 1.1 GB
   [ ] Cool Game                                                            Installed
       DLC • NSZ • 210 MB
 [ ] [icon] Another Game                                      Newer than installed
──────────────────────────────────────────────────────────────────────────────────────
 3 items • 2 selected (15.3 GB) • microSD: 212 GB free
 A Select   X Install 2   Y Update   − Clear   B Back   ZR Options   + Exit
```

- **Mac list**: the saved ones and the ones found on the network. A connects, X searches again, Y types an address, − forgets a saved Mac, B or + exits.
- **Library**: grouped by game (base, updates, DLC). A selects/unselects (selecting a new game also selects the newest update and the missing DLC), − selects everything new or clears, X installs, Y updates. Incompatible items show the reason in the footer.
- **Install**: asks microSD or console memory, checks space (+5%) and installs in sequence; each item sends a `report` to the Mac. On the first install the app asks you to enable game installation (only once).
- **During installation**: Y turns the screen off (handheld mode only; any button turns it back on) and the console does not go to sleep on its own. The HOME button goes to the menu and the copy **keeps going in the background**; when you come back, the box shows where it is. Launching another game closes Ponte (the Switch only runs one at a time) and cancels the installation. This applies when opened by holding R on a game; from the Album the HOME menu pauses the copy, which continues when you return. If the console sleeps (power button), the copy tries to resume with `Range` for up to 60 s after the network comes back.
- **ZR Options**: language, theme and engine install options.
- The list updates itself when the Mac gets new games (checked every 20 s).

## Speed

- The download enters the installer through an 8 MB buffer (Sphaira uses 64 KB, which pauses the transfer constantly) and the TCP receive buffer starts at 1 MB.
- **Open it from the homebrew menu by holding R on a game.** Opened from the Album, the system gives the app much less network memory and the copy is much slower; Ponte warns at the top of the screen when this happens.
- The install box shows how much has arrived, the speed and the time remaining.

## Code

```
switch/app/        the app: Sphaira's install engine and interface + the Ponte screen
switch/hosttest/   the protocol core compiled for the PC and tested against the reference
```

Ponte is built on [Sphaira](https://github.com/ITotalJustice/sphaira) (GPLv3), which already has a mature NSP/NSZ/XCI/XCZ installer. That is why the app is also **GPLv3** (`switch/app/LICENSE`). What Ponte adds and changes is in [`app/PONTE.md`](app/PONTE.md).

## Building

CI (`.github/workflows/switch.yml`, **Switch** workflow) builds on every push to `switch/`, `protocol/` or `reference/` and produces the **Ponte-Switch** artifact: a slimmed-down engine, without FTP/MTP, USB HDD and music.

Locally, with devkitPro (`dkp-pacman -S switch-dev` and the libs listed in Sphaira's README):

```sh
cd switch/app
cmake --preset Ponte
cmake --build --preset Ponte
# build/Ponte/ponte.nro
```

## Testing without the console

`hosttest/` compiles the app's same `ponte_proto.cpp` for the PC and runs the console flow against the reference server: discovery, wrong code (`401`), pairing, `console/state`, `catalog/changes`, stale revision (`412`), download with a drop in the middle and resume via `Range`, `report`, and a different protocol version (`426`).

```sh
make -C switch/hosttest test
```

What these tests do **not** cover, and must be checked on the console: the screen itself, reading what is installed (ns/ncm), broadcast discovery on a real network, and installation through yati.
