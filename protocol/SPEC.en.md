# Ponte Protocol — v1 (draft 4)

**English** · [Português](SPEC.md)

> This is a translation of the official specification, [`SPEC.md`](SPEC.md), which is written in Portuguese. If the two ever differ, the Portuguese version prevails.

Specification of the communication between the **server** (the Mac app) and the **clients** (the PS5 payload, the Ponte app on the Switch, the test tool in `tools/`).

The Python reference implementation (`reference/`) is the executable answer key for this specification. When the two diverge, one of them has a bug: fix it and record it in §10.

## 0. What the system does (and does not do)

- The Mac keeps the library on the SSD, **identifies** each game and publishes a catalog.
- On the console, the Ponte client shows side by side **what exists on the Mac** and **what is already on the console**. You choose what to copy and to which storage.
- The client **copies** the game over the network to the console's storage and leaves it ready to play: it installs packages, or places images and folders where the mounter finds them.
- **Nothing runs over the network during play.** After the copy, the Mac can even be turned off.

Principle: **the console pulls, the Mac serves.** The Mac never initiates a connection to the console.

---

## 1. Supported formats

### 1.1 PS5 (includes PS4 games that run on the PS5)

| `kind` | Extension / form | How the Mac identifies it | What the client does on the console |
|---|---|---|---|
| `folder` | folder with `sce_sys/param.json` at the root (e.g. `PPSA12345/`) | reads `sce_sys/param.json` | copies to the temporary area and moves to `<destination>/homebrew/<TITLE_ID>/` → the mounter registers it |
| `exfat` | `.exfat` (exFAT image) | metadata written during conversion (§7.4); otherwise, the file name | copies to the temporary area and moves to `<destination>/homebrew/` → the mounter mounts and registers it |
| `ffpkg` | `.ffpkg` (UFS image) | file name (internal reading is open, §9) | same as `exfat` |
| `ffpfs` | `.ffpfs` (PFS image, experimental) | file name | same as `exfat` |
| `ffpfsc` | `.ffpfsc` (compressed PFS container, experimental) | file name | same as `exfat` |
| `pkg_ps4` | PS4 `.pkg` | package header (§1.4) | downloads to the temporary area → system installer → deletes |
| `pkg_ps5` | PS5 `.pkg` | package header (§1.4) | same as `pkg_ps4` |
| `folder_ps4` | folder with `sce_sys/param.sfo` | reads `param.sfo` | **to be confirmed** which loader to use; for now it only copies |

The image and folder formats depend on a **mounter** on the PS5 (currently ShadowMountPlus with kstuff), which looks for games in `homebrew/` on internal storage, on the M.2/extended storage and on USB drives.

For PS5 games in folder and image form, `content` is always `base`: updates usually come already merged in these formats.

### 1.1.1 Compatibility profiles

What works on each firmware lives in profile files on the Mac (`profiles/ps5-<firmware>.json`), not in the code. For a console, the profile that applies is the one with the **highest firmware less than or equal** to the console's. If there is none, the one with the lowest firmware applies, with the warning `profile_guessed`.

Profile format:
```json
{
  "platform": "ps5",
  "firmware": "13.60",
  "kinds": {
    "exfat":      {"status": "ok"},
    "ffpkg":      {"status": "ok"},
    "folder":     {"status": "ok", "note": "some titles only run from USB"},
    "ffpfs":      {"status": "experimental"},
    "ffpfsc":     {"status": "experimental"},
    "pkg_ps4":    {"status": "ok"},
    "pkg_ps5":    {"status": "blocked", "note": "PS5 .pkg package does not run on this firmware"},
    "folder_ps4": {"status": "unknown"}
  }
}
```
`status`: `ok` | `experimental` (copies, with a warning) | `unknown` (copies, with a warning) | `blocked` (the item appears as `incompatible`).

The project's reference profile: **13.60**, with homebrew enabled on every boot (kstuff → ShadowMountPlus → etaHEN). On it, the main path (`exfat`, `ffpkg`, `folder`) requires the payload **only to copy and move**; ShadowMountPlus does the rest.

### 1.2 Switch

