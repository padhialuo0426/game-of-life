# Conway's Game of Life

*[中文说明](README.md)*

An interactive Conway's Game of Life for the terminal, written in C11 and built
with CMake. The world is an **unbounded sandbox**: no walls, live cells kept in
a sparse hash set, so memory and per-generation cost scale with the
**population**, never with area — a glider just keeps travelling forever. The
board is drawn with **real-pixel graphics** (Kitty graphics protocol preferred,
sixel fallback) and everything from the button bar to the dialogs can be driven
with the mouse alone.

## Features

- **Unbounded world** — sparse storage, limited only by the live-cell count;
  million-cell patterns load and run as usual.
- **Real-pixel rendering** — auto-detects the **Kitty graphics protocol** (KGP),
  falls back to **sixel**; in-place frame replacement plus synchronized output
  (DEC 2026) makes zooming, repaints and the running simulation **flicker-free**.
- **Mouse everywhere** — drag to pan, wheel to zoom anchored on the cursor;
  buttons, dialogs, list rows and sort headers are all clickable; clicking
  outside a dialog cancels it.
- **Deep zoom** — from 20 pixels per cell down to 1, then onward into
  **sub-pixel** zoom (up to 1 pixel = 256 cells), so a pattern far larger than
  the screen can be seen whole.
- **Transactional editor** — click/drag to draw (Bresenham-interpolated, fast
  strokes leave no gaps), Enter applies, Esc discards, one-click clear; toggle
  between drawing and panning.
- **Time travel** — Jump to any generation: nearby rewinds are **instant** (a
  history ring under a 256 MB memory budget), deeper ones replay from
  generation 0; fast-forward is interruptible throughout and quitting mid-jump
  takes effect immediately.
- **RLE save/load browser** — the community-standard RLE format; sortable
  Name/Size/Modified list with delete, overwrite/replace confirmations and
  load-any-path; **14 classic patterns** ship with the install.
- **Multi-core stepping** — with OpenMP, worlds above twenty thousand cells
  step in parallel across row bands, bit-for-bit identical to the serial path.
- **The small things** — follow mode to chase a spaceship, one-key recentre,
  ±10 ms speed nudges, settings remembered across runs, the terminal restored
  intact even on a crash, `NO_COLOR` honoured.

> **Requires a graphics-capable terminal**: either **KGP** (Kitty, Ghostty,
> WezTerm, Konsole ≥ 24.08) or **sixel** (iTerm2, Konsole, WezTerm, foot,
> `xterm -ti vt340`, mlterm, recent Windows Terminal). Terminals with neither —
> including the default macOS Terminal.app — are not supported; the program
> exits with a message on startup.

## Build

Two CMake presets are provided: `debug` and `release`.

```sh
cmake --preset release
cmake --build --preset release
# or: cmake --preset debug && cmake --build --preset debug
```

The binary lands in `build/<preset>/game-of-life`.

**zlib** is required (KGP payload compression; shipped with macOS and Linux).
If **OpenMP** is available, multi-core stepping is enabled automatically;
without it the program builds and runs single-threaded. Set `OMP_NUM_THREADS`
to tune the core count.

## Install / Uninstall

Installation goes to the user's home by default (no root needed):

- the executable → `~/.local/bin/game-of-life`
- the bundled patterns (including the auto-loaded `default.rle`) →
  `~/.local/share/game-of-life/saves/` (respects `XDG_DATA_HOME`; they sit
  alongside your own saved patterns)

```sh
cmake --build --preset release                     # build first
cmake --build build/release --target install-game   # install

cmake --build build/release --target remove-game    # uninstall
```

Notes:

- `install-game`/`remove-game` are explicit custom targets defined in
  CMakeLists.txt (CMake reserves the name `install` for the generator's own
  target, so it can't be a custom target too); the standard
  `cmake --install build/release` / `cmake --build build/release --target
  install` still work identically, these names are just more descriptive.
- Make sure `~/.local/bin` is on your `PATH` to run `game-of-life` directly.
- Installing **never overwrites** a pattern you already have (a customised
  `default.rle` is safe).
- Uninstall removes the executable, exactly the bundled patterns it installed,
  and `settings.json` (plus the directories if they end up empty) — **never**
  your own saved patterns.
- System-wide: `cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local`,
  then `sudo cmake --build build/release --target install-game`. Even under
  `sudo`, bundled patterns land in *your own* `~/.local/share`, not root's —
  the install script identifies the real invoking user via `SUDO_USER`.

## Run

```sh
game-of-life                             # default pattern / remembered config
game-of-life -f saves/glider-gun.rle     # load a pattern file
game-of-life --help                      # all options
```

## Controls

Below the image sit nine buttons, each labelled with its own shortcut key:

**Start/Pause (P) · Step (N) · Reset (R) · Edit (E) · Jump (J) · Save (S) ·
Load (L) · Help (?) · Quit (Q)**

Click a button, or move the selection with `Tab`/arrows and press
`Space`/`Enter`, or just hit the letter in the parentheses. The first button is
a **toggle**: it reads `Start` while stopped and `Pause` while running. `q` or
`Ctrl-C` quits from any screen, and **`?`** opens a quick in-app reference at
any time.

### Normal mode

| Input | Action |
| --- | --- |
| **left-drag** | pan the view (grab-and-drag; the grabbed point stays put) |
| **wheel** | zoom anchored on the cursor, 1 px per notch |
| `+` / `-` | speed up / slow down (±10 ms per press, 0–2000 ms) |
| `c` | recentre the view on the pattern's bounding box |
| `f` | follow mode: recentre every generation, chase a spaceship |
| `x` | clear the world to a blank slate |
| `P N R E J S L ?` | activate the matching button |
| `Tab` / `←` `→` | move the button selection |
| `Space` / `Enter` | activate the selected button |
| `q` / `Ctrl-C` | quit |

The status HUD floats over the top of the world and shows the run state (in
colour), generation, camera position, live-cell count, zoom level, frame delay
and the follow switch; Save/Load results appear as an **auto-hiding toast** at
the bottom right.

### Exploring the world (pan, zoom, recentre, follow)

The terminal shows a **viewport** into the unbounded world:

- **Drag** to pan; **wheel** to zoom. The ladder runs from 20 px per cell down
  to 1 px (one pixel per notch), then onward into **sub-pixel zoom** — each
  screen pixel stands for a block of cells (lit if any cell in the block is
  alive), up to 1 px = 256 cells. The status line shows `Zoom: Npx` or
  `Zoom: 1px=Nc`.
- **`c`** recentres once; **`f`** recentres every generation (follow).
- Loading a pattern **zooms to fit** automatically: small patterns keep
  comfortable chunky cells, huge ones zoom out until they fit the screen.

### Edit mode (transactional)

Press **`E`**. Editing is a **transaction**: the world is snapshotted on entry,
**Enter applies** (your drawing becomes the new generation-0 restart config)
and **Esc discards** (the pre-edit world is fully restored — even after a
clear).

| Input | Action |
| --- | --- |
| **left-click** | toggle that cell |
| **left-drag** | paint (the first cell's new state, gap-free on fast strokes) |
| **wheel** | zoom |
| arrows + `Space` | move the keyboard cursor / toggle the cell under it |
| `x` | clear all cells (stays in Edit; Esc still restores) |
| `Enter` | **Apply**: keep the edits, generation resets to 0 |
| `Esc` | **Discard**: restore the pre-edit world |

The bottom row becomes **[ Apply ] [ Discard ] [ Clear ] [ Pan: on/off ]**.
With **Pan** on, left-drag pans the world instead of painting, so a pattern can
be drawn past the visible area; a blinking outline marks the keyboard cursor.

### Jump (rewind & fast-forward)

Press **`J`**, type a target **generation**, and hit Enter — forward or
backward; `Esc` cancels. The wheel and drag still work while typing, so you can
frame the view first.

- **Backward**: recent generations live in a history ring (1024 generations,
  256 MB memory budget); within it a rewind is **instant**. Anything deeper
  replays from generation 0 (Life is irreversible — a previous state can only
  be recalled or re-derived, never computed).
- **Forward** actually runs the simulation, shows progress, and services input
  throughout — **`q`/`Ctrl-C` quit the program immediately**, **`Esc`** aborts
  just the jump. Long jumps on patterns whose population grows without bound
  (guns, breeders) still get slow — inherent to the current engine, which is
  why the jump is interruptible.

### Save & load (RLE)

Press **`S`** to save and **`L`** for the load browser. Every dialog is
mouse-friendly: footer buttons like **[ Save ] / [ Cancel ]**, and **clicking
outside the box** cancels.

- **Save** — type a name and press Enter (`.rle` is appended automatically);
  a duplicate name brings up a Yes/No overwrite confirmation.
- **Load** — a scrollable list with **Name / Size / Modified** columns. Move
  with arrows or the wheel, press Enter or click a row to load, `d` deletes
  (with confirmation), `n`/`s`/`m` or a header click sorts (again to reverse).
  If the world isn't empty, loading asks before replacing it. Press `/` (or
  click **[Type a path…]**) to load any path — handy for patterns downloaded
  from the wiki.

Saves live in `$XDG_DATA_HOME/game-of-life/saves/` (usually
`~/.local/share/game-of-life/saves/`), separate from the settings. The format
is the **RLE** used by Golly and the
[LifeWiki](https://conwaylife.com/wiki/); loading centres and zooms to fit,
saving writes every live cell of the current world. A glider, for example:

```
x = 3, y = 3, rule = B3/S23
bo$2bo$3o!
```

(The classic `.cells` plaintext format is still accepted for an explicit `-f`.)

## Rendering

The board is a real-pixel bitmap, so the viewport is limited only by the
terminal's **pixel** dimensions and fills the window at any zoom.

- Startup probes for the **Kitty graphics protocol** first (RGB frames →
  zlib → base64, transmitted under a fixed image id that is replaced in place,
  so terminal memory stays bounded); it falls back to **sixel**, and exits with
  a message if neither is available.
- Frames are deduplicated (an unchanged frame costs nothing), input is
  coalesced (fast drags don't back up), and clear-then-redraw is presented as
  one synchronized frame (no flashing).

Environment variables override the detection:

```sh
GOL_KITTY=1 game-of-life   # force KGP on (=0 disables it)
GOL_SIXEL=1 game-of-life   # force sixel on (=0 disables it)
```

## Engine backend

The default is the **sparse hash-set** engine (best for interactive editing and
small / chaotic patterns). Set `GOL_HASHLIFE=1` to switch to the **Hashlife**
(Gosper hashed-quadtree) backend:

```sh
GOL_HASHLIFE=1 game-of-life -f saves/glider-gun.rle
```

Hashlife collapses a jump (`j`) into a series of memoised power-of-two leaps, so
far-forward jumps on structured / repetitive patterns are near-instant — e.g.
advancing a glider one billion generations takes ~0.3 ms (the sparse engine
would have to compute every generation, which is intractable). The trade-off:
on chaotic / noisy patterns the node hash table grows, and v1 has no garbage
collection, so on hitting a memory cap it stops the jump with a notice — for
those interactive cases the default sparse engine is still the better choice.
The coordinate range matches the sparse engine (±2³⁰).

## Settings persistence

Parameters are remembered in `$XDG_CONFIG_HOME/game-of-life/settings.json`
(usually `~/.config/game-of-life/settings.json`):

- The effective settings — seed-region size, frame delay, density — are written
  back on **every** run, so a configuration always carries over.
- Command-line options that map to a stored setting (size, delay, density) are
  applied for the run **and** remembered; `-s`/`-f` are not stored.

```json
{
  "width": 30,
  "height": 20,
  "wrap": false,
  "world": 2,
  "delay_ms": 120,
  "density": 0.250
}
```

`width`/`height` size the seed region a random/loaded pattern starts in (the
world itself is unbounded); `world`/`wrap` are fixed at `2`/`false` and kept
only for compatibility with older versions.

## Options

| Option | Description | Default |
| --- | --- | --- |
| `-w, --width N` | Seed-region width in cells | 30 (or remembered) |
| `-h, --height N` | Seed-region height in cells | 20 (or remembered) |
| `-d, --delay MS` | Delay between generations (ms) | 120 |
| `-p, --density F` | Random initial live probability (0..1) | 0.25 |
| `-s, --seed N` | Random seed | time-based |
| `-f, --file PATH` | Load the initial config from a pattern file | default pattern |

### Default & bundled patterns

Without `-f`, the program loads
`~/.local/share/game-of-life/saves/default.rle`, falling back to a random start
if it is missing (only an explicit `-f` that cannot be read is a hard error).
To change the default, overwrite that file — or simply save over it from inside
the app under the name `default`.

Install copies the **14 classic patterns** from `saves/` into your saves
folder, so they appear straight away under **`L`**:

| File | Type |
| --- | --- |
| `default.rle` | Glider (the auto-loaded default) |
| `glider.rle` | Glider — spaceship, period 4 |
| `lwss.rle` | Lightweight spaceship |
| `blinker.rle` / `toad.rle` / `beacon.rle` | Oscillators, period 2 |
| `pulsar.rle` | Oscillator, period 3 |
| `pentadecathlon.rle` | Oscillator, period 15 |
| `block.rle` / `beehive.rle` | Still lifes |
| `r-pentomino.rle` | Methuselah (stabilises after 1103 gens) |
| `acorn.rle` | Methuselah (stabilises after 5206 gens) |
| `diehard.rle` | Methuselah (vanishes after 130 gens) |
| `glider-gun.rle` | Gosper glider gun |

## Pattern formats

The interchange format throughout is **RLE** (bundled patterns and in-app
save/load). The classic `.cells` plaintext is still accepted via an explicit
`-f name.cells`: lines starting with `!`/`#` are comments, `.` or space is
dead, any other character (typically `O`) is alive.

```
! Glider
.O.
..O
OOO
```

## License

Free software under the GNU General Public License version 3 (GPLv3). See
[LICENSE](LICENSE) for the full text.
