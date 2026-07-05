# Conway's Game of Life

*[中文说明](README.zh.md)*

An interactive terminal implementation of Conway's Game of Life, written in C
and built with CMake. Keyboard-driven, with a square cell grid and an on-screen
button bar. The world can be **finite** (patterns that leave the edge vanish —
the default), **toroidal** (edges wrap around), or **infinite** (an unbounded,
sparsely-stored world you pan around). Board size, world type and other
parameters are remembered between runs.

## Build

Two CMake presets are provided: `debug` and `release`.

```sh
cmake --preset release
cmake --build --preset release
# or: cmake --preset debug && cmake --build --preset debug
```

The binary is written to `build/<preset>/game-of-life`.

## Install / Uninstall

Installation goes to the user's home by default (no root needed):

- the executable → `~/.local/bin/game-of-life`
- the default pattern → `~/.config/game-of-life/default.cells`
  (respects `XDG_CONFIG_HOME`; this is the file the program auto-loads when run
  without `-f`)

```sh
cmake --build --preset release          # build first
cmake --install build/release           # install
# or from the build dir: cd build/release && make install

cmake --build build/release --target uninstall
# or from the build dir: cd build/release && make uninstall
```

Notes:

- Make sure `~/.local/bin` is on your `PATH` to run `game-of-life` directly.
- Installing never overwrites an existing default config you have customised.
- **uninstall** removes the executable, the default pattern and the
  `settings.json` file (and the `game-of-life` config directory if it ends up
  empty).
- For a system-wide install, override the prefix:
  `cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local` (then `sudo cmake
  --install build/release`).

## Run

```sh
./build/release/game-of-life                              # default / remembered
./build/release/game-of-life -f patterns/glider.cells    # load a pattern file
./build/release/game-of-life -f patterns/pulsar.cells -w 40 -h 24
./build/release/game-of-life --help                      # all options
```

An interactive terminal is required.

## Controls

The bar below the grid has six buttons:
**Start / Pause / Step / Reset / Edit / Canvas**. There is no Quit button — `q`
(or `Ctrl-C`) quits from any screen.

Each cell is drawn two characters wide so the grid looks square despite the
terminal's tall character cells.

### Normal mode (button bar)

| Key | Action |
| --- | --- |
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
- **Canvas** — enter canvas mode: resize and/or switch world type (see below).

### Edit mode

Draw or modify the initial configuration by hand. A blinking hollow-square
cursor (`[]`) marks the current cell; it flashes so you can still see whether
that cell is alive or dead.

| Key | Action |
| --- | --- |
| arrow keys | move the cursor |
| `Space` or `Enter` | toggle the cell under the cursor (alive/dead) |
| `Tab` or `Esc` | leave edit mode |

On leaving, the edited grid becomes the new initial configuration, so **Reset**
returns to what you drew.

### Canvas mode

Change the board dimensions and/or the world type. An existing pattern is
preserved, centered inside the new size (cells outside the new bounds are
clipped).

| Key | Action |
| --- | --- |
| digits `0`–`9` | type a number straight into the focused field |
| `Backspace` | delete the last digit |
| `Up` / `Down` or `Tab` | move focus between the Width and Height fields |
| `Left` / `Right` | nudge the focused field by ∓1 |
| `Space` | cycle world type: Finite → Toroidal → Infinite |
| `Enter` | apply (and remember the new settings) |
| `Esc` | cancel |

Just type the size you want — e.g. focus Width, type `120`, press `Down`, type
`80`. Sizes range from 3 up to the largest board that fits the current terminal.
Applying only resets the simulation to generation 0 when the size actually
changed, so a pure Finite↔Toroidal switch leaves a running simulation intact.
Applied changes are saved to `settings.json` (see below).

### Fitting the terminal

The board is automatically kept within the terminal window: on startup the
requested/remembered size is clamped to fit, and the maximum size in canvas mode
is whatever fits. If you resize the terminal window while the program is
running, the board adapts — it shrinks to fit a smaller window (it is not forced
to grow when the window gets larger). A very large `-w`/`-h` is simply clamped to
what the terminal can show.

### Sixel graphics (much larger boards)

If your terminal supports **sixel** graphics (Konsole, foot, WezTerm, xterm
`-ti vt340`, mlterm, recent Windows Terminal, …), the board is drawn as a real
bitmap instead of text cells. Each cell becomes a block of pixels, so the board
is limited by the terminal's **pixels**, not by its character grid — that is
roughly an 8× jump in how many cells fit on screen, and it lets the world grow to
hundreds of cells across. The image auto-scales (zoom-to-fit): small boards get
chunky cells, large boards get one-pixel cells.

Sixel is detected automatically at startup (a Device Attributes query). To force
it, set an environment variable:

```sh
GOL_SIXEL=1 game-of-life   # force sixel on
GOL_SIXEL=0 game-of-life   # force the plain text renderer
```