| `kind` | Extension | How the Mac identifies it | What the client does |
|---|---|---|---|
| `nsp` | `.nsp` | PFS0 container: Title ID from the name of the internal `.tik`; otherwise, from the file name | installs through the Sphaira engine |
| `nsz` | `.nsz` | same as `nsp` | installs (decompression on the console) |
| `xci` | `.xci` | file name; with `prod.keys` on the Mac, full metadata (future) | installs |
| `xcz` | `.xcz` | same as `xci` | installs |

Title ID and content type (Title ID as a 64-bit integer, `t`):

| Condition | `content` | `parent_title_id` |
|---|---|---|
| `t & 0xFFF == 0x000` and bit 12 (`0x1000`) clear | `base` | — |
| `t & 0xFFF == 0x800` | `update` | `t & ~0xFFF` |
| bit 12 set and `t & 0xFFF != 0` | `dlc` | `(t & ~0xFFF) - 0x1000` |
| anything else | `unknown` | — |

File names in the community convention: `Name [0100ABCD12340000][v65536].nsp`. The Title ID is the first group of 16 hexadecimal digits in square brackets; the version is `[vN]`, with N an integer.

### 1.3 Archives (any platform)

`.zip`, `.rar`, `.7z`, `.tar` and split parts (`.part1.rar`/`.part01.rar`, `.7z.001`, `.zip.001`, `.z01`…). **They never go to the console.** The Mac groups the parts of the same set, waits for all of them to be stable, extracts into the library and identifies what came out. Password → the Mac asks for it in the interface. After extraction, the archive is deleted or moved to `_originais/` (configurable). Incomplete set (a part is missing) → "needs attention".

Extraction details:
- Extractor: 7-Zip (`7zz`/`7z`) when installed; without it, macOS's `bsdtar`, which only covers single-part archives without a password. Split parts or a password without 7-Zip → "needs attention", asking to install it.
- Extracts into `<archive folder>/<name>.ponte-tmp/`, which the scan ignores. At the end: if a single item came out (the common case, e.g. `PPSA01234/`), it goes straight into the archive's folder; otherwise, the temporary folder becomes `<name>/`. This keeps the game within the scan's 2-level limit (§7.2).
- Name already taken → appends ` (2)`, ` (3)`…

### 1.4 Identifying `.pkg`

All header fields are **big-endian**.

1. Magic in bytes 0–3: `7F 43 4E 54` (`\x7FCNT`). Any other magic → "needs attention".
2. Content ID: 36 ASCII bytes starting at `0x40` (e.g. `UP0000-CUSA00001_00-XXXXXXXXXXXXXXXX`). The Title ID is the 9 characters starting at the 8th (`CUSA00001`).
3. Platform by Title ID prefix: `PPSA` and `PPSB` → `pkg_ps5`; other 4-letter prefixes (`CUSA`, `PCJS`, `PLJM`…) → `pkg_ps4`. Configurable.
4. For `pkg_ps4`, metadata comes from the internal `param.sfo`: an entry table with `entry_count` (u32 at `0x10`) 32-byte entries starting at `table_offset` (u32 at `0x18`). Each entry: `id` (u32, +0), `offset` (u32, +16), `size` (u32, +20). `id 0x1000` = `param.sfo`; `id 0x1200` = `icon0.png`.
5. `CATEGORY` from `param.sfo`: `gd` → `base`; `gp` → `update`; `ac` → `dlc`; other → `unknown`. For `update` and `dlc`, `parent_title_id` = the package's own Title ID (same title).
6. Version: `APP_VER` (base) or `VERSION`; minimum firmware: `SYSTEM_VER`, a BCD integer (`0x05050000` → `5.05`).

### 1.5 Identifying folders

- `sce_sys/param.json` (PS5): `titleId`, `contentId`, `contentVersion`, `localizedParameters.<defaultLanguage>.titleName`, `requiredSystemSoftwareVersion` (BCD hexadecimal text, e.g. `0x0403000000000000` → `4.03`). Missing fields are `null` and do not prevent identification. It also requires `eboot.bin` at the root; without it → "needs attention".
- `sce_sys/param.sfo` (PS4): the same fields as §1.4, items 5–6.
- Icon: `sce_sys/icon0.png`, when present.

### 1.6 Metadata sidecar file

