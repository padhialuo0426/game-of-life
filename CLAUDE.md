# CLAUDE.md — Conway's Game of Life (TUI)

Working notes for Claude Code. Read this first when picking up work on this repo,
especially from another machine. The Git repo is the single source of truth:
`git pull`, then read this file + `git log`.

## What this project is

An interactive Conway's Game of Life for the terminal, written in C11, built with
CMake. **Rendering is sixel-only** (real pixels, no text/character fallback), so
it **requires a sixel-capable terminal** (iTerm2, Konsole, WezTerm, foot,
`xterm -ti vt340`, …). The default macOS Terminal.app is NOT supported and the
program exits with a message on startup if sixel isn't detected.

**Single world type: an unbounded ("infinite") sandbox** — a sparse hash-set of
live cells with a pannable/zoomable viewport. There is no Finite or Toroidal
world any more, and no Canvas mode (both removed 2026-07-06 on Fedora; the product
direction was always the "true infinite sandbox"). The world is unbounded; `-w/-h`
now only size the seed region a random/loaded pattern starts in.

## Build / run / install

```sh
cmake --preset release
cmake --build --preset release          # -> build/release/game-of-life
cmake --install build/release           # -> ~/.local/bin/game-of-life (+ default.cells to ~/.config)
game-of-life                            # the sandbox (no flags needed)
game-of-life --help
```

Presets: `debug` and `release` (see `CMakePresets.json`). Warnings are on
(`-Wall -Wextra -Wpedantic`); keep the build warning-clean.

Convention this session: **after every successful build, run `cmake --install`**
so the installed `~/.local/bin` binary stays current for the user to test.

## Source layout

- `src/sparse.{c,h}` — the world engine: open-addressing hash set of live cells
  (SplitMix64 hash, backward-shift deletion). `sparse_step` tallies neighbours;
  `sparse_query(x0,y0,x1,y1,fn)` iterates the live cells in a rectangle (render).
- `src/sixel.{c,h}` — sixel bitmap encoder. Incremental `SixelCanvas`
  (`new/set_alive/set_cursor/encode/free`) plotted straight from the live-cell set;
  `sixel_render_board` is a thin Board wrapper on top. Palette: dead/alive/grid/cursor.
- `src/board.{c,h}` — dense `Board`, now used only as a transient seed buffer to
  lay out the initial random/`.cells` pattern before handing it to the sparse
  world (`seed_from_board`). Not on the render/step hot path any more.
- `src/terminal.{c,h}` — raw mode, key + SGR-mouse decoding, sixel detection
  (Primary DA query), alt-screen + mouse enable/disable.
- `src/config.{c,h}` — load `.cells` plaintext patterns (centered into a board).
- `src/settings.{c,h}` — JSON-ish persisted settings in `~/.config/game-of-life/settings.json`.
- `src/main.c` — UI state machine (Normal/Edit only), render loop, input handling,
  mouse pan/zoom, recenter/follow.

## Changes made this session (newest first, by commit)

- **(Fedora) Collapse to a pure infinite sandbox.** Removed the Finite and
  Toroidal world types, the `WorldType` enum, the dense stepping path, Canvas
  mode (the `BTN_CANVAS` button, `handle_canvas`/`canvas_apply`, resize/conversion
  helpers `resize_boards`/`sparse_to_dense`/`dense_to_sparse`), the `--wrap` /
  `--infinite` flags, and `fit_limits`/`handle_winch` board reallocation. `main.c`
  is now a two-mode (Normal/Edit) UI over the sparse world only; the initial
  pattern is laid out in a transient `Board` and handed off via `seed_from_board`.
  Buttons: **Start Pause Step Reset Edit** (5, no Canvas).
- **(Fedora) Recenter + follow.** `c` recentres the camera on the live cells'
  bounding box; `f` toggles a follow mode that recentres every generation. Status
  line shows `Follow: on/off`.
- **(Fedora machine) Infinite world: render directly from the sparse set.**
  Was next-step #4. `prepare_view` used to snapshot the whole viewport into a
  dense `Board` with one `sparse_get` per viewport cell (O(viewport) hash lookups
  — millions at 1px zoom), then encode. Now: `sparse_query(w, x0,y0,x1,y1, fn)`
  iterates only the live population and plots onto a new incremental
  `SixelCanvas` (`sixel_canvas_new/set_alive/set_cursor/encode/free` in sixel.c;
  `sixel_render_board` reimplemented on top of it). Dropped `App.view` and
  `prepare_view`; added `render_infinite()` + a shared `emit_frame()` tail.
  Output is **byte-identical** to the old path; measured **6.9× faster** per
  frame at 1000×1000 @ 1px (11.6→1.7 ms), unchanged at small viewports (encode
  dominates there). Process RSS still flat.