If the terminal cannot report its pixel size, or does not support sixel, the
program falls back to the text grid automatically.

### World type: finite, toroidal, infinite

- **Finite** (default) — cells beyond the border are dead. Patterns that travel
  off an edge collide with the wall and disappear (a glider crashing into a
  corner leaves a small still-life remnant).
- **Toroidal** — the edges wrap around (top↔bottom, left↔right), so a glider
  that leaves one edge reappears on the opposite one.
- **Infinite** — an unbounded world with no walls. It is stored sparsely (only
  the live cells are kept, in a hash set), so memory and per-generation cost
  scale with the population, not with any area. A glider just keeps travelling
  forever. The terminal shows a **viewport** into this world; move it around
  with the arrow keys (see below).

Start with `--wrap` (toroidal) or `--infinite`, or switch any time in canvas
mode. Finite and Toroidal use a dense grid engine; Infinite uses the sparse one.

### Panning the infinite world

In the infinite world the board no longer has a fixed size, so in normal mode
the **arrow keys pan the viewport** and `Tab` cycles the buttons (in bounded
worlds the arrows also cycle buttons, as there is nothing to pan). The status
line shows the camera position `Cam: (x,y)` and the live-cell count. In edit
mode the cursor roams the unbounded world and the view follows it. Switching
from Infinite back to a bounded world adopts the current viewport as the new
finite canvas.

## Settings persistence

Parameters are remembered between runs in a JSON file:

```
$XDG_CONFIG_HOME/game-of-life/settings.json
# or, if XDG_CONFIG_HOME is unset:
~/.config/game-of-life/settings.json
```

- On first run (when no settings file can be read) the file is created with the
  effective settings.
- When you apply changes in **Canvas** mode, the new board size and world type
  are written back, so the next run starts the same way — no need to reconfigure
  each time.
- Command-line options override the stored values for that run.

Example `settings.json`:

```json
{
  "width": 30,
  "height": 20,
  "wrap": false,
  "world": 0,
  "delay_ms": 120,
  "density": 0.250
}
```

`world` is `0` = finite, `1` = toroidal, `2` = infinite. (`wrap` is kept for
backward compatibility with older versions.)

## Options

| Option | Description | Default |
| --- | --- | --- |
| `-w, --width N` | Board width in cells | 30 (or remembered) |
| `-h, --height N` | Board height in cells | 20 (or remembered) |
| `-d, --delay MS` | Delay between generations (ms) | 120 |
| `-p, --density F` | Random initial live-cell probability (0..1) | 0.25 |
| `-s, --seed N` | Random seed (default: time-based) | — |
| `-f, --file PATH` | Load initial config from a pattern file | default path |
| `--wrap` | Start in toroidal (wrap-around) world | finite |
| `--infinite` | Start in the unbounded (sparse) world | finite |

### Default pattern

If `-f` is not given, the program looks for a default pattern file at
`~/.config/game-of-life/default.cells` (respecting `XDG_CONFIG_HOME`). If that
file exists it is loaded; otherwise the board falls back to a random start
(density `-p`, default 0.25). An explicit `-f PATH` that cannot be read is a
hard error, but a missing default file is not — it just triggers the random
fallback.

`make install` puts a glider there as `default.cells`. To use a different
default, overwrite it, e.g.:

```sh
mkdir -p ~/.config/game-of-life
cp patterns/pulsar.cells ~/.config/game-of-life/default.cells
```

### Included patterns

The `patterns/` directory contains classic Life patterns you can load with
`-f patterns/<name>.cells`:

| File | Type |
| --- | --- |
| `default.cells` | Glider (installed as the default) |
| `glider.cells` | Glider — spaceship, period 4 |
| `lwss.cells` | Lightweight spaceship |
| `blinker.cells` | Oscillator, period 2 |
| `toad.cells` | Oscillator, period 2 |
| `beacon.cells` | Oscillator, period 2 |
| `pulsar.cells` | Oscillator, period 3 |
| `pentadecathlon.cells` | Oscillator, period 15 |
| `block.cells` | Still life |
| `beehive.cells` | Still life |
| `r-pentomino.cells` | Methuselah (stabilises after 1103 gens) |
| `acorn.cells` | Methuselah (stabilises after 5206 gens) |
| `diehard.cells` | Methuselah (vanishes after 130 gens) |
| `glider-gun.cells` | Gosper glider gun (needs a wide board) |

## Config file format

A subset of the classic `.cells` plaintext format (see `patterns/`):

- Lines beginning with `!` or `#` are comments.
- In pattern lines, `.` or space is a dead cell; any other character
  (typically `O`) is a live cell.
- The pattern is centered inside the board; cells outside the board are clipped.

```
! Glider
.O.
..O
OOO
```

## License

This program is free software, licensed under the GNU General Public License
version 3 (GPLv3). See the [LICENSE](LICENSE) file for the full text.