For each identified item, the Mac writes `<name>.ponte.json` next to it with the final metadata (the catalog fields of §5.3, except `id` and `revision`). It serves to:
- keep the metadata read from the folder when it becomes `.exfat`;
- store what you filled in by hand in "needs attention";
- give a name and version to `.ffpkg`/`.ffpfs` without having to read the image.

If the sidecar file exists, it takes priority over automatic identification. The icon goes in `<name>.ponte.png`.

---

## 2. Transport

| Item | Value |
|---|---|
| Protocol | HTTP/1.1 over TCP, no TLS (local network) |
| Default port | `47101` (configurable on the Mac) |
| Body | UTF-8 JSON on `/api` routes, binary (`application/octet-stream`) on file routes |
| Prefix | `/api/v1` |
| Connections | persistent (keep-alive) supported and recommended |

- Responses always carry `Content-Length` (no chunked encoding), to allow a minimal C client.
- Sizes and offsets are **64-bit** integers (games exceed 4 GB). C clients use `uint64_t`.
- `HEAD` works on every file route, with the same headers as `GET`.

## 3. Network discovery

1. The client sends a UDP broadcast to port `47100` with the ASCII text `PONTE?1`, from any port, and listens on that same socket for ~1 second.
2. Each server replies (unicast, to the source port) with JSON:
   ```json
   {"ponte": 1, "name": "João's MacBook", "port": 47101, "id": "b3f1c2d4e5f60718"}
   ```
3. The client uses the reply's source IP + `port`. If several servers reply, it shows the list; `id` identifies each Mac and is the key of the saved pairing.

Alternative: fixed IP and port in the client's configuration file.

## 4. Authentication

- The Mac shows a 6-digit code, which changes after each successful pairing.
- On first contact, the client asks for the code and calls `POST /api/v1/pair`, receiving a `client_token` (64 hexadecimal characters).
- Every subsequent request sends `Authorization: Bearer <client_token>`.
- Protection: after 5 wrong codes within 1 minute, the Mac refuses new attempts for 1 minute (`429 too_many_attempts`).
- The Mac lists the paired consoles and can revoke any of them.
- Pairing can be turned off on the Mac ("trust the local network"). In this mode, the token is not required and the client's platform comes from the request bodies.

## 5. Routes

Without a valid pairing, every route except `info` and `pair` responds `401`.

### 5.1 `GET /api/v1/info` (no authentication)

```json
{"protocol": 1, "server": "ponte-mac/0.1.0", "name": "João's MacBook", "id": "b3f1c2d4e5f60718", "pairing_required": true}
```
The client sends the header `X-Ponte-Protocol: 1` on every request. Different major version → `426 protocol_mismatch`, and the client warns and stops.

### 5.2 `POST /api/v1/pair`

```json
{"code": "482913", "client": {"platform": "ps5", "name": "Living room PS5", "version": "0.1.0", "firmware": "13.60"}}
```
Response `200`: `{"client_token": "c9a1…"}`. Wrong code → `401`. The platform given here is associated with the token.

### 5.3 `GET /api/v1/catalog?platform=ps5|switch`

Summary list of the **ready** items. Without `platform`, uses the token's platform.

```json
{
  "generated_at": "2026-10-28T18:10:00Z",
  "items": [
    {
      "id": "8f3e0a1c9b2d4e57",
      "revision": "a41c09e2",
      "platform": "ps5",
      "kind": "exfat",
      "content": "base",
      "title": "Game Name",
      "title_id": "PPSA01234",
      "content_id": "UP0000-PPSA01234_00-XXXXXXXXXXXXXXXX",
      "version": "01.000.000",
      "required_firmware": "4.03",
      "size": 48318382080,
      "file_count": 1,
      "parent_title_id": null,
      "warnings": [],
      "added_at": "2026-10-27T22:10:00Z",
      "icon": "/api/v1/items/8f3e0a1c9b2d4e57/icon"
    }
  ]
}
```