- `faa2fed` **Infinite world: mouse-wheel zoom + fix lag at large resolution.**
  - Wheel zoom anchored on the cursor; zoom level = pixels-per-cell in
    `[INFINITE_CELL_MIN=1 .. INFINITE_CELL_MAX=20]`, `ZOOM_STEP=2`. Stored in
    `App.infinite_cell_px`. Status line shows `Zoom: Npx`.
  - Viewport cap is now **dynamic**: `infinite_viewport()` clamps to the
    terminal's own detected pixel size (`avail_w`/`avail_h` from
    `terminal_pixel_size`), not a magic constant — fills any display (Retina/4K/8K)
    at any zoom down to the 1px floor. Removed the old `INFINITE_VIEW_MAX` constant.
  - **Input coalescing** in the main loop: drain all pending input with
    `terminal_read_key(0)` and render once, instead of once per event. Fixes the
    progressive drag lag (our own backlog) and cuts the number of full-screen
    sixel images emitted during a drag.
- `71d79d4` README: fixed mismatches vs code (settings persist every run + CLI
  options persist; zh panning section still said "arrows pan").
- `56fa106` README: removed all "plain-text rendering" wording; sixel-only.
- `5fe34e2` **Sixel-only + mouse drag-pan + iTerm2 memory mitigations.**
  - Removed the text/character renderer entirely; sixel is mandatory (error-exit
    if not detected). Simplified `fit_limits`/`infinite_viewport`/delta math.
  - SGR mouse parsing (`\033[<...M/m`, modes 1000;1002;1006) → `KEY_MOUSE` /
    `MouseEvent` / `terminal_mouse()`. Infinite world: **left-drag pans**
    (grab-and-drag, cursor-anchored); arrow keys/Tab now move button selection in
    every world (removed arrow-panning).
  - Ran on the **alternate screen buffer**; **per-frame image dedup** (FNV-1a hash
    of the rendered image; skip re-emitting when unchanged). See memory section.
- `fc9d27e` Persist CLI-overridden settings every run; misc polish (signal-path
  cursor restore; `da_has_sixel` const cast; `GOL_SIXEL` exact match; gitignore nl).

## THE OPEN BUG: iTerm2 memory blows up (NOT solved)

### Environment / symptom
- User's machine: **MacBook Air M5, 16 GB, iTerm2**.
- Running the game (esp. an active simulation, and dragging at max zoom-out) makes
  **iTerm2's** memory grow without bound — user drove it to ~50 GB and triggered a
  macOS memory-pressure warning. Memory is only released when the **terminal**
  closes. At 1px zoom, fast dragging goes laggy then the program **crashes/exits
  instantly** (almost certainly iTerm2 being OOM-killed and taking our pty down).

