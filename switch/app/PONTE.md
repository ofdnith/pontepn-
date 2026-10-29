# What this directory is

**English** · [Português](PONTE.pt-BR.md)

Code for the **Ponte** app for the Switch. It is [Sphaira](https://github.com/ITotalJustice/sphaira) (GPLv3, by ITotalJustice and contributors) at commit `338348e74b6d278bc570a9be5373d25197bf6c8d`, with the Ponte screen and the changes below. Sphaira serves as the engine: installer (yati), interface, networking and translation. The license remains GPLv3 (`LICENSE`).

## Added

| File | What |
|---|---|
| `sphaira/include/utils/ponte_proto.hpp`, `sphaira/source/utils/ponte_proto.cpp` | Ponte v1 protocol in plain C++ (JSON with yyjson, grouping, install order). No libnx: it compiles and is tested on the PC by `../hosttest`. |
| `sphaira/include/utils/ponte.hpp`, `sphaira/source/utils/ponte.cpp` | saved Macs (`/config/sphaira/ponte.ini`), UDP discovery, HTTP calls, console inventory (ns/ncm). |
| `sphaira/include/ui/menus/ponte.hpp`, `sphaira/source/ui/menus/ponte.cpp` | the screen. |
| `assets/ponte/icon.jpg` | app icon. |

## Changed from Sphaira

- `sphaira/source/main.cpp`: opens straight into the Ponte screen, without the Sphaira menu.
- `sphaira/CMakeLists.txt`: name **Ponte**, its own version (`PONTE_VERSION`), icon, output `ponte.nro`, defines `PONTE_STANDALONE` (on the screen: + exits, ZR opens the options). The Sphaira version (`sphaira_VERSION`) stays as it is: it is the engine's version.
- `CMakePresets.json`: **Ponte** preset, the same as Sphaira's "Lite" but with NSZ.
- `sphaira/source/yati/source/http.cpp` (+ `.hpp`): constructor with extra headers (Bearer, `X-Ponte-*`); 512 KB socket reads and an 8 MB buffer (below).
- `sphaira/include/utils/devoptab_common.hpp`, `sphaira/source/utils/devoptab_common.cpp`: `PushPullThreadData::UseLargeBlockingBuffer()`. In Sphaira the buffer between curl and the installer is 64 KB and, when full, it **pauses** curl, which only resumes at the next progress callback; in the meantime the TCP window closes and the Mac stops sending. With this option (only for the Ponte download), the buffer is 8 MB and writing waits for space instead of pausing. Data already read is removed from the start of the buffer every so often, not on every read. Sphaira's network mounts work as before.
- `sphaira/source/main.cpp`: the TCP receive buffer starts at 1 MB (previously 64 KB) in application mode.
- `sphaira/source/ui/progress_box.cpp`: bytes done and total, speed as a moving average, and no "0 seconds remaining" while the speed is still zero.
- `assets/romfs/themes/ponte_theme.ini` + `sphaira/include/app.hpp`, `sphaira/source/app.cpp`: theme with the brand colors, Ponte's default; anyone who had Sphaira's default theme saved is switched to it.
- `sphaira/source/yati/yati.cpp`: notifies the screen when something is installed.
- `sphaira/include/app.hpp`: `App::SetRunInBackground()` (reference counted): while Ponte is installing, `appletSetFocusHandlingMode(NoSuspend)` and the HOME menu does not freeze the app; at the end it goes back to `SuspendHomeSleep`. Application mode only.
- `sphaira/source/app.cpp` (`App::Loop`): when out of focus it does not draw and sleeps 50 ms per iteration (without vsync the loop would spin nonstop).
- `sphaira/source/ui/progress_box.cpp` (+ `.hpp`): optional hint line above the buttons; speed computed from the real time between samples (out of focus the box is not drawn).
- `sphaira/source/ui/menus/main_menu.cpp`: registers the screen (only matters if the Sphaira menu is used).
- `assets/romfs/i18n/pt.json`: Portuguese texts.
- Sphaira's `.github/` removed; CI is this repository's `.github/workflows/switch.yml`.

## Configuration and the homebrew menu

Since 0.1.1 Ponte keeps its options in `/config/ponte/` (`config.ini`, `ponte.ini`), separate from Sphaira's. Themes, extra translations and the engine cache remain in `/config/sphaira/` and `/switch/sphaira/cache/`.

**Ponte never touches `/hbmenu.nro`.** Version 0.1.0 inherited Sphaira's configuration and, if the "Replace hbmenu on exit" option was enabled there, on exit it copied `ponte.nro` over the homebrew menu: the homebrew menu would then open Ponte. In 0.1.1:
- `App::GetReplaceHbmenuEnable()` always returns `false` and the option is gone from the settings;
- the block in `~App()` that copies the app to `/hbmenu.nro` is not compiled;
- if Ponte notices it is running as `/hbmenu.nro`, it offers to restore the homebrew menu (from `/switch/hbmenu.nro`, the backup Sphaira makes, or from `/switch/sphaira/sphaira.nro`).

## Not done yet (proposed)

- Read the file in order (`IsStream()`): it would avoid reconnecting when the installer jumps to the `.cnmt.nca` at the end of the NSP, but it disables "skip what is already installed" and other yati conversions. Left as it is.
- Progress for the whole queue (bytes of all items) and speed measured on the network, not on writing (with NSZ, more is written than arrives).

## Updating the engine

To bring in a new Sphaira version: apply the changes above on top of the new code (the diff between `338348e` and this directory) and run `make -C ../hosttest test` and the CI.
