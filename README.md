# Conway's Game of Life

*[中文说明](README.zh.md)*

An interactive terminal implementation of Conway's Game of Life, written in C
and built with CMake. It is an **unbounded sandbox**: the world has no walls and
is stored sparsely (only the live cells are kept, in a hash set), so memory and
per-generation cost scale with the population, not with any area — a glider just
keeps travelling forever. You **pan** the view by dragging with the mouse,
**zoom** with the wheel, and can drop or draw patterns anywhere. The board is
drawn with **sixel** real-pixel graphics, with an on-screen button bar.

> **Requires a sixel-capable terminal** (e.g. iTerm2, Konsole, WezTerm, foot,
> `xterm -ti vt340`, mlterm, recent Windows Terminal). Terminals without sixel —
> including the default macOS Terminal.app — are not supported; the program
> exits with a message on startup.

## Build

Two CMake presets are provided: `debug` and `release`.

```sh
cmake --preset release
cmake --build --preset release
# or: cmake --preset debug && cmake --build --preset debug
```

The binary is written to `build/<preset>/game-of-life`.

If **OpenMP** is available it is used automatically to spread each generation's
work across CPU cores (a big help on large patterns); without it the program
builds and runs single-threaded. Set `OMP_NUM_THREADS` to tune the core count.

## Install / Uninstall

Installation goes to the user's home by default (no root needed):

- the executable → `~/.local/bin/game-of-life`
- the default pattern → `~/.local/share/game-of-life/saves/default.rle`
  (respects `XDG_DATA_HOME`; this is the file the program auto-loads when run
  without `-f`, and it sits alongside your own saved patterns)

```sh
cmake --build --preset release          # build first
cmake --install build/release           # install
# or from the build dir: cd build/release && make install

cmake --build build/release --target uninstall
# or from the build dir: cd build/release && make uninstall
```

Notes:

- Make sure `~/.local/bin` is on your `PATH` to run `game-of-life` directly.
- Installing never overwrites an existing `default.rle` you have customised.
- **uninstall** removes the executable, the installed `default.rle`, and the
  `settings.json` file (and the config/data directories if they end up empty). It
  never deletes your own saved patterns.
- For a system-wide install, override the prefix:
  `cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local` (then `sudo cmake
  --install build/release`).

## Run

```sh
./build/release/game-of-life                              # default / remembered
./build/release/game-of-life -f saves/glider.rle         # load a pattern file
./build/release/game-of-life -f saves/pulsar.rle -w 40 -h 24       # seed-region size
./build/release/game-of-life --help                      # all options
```