Fields:
- `id`: 16 hexadecimal characters = the start of the SHA-256 of `<platform>:<item's relative path in the library>`. Stable as long as the item is not moved or renamed.
- `revision`: 8 hexadecimal characters that change whenever the item's content changes (derived from each file's size and modification date). See §5.5.
- `kind`: one of the values in §1.
- `content`: `base` | `update` | `dlc` | `unknown`.
- `parent_title_id`: for `update` and `dlc`, the base game. The client groups by it.
- `required_firmware`: always in the `X.YY` format, when known.
- `warnings`: list of codes — `experimental_format`, `metadata_from_filename`, `duplicate_title_id`, `profile_guessed`, `unknown_compatibility`. Those that depend on a profile (`experimental_format`, `unknown_compatibility`, `profile_guessed`) only appear when the console's firmware is known: the one given at pairing or the last one sent in `console/state`.
- `title_id`, `content_id`, `version`, `required_firmware` and `icon` may be `null`. `title` is never empty: if there is no name, it uses the file name without extension.
- Order: by `title`, case-insensitive.

### 5.4 `GET /api/v1/items/{id}`

The fields of §5.3 plus the list of files:

```json
{
  "id": "8f3e0a1c9b2d4e57",
  "revision": "a41c09e2",
  "kind": "folder",
  "files": [
    {"index": 0, "path": "eboot.bin", "size": 31457280, "sha256": "ab12…"},
    {"index": 1, "path": "sce_sys/param.json", "size": 4096, "sha256": null}
  ]
}
```

- `path`: relative to the item's root, always with `/`, no `..`, no leading slash, no empty components. The client rejects any path outside these rules.
- File order: alphabetical by `path` (UTF-8 byte order), so that `index` is stable.
- Files that the Finder creates on its own (`.DS_Store` and `._*`) are never part of an item.
- Single-file items have a single file, with `path` equal to the file name.
- `sha256` may be `null` while the Mac computes it.
- Sidecar files (`.ponte.json`, `.ponte.png`) never appear here.

### 5.5 `GET /api/v1/items/{id}/files/{index}`

Binary content. Response headers:
- `Content-Length`, `Accept-Ranges: bytes`
- `X-Ponte-Revision: <revision>`
- `X-Ponte-Sha256: <hash>` when known

Resuming:
- The client sends `Range: bytes=START-` (or `START-END`) **together with** `X-Ponte-Revision: <the revision it had>`.
- If the item's revision has changed, the server responds `412 revision_changed` and the client restarts that item from scratch.
- Valid `Range` → `206` + `Content-Range: bytes START-END/TOTAL`. No `Range` → `200`.
- `Range` past the end → `416 bad_range`. `START` equal to the file size is also `416`: the client should have checked that the file was already complete.

For programs that download without sending headers (the PS5 package installer receives only a URL):
- the route accepts a name at the end, which is ignored: `/api/v1/items/{id}/files/{index}/{name}` (e.g. `.../files/0/game.pkg`), so the downloader sees the extension;
- on file routes only, the token can go in the query: `?token=<client_token>`, instead of `Authorization`.

### 5.6 `GET /api/v1/items/{id}/icon`

PNG or JPEG with the correct `Content-Type`. `404` if there is none.

### 5.7 `POST /api/v1/console/state`

The client sends what is **already on the console**. Sent when the app opens and after each copy.

```json
{
  "platform": "ps5",
  "firmware": "13.60",
  "storages": [
    {"id": "internal", "label": "Internal", "free": 412316860416, "total": 825000000000},
    {"id": "ext0", "label": "M.2", "free": 1700000000000, "total": 2000000000000},
    {"id": "usb0", "label": "USB", "free": 900000000000, "total": 1000000000000}
  ],
  "installed": [
    {"title_id": "PPSA01234", "version": "01.000.000", "content": "base", "storage": "ext0"},
    {"title_id": "CUSA00001", "version": "01.05", "content": "update", "storage": "internal"}
  ]
}
```
On the Switch, `storages` are `nand` and `sd`, and `installed` comes from the system's list of applications and versions (version as an integer, in text: `"65536"`).

Response: the catalog (§5.3) with two extra fields per item:
- `state`, decided in this order (the first that applies wins):
  1. `incompatible` — the profile marks the `kind` as `blocked`, or `required_firmware` is higher than the console's firmware.
  2. `installed` — the console has the same `title_id` and `content` with a version **greater than or equal**.
  3. `update_available` — the console has the same `title_id` and `content`, but with a **lower** version.
  4. `new` — otherwise (including when the item has no `title_id`).
