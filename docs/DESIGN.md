# Ponte visual standard

**English** · [Português](DESIGN.pt-BR.md)

The three apps (Mac, PS5 and Switch) use the same colors, the same progress anatomy and the same size, speed and time texts. Anyone who has used one recognizes the others.

## Colors

Dark theme (PS5 and Switch always; Mac in dark mode):

| Token | Hex | Use |
|---|---|---|
| `bg` | `#10141C` | background |
| `panel` | `#181E29` | cards, rows, panels |
| `line` | `#273042` | borders, dividers, **bar track** |
| `input` | `#0D1118` | fields and secondary buttons |
| `text` | `#E8ECF3` | main text |
| `muted` | `#8E99AD` | secondary text (status, details) |
| `accent` | `#3FB6C4` | **Ponte teal**: main action, bar in progress, speed, focus |
| `accent-ink` | `#062A2F` | text on `accent` |
| `ok` | `#4CC38A` | done, installed, connected |
| `warn` | `#E5B450` | paused, waiting, needs attention |
| `bad` | `#EF6B6B` | failed, error |

Light theme (Mac only, in the system's light mode): `accent #0B8794`, `ok #1E9E63`, `warn #B97F06`, `bad #D23C3C`, track `#E2E7EE`, card `#F3F5F8`. Text and background follow the system.

Rules:
- **Color is never the only cue**: state is always also shown as text ("pausado" (paused), "falhou" (failed)) or an icon.
- Bar in progress = `accent`; paused/waiting = `warn`; done = `ok`; failed = `bad`; track = `line`.
- A single accent color per screen. The platforms (PS5, Switch) have no color of their own; they use an icon or text.

## Progress row

Anything that takes more than a moment and has a size shows the same row, in every app:

```
[icon] Title ................................... 37%
████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min
detail: step, console, current file
```

- **Percentage** to the right of the title, in the bar's color. With no known total: indeterminate bar (a moving stripe) and no percentage.
- **Status**: `<done> de <total> · <speed> · faltam <time>` (“<done> of <total> · <speed> · <time> left”), in that order, only what is known. Paused: the reason in its place ("pausado: um console está copiando; continua sozinho" (paused: a console is copying; resumes on its own)).
- **Detail**: one line, truncated in the middle if long.
- Speed: average of the last ~5 s; stalled for more than 2 s shows 0 (not the old average).
- Time remaining: from bytes when there is a total, otherwise from the percentage; under 1 min, "falta menos de 1 min" (less than 1 min left).

Where it appears:

| App | Operations with bar and speed |
|---|---|
| Mac | copies to the consoles, archive extraction, folder → `.exfat` conversion, verification (sha256) |
| PS5 | verification of what was already there (resume), download, final verification, moving to `homebrew/`, handing off the `.pkg`, diagnostics |
| Switch | installation of each item (download + writing), whole queue |

## Texts

- Sizes in base 10, like the Finder: `1,2 GB`, `38 MB/s` (decimal comma in Portuguese).
- Duration: `45 s`, `12 min`, `1 h 5 min`.
- States: `na fila` (queued), `copiando` (copying), `pausado` (paused), `conferindo` (verifying), `movendo` (moving), `instalando` (installing), `concluído` (done), `falhou` (failed), `cancelado` (canceled).
- Error messages say what happened **and** what to do ("O Mac não respondeu. Confira se o app está aberto; a cópia continua sozinha quando ele voltar." (The Mac did not respond. Check that the app is open; the copy resumes on its own when it comes back.)).

## Where it is in the code

| App | File |
|---|---|
| Mac | `mac/Sources/PonteMac/Brand.swift` (`Brand`, `ProgressRow`, `Fmt`) |
| PS5 | CSS variables at the top of `ps5/ui/index.html` |
| Switch | `switch/app/sphaira/source/ui/menus/ponte.cpp` (brand colors) and the default theme |