A sixel-capable interactive terminal is required (see [Requirements](#conways-game-of-life)).

## Controls

The bar below the image has eight buttons:
**Start / Pause / Step / Reset / Edit / Jump / Save / Load**. Click a button with
the mouse, or move the selection with `Tab`/arrows and press `Space`/`Enter`.
There is no Quit button — `q` (or `Ctrl-C`) quits from any screen.

The board is drawn as a sixel bitmap: each cell is a square block of pixels whose
size is the current zoom level.

### Normal mode (button bar)

| Key | Action |
| --- | --- |
| **drag** (left button) | pan the view (grab-and-drag) |
| **mouse wheel** | zoom in / out, anchored on the cursor |
| `+` / `-` | speed the simulation up / slow it down |
| `c` | recentre the view on the pattern |
| `f` | toggle follow mode (auto-recentre every generation) |
| `j` | jump to a generation (see below) |
| `x` | clear the world to a blank slate |
| `s` / `l` | save / load a pattern file (RLE — see below) |
| `Tab` or `Right` | move selection to the next button |
| `Left` | move selection to the previous button |
| `Space` or `Enter` | activate the selected button |
| `q` / `Ctrl-C` | quit immediately |

- **Start** — begin, or resume from pause.
- **Pause** — freeze the simulation (Start resumes from where it stopped).
- **Step** — advance exactly one generation, then pause. Works from any
  paused/reset state, so you can watch the simulation frame by frame.
- **Reset** — reload the initial configuration (generation 0).
- **Edit** — enter edit mode (see below).
- **Jump** — jump to any generation, forward or back (see below).

The status line shows the state, generation, camera position `Cam: (x,y)`, the
live-cell count, the current zoom (`Zoom: Npx`, pixels per cell), and whether
follow mode is on.

### Exploring the world (pan, zoom, recenter, follow)

The world is unbounded, so the terminal shows a **viewport** into it:

- **Drag with the left button** to pan (the point under the cursor stays under
  it).
- **Mouse wheel** to zoom, anchored on the cursor — from chunky cells, down
  through one pixel per cell, and further into **sub-pixel** zoom where each
  screen pixel stands for a block of cells (a pixel lights if any cell in the
  block is alive). Sub-pixel zoom lets a pattern far larger than the screen be
  seen whole. The status line shows `Zoom: Npx` (N pixels per cell) or
  `Zoom: 1px=Nc` (one pixel per N cells).
- **`c`** recentres the view on the live cells' bounding box — handy when a
  pattern has drifted off-screen.
- **`f`** toggles **follow mode**, which recentres every generation so you can
  watch a spaceship travel without it leaving the screen.

### Edit mode

Draw or modify the configuration by hand. A blinking outline (a yellow border
around the cell) marks the current cell; it flashes so you can still see whether
that cell is alive or dead. The cursor roams the unbounded world with the arrow
keys and the view follows it.

| Key | Action |
| --- | --- |
| arrow keys | move the cursor (the view follows) |
| `Space` or `Enter` | toggle the cell under the cursor (alive/dead) |
| `Tab` or `Esc` | leave edit mode |

On leaving, the edited world becomes the new restart configuration, so **Reset**
returns to what you drew.

### Jump (rewind & fast-forward)

Press **`j`** (or the **Jump** button), type a target **generation**, and press
`Enter` to leap there — forward or backward. `Esc` cancels the prompt.

- **Backward** works because recent generations are kept in a history ring, so a
  rewind to a nearby generation is instant. A rewind further back than the ring
  reaches replays from generation 0 (Conway's Life is irreversible — a previous
  state can't be computed, only recalled or re-derived).
- **Forward** fast-forwards by actually running the simulation; the progress is
  shown and it stays interruptible throughout. During a jump, **`q` / `Ctrl-C`
  quit the program immediately** and **`Esc`** aborts just the jump (input is
  serviced continuously, so quit is honoured even mid-jump on a huge pattern).
  Very long jumps on patterns whose population grows without bound (glider guns,
  breeders) are still slow and memory-hungry — inherent to the current engine
  (though the generation step is multi-core; see [Build](#build)) — which is why
  the jump is interruptible.

### Saving & loading patterns (RLE)

Press **`s`** (or the **Save** button) and **`l`** (or **Load**) to open the
save/load browser.

- **Save** — type a name and press `Enter`; `.rle` is added automatically and the
  file is written to your saves folder. If the name already exists you are asked
  to confirm the overwrite.
- **Load** — a scrollable list of your saved patterns with **Name / Size /
  Modified** columns. Move with the arrow keys (or the mouse wheel), press
  `Enter` or click a row to load it, `d` to delete (with confirmation), and
  `n`/`s`/`m` (or click a column header) to sort — press again to reverse. If the
  current world isn't empty, loading asks before replacing it. To load a file
  from anywhere else, press `/` (or click **[Type a path…]**) and type a path —
  handy for patterns downloaded from the wiki.

Saved patterns live in `$XDG_DATA_HOME/game-of-life/saves/` (usually
`~/.local/share/game-of-life/saves/`), kept separate from your settings. The
startup default pattern lives there too, as `default.rle`.

The format is the community-standard **RLE** (the one Golly and
[LifeWiki](https://conwaylife.com/wiki/) use). Loading centres the pattern and
zooms to fit; saving writes every live cell of the current world. For example, a
saved glider is:

```
x = 3, y = 3, rule = B3/S23
bo$2bo$3o!
```

(The `.cells` format is still accepted for an explicit `-f`; RLE is the format
for in-app save/load and the default pattern because it stays compact.)

### Sixel rendering

The board is drawn as a real **sixel** bitmap: each cell becomes a block of
pixels, so the viewport is limited only by the terminal's **pixel** dimensions —
it fills the whole window at any zoom. Zooming out past one pixel per cell enters
sub-pixel zoom (a block of cells per pixel), so even a pattern much larger than
the screen can be viewed whole.

Sixel support is detected at startup via a Device Attributes query. If your
terminal supports sixel but is not detected, force detection on:

```sh
GOL_SIXEL=1 game-of-life   # skip the query, assume sixel is available
```

Setting `GOL_SIXEL=0` forces detection off, in which case the program will
report that a sixel-capable terminal is required and exit.

## Settings persistence

Parameters are remembered between runs in a JSON file:

```
$XDG_CONFIG_HOME/game-of-life/settings.json
# or, if XDG_CONFIG_HOME is unset:
~/.config/game-of-life/settings.json
```

- The effective settings — seed-region size, delay and density — are written to
  this file on **every** run (creating it on the first run), so a configuration
  always carries over to the next run.
- Command-line options that map to a stored setting (size, delay, density) are
  applied for the run **and** persisted, so an option you pass (e.g. `-w 50`)
  becomes the remembered value next time. `-s`/`-f` are not stored.

Example `settings.json`:

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
world itself is unbounded). `world`/`wrap` are always `2`/`false` now and kept
only for backward compatibility with older versions.

## Options

| Option | Description | Default |
| --- | --- | --- |
| `-w, --width N` | Seed-region width in cells | 30 (or remembered) |
| `-h, --height N` | Seed-region height in cells | 20 (or remembered) |
| `-d, --delay MS` | Delay between generations (ms) | 120 |
| `-p, --density F` | Random initial live-cell probability (0..1) | 0.25 |
| `-s, --seed N` | Random seed (default: time-based) | — |
| `-f, --file PATH` | Load initial config from a pattern file | default path |

### Default pattern

If `-f` is not given, the program looks for a default pattern file at
`~/.local/share/game-of-life/saves/default.rle` (respecting `XDG_DATA_HOME`). If
that file exists it is loaded; otherwise the board falls back to a random start
(density `-p`, default 0.25). An explicit `-f PATH` that cannot be read is a
hard error, but a missing default file is not — it just triggers the random
fallback.

`make install` puts a glider there as `default.rle`. To use a different default,
overwrite it (or just save over it from inside the app with the name
`default`), e.g.:

```sh
mkdir -p ~/.local/share/game-of-life/saves
cp saves/pulsar.rle ~/.local/share/game-of-life/saves/default.rle
```

### Included patterns

The `saves/` directory holds a set of classic Life patterns as `.rle`. **Install
copies them into your saves folder**, so they show up straight away in the in-app
**Load** browser (`l`). You can also load one at startup with
`-f saves/<name>.rle`:

| File | Type |
| --- | --- |
| `default.rle` | Glider (installed as the auto-loaded default) |
| `glider.rle` | Glider — spaceship, period 4 |
| `lwss.rle` | Lightweight spaceship |
| `blinker.rle` | Oscillator, period 2 |
| `toad.rle` | Oscillator, period 2 |
| `beacon.rle` | Oscillator, period 2 |
| `pulsar.rle` | Oscillator, period 3 |
| `pentadecathlon.rle` | Oscillator, period 15 |
| `block.rle` | Still life |
| `beehive.rle` | Still life |
| `r-pentomino.rle` | Methuselah (stabilises after 1103 gens) |
| `acorn.rle` | Methuselah (stabilises after 5206 gens) |
| `diehard.rle` | Methuselah (vanishes after 130 gens) |
| `glider-gun.rle` | Gosper glider gun |

## Pattern formats

The interchange format throughout is the community-standard **RLE** (`saves/` and
the in-app Save/Load). The classic `.cells` plaintext format is still accepted for
an explicit `-f name.cells` if you have one:

- Lines beginning with `!` or `#` are comments.
- In pattern lines, `.` or space is a dead cell; any other character
  (typically `O`) is a live cell.

```
! Glider
.O.
..O
OOO
```

## License

This program is free software, licensed under the GNU General Public License
version 3 (GPLv3). See the [LICENSE](LICENSE) file for the full text.