- `state_reason`: short text to display, or `null` (e.g. "PS5 .pkg package does not run on this firmware").

Version comparison:
- PS4/PS5: split on `.`, compare each part as a number (`01.000.000` = `1.0.0`; `01.05` > `01.4`). Missing parts count as 0.
- Switch: compare as an integer.
- Firmware `X.YY`: compare as numeric (X, YY).
- `null` version on either side → treated as equal (`installed`), so as not to suggest a copy for no reason.

### 5.8 `POST /api/v1/items/{id}/report`

Final result of each copy:
```json
{"result": "installed", "storage": "ext0", "detail": null, "duration_s": 512}
```
`result`: `installed` | `copied` | `failed` | `cancelled`. `copied` = "file in place, waiting for the mounter to register it". Response `204`.

### 5.9 `GET /api/v1/catalog/changes?since=<generated_at>`

```json
{"changed": true, "new_items": 2, "generated_at": "2026-10-28T18:20:00Z"}
```
`new_items` counts items with `added_at` later than `since`. Invalid `since` → `400 bad_request`.

## 6. Errors

```json
{"error": {"code": "item_not_ready", "message": "Still extracting"}}
```

| HTTP | `code` | When |
|---|---|---|
| 400 | `bad_request` | invalid parameter or JSON |
| 401 | `unauthorized` | missing/invalid token or wrong pairing code |
| 404 | `not_found` | nonexistent route, item or file |
| 409 | `item_not_ready` | the item exists but is still being prepared |
| 412 | `revision_changed` | the item changed since the copy started |
| 416 | `bad_range` | invalid `Range` |
| 426 | `protocol_mismatch` | incompatible major version |
| 429 | `too_many_attempts` | too many pairing attempts |

## 7. Mac side: lifecycle of an item

```
detected → stable → (extracting) → identifying → (converting) → verifying → ready
              ↘───────────────────────↘──────────────────↘──→ needs attention
```

### 7.1 Stable
Size and modification date unchanged for 10 s, and no in-progress download file nearby (`.part`, `.crdownload`, `.download`, `.tmp`). For folders, this applies to every file inside.

### 7.2 Identifying
Rule order: sidecar file (§1.6) → folder with `sce_sys/param.json` → folder with `sce_sys/param.sfo` → PS5 image extension → `.pkg` (§1.4) → Switch extensions → archive (§1.3). No safe rule → "needs attention", where you choose platform, type and Title ID by hand (which writes the sidecar file).

The Mac looks for items up to 2 levels below the library root. A folder recognized as a game is not searched inside for other items.

### 7.3 Duplicates
Two items with the same `platform`, `title_id`, `content` and `version` → the most recent one (`added_at`) gets `duplicate_title_id`. Both remain in the catalog.

### 7.4 Converting (PS5 `folder` → `exfat`, optional, off by default in the app)
Without conversion, the folder enters the catalog as `folder` as soon as it is stable.

Same method as ShadowMountPlus's `mkexfat_macos.sh`:
1. Requires `eboot.bin` at the folder's root.
2. Cluster: 64 KiB when the average file size is ≥ 1 MiB; otherwise 32 KiB. Configurable (force 64 KiB).
3. Image size: sum of the files rounded up to whole clusters + 4 bytes per cluster (FAT) + 1 bit per cluster (bitmap) + 256 bytes per file and per folder + a fixed 32 MiB; slack = 0.5% of that, clamped between 64 MiB and 512 MiB; total at least (raw data + 64 MiB); rounded up to whole MiB.
4. `mkfile -n <MiB>m` → `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse` (the image does not appear in the Finder) → `newfs_exfat -b <cluster> -v <TITLE_ID>` on the whole device (no partition table) → `diskutil mount -mountPoint <folder>` (does not need `sudo`; if it fails, `mount -t exfat`) → copy the folder's content to the image root → unmount and `hdiutil detach`.
   The copy is done file by file, without extended attributes (exFAT would store them as `._*`) and without `.DS_Store`.