### Root cause (established)
This is an **iTerm2 bug, not ours.** Older iTerm2 **retains the decoded bitmap of
essentially every sixel image it displays and never frees it.** 50 GB ≈ thousands
of full-screen bitmaps. It is NOT a heap leak in our process (the OS reclaims our
memory instantly on exit; a leak couldn't grow across runs). Documented upstream:
GitLab iTerm2 issues #10420 (inline-image leak), #9070 (sixel CPU/mem). **iTerm2
fixed image memory leaks in 3.7.0beta1 (Apr 2026)** — so the real fix for the user
is updating iTerm2, and/or using a terminal without this bug (Konsole/WezTerm/foot).

### What was tried, in order, and the result
1. **Avoid a bottom-row newline** (`CLR_EOL` = `\033[K` with no `\n` on the last
   line) so the frame doesn't scroll → less into scrollback. *Helped as hygiene,
   NOT sufficient.*
2. **Clear scrollback** (`\033[3J`) per-frame and on exit. *User confirmed setting
   iTerm2 scrollback to 0 fixed it, but `\033[3J` did NOT — iTerm2 doesn't release
   image memory on `\033[3J`, only on scrollback eviction / teardown. Reverted.*
3. **Alternate screen buffer** (`\033[?1049h/l`, like vim/less). Has no scrollback,
   so scrolled images are discarded. *Still grew — iTerm2 retains images on the alt
   screen too. Kept anyway (correct TUI hygiene; restores the user's screen on exit).*
4. **Per-frame image dedup** (FNV-1a hash; only re-emit the sixel when it actually
   changed). *Real win: idle/paused/navigating/stabilised frames emit zero images,
   so those cases stop feeding the leak. But a running/animating sim still emits one
   image per generation and still grows on buggy iTerm2 — "slower, still fills".*
5. **Input coalescing** (drain input, render once). *Cuts images emitted during a
   fast drag by 10–50×; fixes our own drag-lag backlog. Reduces pressure on the
   leak but can't cure a terminal that never frees images.*

### Status
**Not solved on affected iTerm2 versions**, because it's fundamentally iTerm2's
image-retention bug — no escape sequence makes it release the memory. Our
mitigations (alt screen, dedup, coalescing) bound the idle/interactive cases and
slow the rest, but an active simulation on a leaky iTerm2 will still grow.

**CONFIRMED iTerm2-specific (2026-07-06, Fedora + Konsole).** Verified on the
Linux box: (1) an automated PTY check sampling our own process RSS during 18s of
active simulation showed it flat at 3304 KB — **zero growth; our process does not
leak**. (2) The user ran `--infinite` in Konsole watching the Konsole process
memory: **stable across active sim + long drags**, and drag-pan + wheel-zoom are
smooth/responsive, no crash. So the blow-up is purely iTerm2's image retention;
Konsole/WezTerm/foot are fine. Real fix for the iTerm2 user = update to ≥3.7.0beta1.

See also the memory note: `.claude/.../memory/sixel-scrollback-retention.md`.

## Next steps

The infinite world is now highly usable (direct sparse render, mouse pan+zoom,
verified stable on Konsole). Below is the current prioritised backlog, agreed
across both machines. Grouped by value/effort. Nothing here is started yet unless
marked DONE. When you pick one up, update this list.

### Done / standing advice
- ~~Verify Fedora + Konsole~~ **DONE (2026-07-06)** — see "THE OPEN BUG" note.
- ~~Direct sparse render (old perf item #4)~~ **DONE (2026-07-06)** — see changes list.
- ~~Tier 1 · Recenter / follow~~ **DONE (2026-07-06)** — `c` centres, `f` follows.
- ~~Tier 2 · Collapse to a pure infinite sandbox~~ **DONE (2026-07-06)** — Finite/
  Toroidal + Canvas removed; see changes list.
- **Advice to the iTerm2 user (not code):** update iTerm2 to ≥ 3.7.0beta1 — the
  actual fix for the image-retention memory blow-up.

### Tier 1 — quick wins (next up)
1. **Runtime speed control.** `delay_ms` is currently fixed at launch (`-d` only).
   Add `+`/`-` (or `[`/`]`) to change it live, with the current speed on the status
   line. Small code; essential for actually watching evolution. **Recommended next.**

### Tier 2 — nice to have
2. **Large-pattern save/load (RLE).** Real sandbox persistence: load the community
   -standard `.rle` format (guns, spaceships — `.cells` is inefficient for big
   patterns) and save the current world back out. High value, **largest effort**
   (new parser + writer).
3. **Clear-to-empty action.** One key to blank the world and draw from scratch
   (`sparse_clear` already exists). Tiny.
4. **Keyboard zoom (`+`/`-`).** Zoom without a mouse. Tiny. (Mind the collision if
   `+`/`-` is taken by speed control in Tier-1 item 1 — pick distinct keys.)

### Tier 3 — perf, only if it actually bites
5. **Encode sixel directly from the live-cell set.** Removes the residual O(pixels)
   encode cost at huge screen / 1px zoom (~1.7 ms/frame today). Only worth doing if
   big-screen encode ever becomes the bottleneck; the direct-render change already
   killed the O(viewport) hash-lookup cost that was the real problem.

## Gotchas / constraints for future work

- **Keep the build warning-clean** under `-Wall -Wextra -Wpedantic`.
- **One world only (unbounded/sparse).** Don't reintroduce Finite/Toroidal, a
  `WorldType`, or Canvas mode — the product is the infinite sandbox. `Board` is a
  transient seed buffer, not the sim state.
- **Sixel is mandatory**: don't reintroduce a text renderer or assume a char grid.
- **Alt screen**: on exit, do NOT clear the user's main screen/scrollback yourself
  — leaving the alt screen (`terminal_restore`) restores it. Don't reintroduce
  `\033[2J/3J` on exit.
- **Don't fight the iTerm2 leak with scrollback tricks** — proven useless. The only
  real levers are: emit fewer images (dedup/coalescing, done) or a fixed iTerm2.
- **`ws_xpixel/ypixel`** (terminal pixel size) can be 0 / unavailable on some
  terminals; all sixel-layout code must handle that (it does — falls back).
- Mouse SGR: wheel = button 64 (up) / 65 (down); left = 0; motion bit = 0x20;
  final char `M` = press/drag, `m` = release. Parsed in `terminal.c`.