5. Writes as `<TITLE_ID>.exfat.ponte-tmp` and renames to `<TITLE_ID>.exfat` at the end. On any failure: unmounts, detaches, deletes the temporary file, keeps the folder and marks "needs attention".
6. Before starting, checks free space ≥ the computed size.
7. Writes the sidecar file with the folder's metadata and copies `sce_sys/icon0.png` to `<TITLE_ID>.exfat.ponte.png`.
8. After success, the original folder is deleted or moved to `_originais/` (configurable).

### 7.5 Verifying (optional, off by default in the app)
`sha256` of each file in the background. The item enters the catalog before this finishes (`sha256: null`).
With verification off, the server neither computes nor reports hashes (`sha256: null` always, no `X-Ponte-Sha256`); the client checks only the size and does not re-read what it already had when resuming.

### 7.6 Copies in progress and history (Mac only)
The Mac measures copies by what it serves itself; nothing changes for the clients.
- A copy is the pair (item, console). Console = the token (the PS5 installer uses the same one, via `?token=`) or the IP without pairing.
- Progress: for each file, the furthest point already delivered (start of the `Range` + bytes sent), so what the console already had when resuming counts. Speed: bytes sent in the last 5 s.
- It goes to the history when the `report` arrives (§5.8), or as `interrupted` after 2 min without sending and without a `report`.
- `.pkg`: the `installed` `report` arrives before the system installer downloads the package. The download that comes afterwards (up to 30 min) completes the same record; it closes 30 s after the last send.
- The history (last 500 copies) lives in the Mac's state folder (`history.json`), never in the library.

## 8. Client side: rules

1. On opening: discovers the Mac, sends `console/state`, shows the annotated catalog, grouped by game (base + updates + DLC).
2. You choose the items and the storage. The client checks free space (`size` + 5% margin) before starting.
3. **Temporary area outside `homebrew/`**: everything is downloaded to `<destination>/ponte/tmp/<id>/`, which the mounter does not watch. Only at the end is the item **moved** (a rename on the same disk, instantaneous) to `<destination>/homebrew/`. This prevents the mounter from registering a half-copied game.
4. Resuming: if `<destination>/ponte/tmp/<id>/` exists from a previous copy with the same `revision` (stored in `ponte/tmp/<id>/.revision`), it continues where it stopped using `Range`; otherwise, it deletes and restarts.
5. Checks the size and, if available, the `sha256` of each file before moving or installing.
6. Destination already taken (same name in `homebrew/`) → asks whether to replace it.
7. `pkg_*`: on the PS5, the client hands the system installer the URL of the file on the Mac (`.../files/0/<name>.pkg?token=…`, §5.5) and the installer downloads it directly, without going through the temporary area (no double space needed). Report `installed` = the system accepted the installation; it continues in the background on the console.
8. Order within a game: base → update → DLC.
9. Always sends a `report`, including on failure or cancellation.
10. Paths (`homebrew/`, `ponte/tmp/`, the roots of each storage) live in the client's configuration file.

### 8.1 Switch client (Ponte app)

On the Switch the game is not copied first and installed afterwards: the file goes **straight from the network to the installer** of Sphaira (yati), content by content. A prior copy to the card would require twice the space. The principle of never writing directly to the final destination still holds: the installer writes to system *placeholders* and only registers the content once it has arrived complete and verified. Therefore, relative to the rules above:

- **No temporary area** (rules 3, 4 and 7 do not apply). The installer reads the file with an open `Range` (`bytes=N-`) and, if the connection drops, requests again from where it stopped for up to 60 s. Every file request carries `X-Ponte-Revision`; `412` ends that item as `failed` (the console refreshes the list and the item can be chosen again).
- **Verification** (rule 5): instead of the file's `sha256`, the installer checks the SHA-256 of each NCA against the CNMT. `X-Ponte-Sha256` is ignored.
- **Space** (rule 2): sum of the chosen `size` values + 5% against the `free` of the destination storage (`sd` or `nand`), chosen at install time. For `nsz`/`xcz` this is an underestimate: once installed, the content takes up the decompressed size, which the Mac does not report.
- **Order** (rule 8): per game, base → updates from oldest to newest → DLC. A failure does not stop the following items; a cancellation does.
- **`report`** (rule 9): `installed` when finished, `failed` with `detail`, or `cancelled`. Never `copied`.
- **Quick selection**: selecting a game that is not on the console also selects its newest update and the missing DLC.

`console/state` on the Switch:
- `firmware`: system version as `X.YY` (e.g. 19.0.1 → `19.00`).
- `storages`: `nand` (console memory) and `sd`.
- `installed`: one record per installed content, with the **content's own Title ID** (an update ends in `800`; a DLC uses its own), `version` as an integer in text and `content` by meta type (`Application` → `base`, `Patch` → `update`, `AddOnContent` → `dlc`).

Other client details:
- Discovery: sends `PONTE?1` to the subnet broadcast and to `255.255.255.255`, repeating every 400 ms for ~1.2 s.
- Pairing: the console name is the nickname configured on the Switch. `401` on `console/state` with a saved token = pairing revoked on the Mac: the token is deleted and the code is asked for again.
- With the library open, it queries `catalog/changes` every 20 s and refreshes the list when there is something new.
- Configuration: `/config/sphaira/ponte.ini`, one section per server `id` (`name`, `address`, `token`).

## 9. Open items

- [ ] Read `param.json` from inside `.exfat` without sidecar metadata (images not created by Ponte). A read-only exFAT reader, just to find one file.
- [ ] Same for `.ffpkg` (UFS) and `.ffpfs` (PFS), which macOS does not mount.
- [ ] Full XCI/XCZ metadata and Switch game names (require `prod.keys`).
- [ ] Loader for `folder_ps4`.
- [ ] Register games directly from the payload, without depending on the external mounter.
- [ ] DLC for PS5 games as a folder or image: today folders and images are always `base` (§1.1) and PS5 `.pkg` is blocked on 13.60. How the mounter handles standalone DLC must be known before defining `kind`, identification and destination. (PS4 DLC as `.pkg` and Switch DLC already work.)
- [x] Copy progress visible on the Mac in real time — solved on the Mac side only, without changing the protocol (§7.6).

## 10. History

**draft 4** (the protocol is still `1`: compatible additions only)
- §9: DLC for PS5 games as a folder or image added as an open item. No protocol change.
- Conversion to `.exfat` (§7.4), `sha256` verification (§7.5) and archive extraction (§1.3) become optional and off by default in the Mac app; without a hash, the client checks only the size.
- §8.1: rules for the Switch client, the Ponte app (installation straight from the network, no temporary area; content of `console/state` on the Switch).
- The file route accepts `?token=` and a name at the end, so the PS5 installer can download `.pkg` directly from the Mac (§5.5, §8 item 7).
- `.DS_Store` and `._*` are kept out of items (§5.4).
- Archive extraction detailed: extractor, temporary folder, flattening (§1.3).
- Conversion to `.exfat`: mounting with `diskutil` without `sudo`, volume label and copying without extended attributes (§7.4).
- `tools/conformance.py`: black-box test of the specification, used on the Python reference and on the Swift server.
- Copies in progress and history on the Mac, measured by the server (§7.6); closes the §9 item without changing the protocol.
- In-progress download markers (§7.1) revised; the 10-second-unchanged rule still applies to any program. No protocol change.
- Wording: the 13.60 profile and the `state_reason` example (§1.1.1, §5.7) refer to "PS5 .pkg package". No protocol change.

**draft 3**
- Temporary area moved outside `homebrew/` (the mounter could register a half-copied game).
- Stable `id` defined; `revision` + `412 revision_changed` so as not to resume over a file that changed.
- `files` order and `path` rules fixed, so that `index` is stable.
- Per-firmware compatibility profiles in files, with a selection rule.
- `state` rules with precedence and per-platform version comparison.
- Exact rules for the Switch Title ID and for reading `.pkg`, `param.sfo` and `param.json`.
- `.ponte.json` sidecar file to preserve metadata after conversion to `.exfat`.
- Keep-alive, `HEAD`, 64-bit integers, `X-Ponte-Protocol`, pairing attempt limit, errors 400/412/429.
- `region` removed from the catalog (no reliable source).

**draft 2** — all PS5 and Switch formats; `console/state`; 13.60 profile; folder → `.exfat` conversion.

**draft 1** — initial version.
