# CLAUDE.md — Conway's Game of Life (TUI)

Working notes for Claude Code. Read this first when picking up work on this repo,
especially from another machine. The Git repo is the single source of truth:
`git pull`, then read this file + `git log`.

## What this project is

An interactive Conway's Game of Life for the terminal, written in C11, built with
CMake. **Rendering uses real-pixel graphics** (no text/character fallback) via
the **Kitty Graphics Protocol** (preferred, auto-detected) with a **sixel
fallback**. Requires a KGP-capable terminal (Kitty, Ghostty, WezTerm, Konsole
≥ 24.08) or a sixel-capable terminal (iTerm2, Konsole, WezTerm, foot,
`xterm -ti vt340`). The default macOS Terminal.app is NOT supported and the
program exits with a message on startup if neither protocol is available.

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

- `src/engine.{c,h}` — **the engine seam.** Opaque `LifeEngine` + `EngineSnapshot`
  wrapping the sparse backend today; this is the interface a future Hashlife
  backend implements. The two "ENGINE SEAM" ops are `engine_advance(n)` (Hashlife
  would leap in ~O(log n)) and `engine_snapshot`/`engine_restore` (Hashlife would
  store a canonical quadtree node id). main.c/history.c talk only to `engine_*`.
- `src/history.{c,h}` — bounded ring (HISTORY_CAP=1024) of `EngineSnapshot`s
  tagged by generation, for instant rewind. Direct-mapped (`gen % cap`), with
  branch-detection: recording a non-contiguous gen clears the ring (timeline
  changed). `history_floor(gen)` finds the nearest replay base.
- `src/popup.{c,h}` — a transient "toast": a `Popup` (text + monotonic stamp +
  active flag) that floats over the world for `POPUP_TTL_MS` (5s) then auto-hides.
  `popup_show`/`popup_visible`/`popup_expire`/`popup_remaining_ms`/`popup_clear`.
  Used for Save/Load results (main.c draws it bottom-right over the world image;
  the main loop wakes on `popup_remaining_ms` to hide an idle one).
- `src/rle.{c,h}` — read/write the community-standard RLE pattern format
  (`rle_load` → malloc'd (x,y) array; `rle_save` from an (x,y) array). Used by the
  `s`/`l` file prompt. Engine-independent.
- `src/sparse.{c,h}` — the current world engine behind `engine.c`: open-addressing
  hash set of live cells (SplitMix64 hash, backward-shift deletion). `sparse_step`
  dispatches to a serial reference (`sparse_step_serial`) or, for worlds ≥20000
  cells with OpenMP, a lock-free multi-core stepper (`sparse_step_parallel`,
  y-band partitioning — see the changes note); both give identical results.
  `sparse_query(x0,y0,x1,y1,fn)` iterates the live cells in a rectangle (render +
  snapshot).
- `src/sixel.{c,h}` — sixel bitmap encoder. Incremental `SixelCanvas`
  (`new/set_alive/set_cursor/encode/free`) plotted straight from the live-cell set;
  `sixel_render_board` is a thin Board wrapper on top. Palette: dead/alive/grid/cursor.
- `src/kitty.{c,h}` — Kitty Graphics Protocol encoder, parallel API to the sixel
  canvas (RGB pixel buffer → zlib-compress → base64 → KGP APC sequence). Auto-
  detected at startup; fallback to sixel when the terminal does not support KGP.
  Depends on zlib (system, `find_package(ZLIB)`).
- `src/board.{c,h}` — dense `Board`, now used only as a transient seed buffer to
  lay out the initial random/`.cells` pattern before handing it to the sparse
  world (`seed_from_board`). Not on the render/step hot path any more.
- `src/terminal.{c,h}` — raw mode, key + SGR-mouse decoding, sixel detection
  (Primary DA query), alt-screen + mouse enable/disable.
- `src/config.{c,h}` — load `.cells` plaintext patterns (centered into a board).
- `src/settings.{c,h}` — JSON-ish persisted settings in `$XDG_CONFIG_HOME/game-of-life/
  settings.json`, plus dir helpers: `settings_config_dir` (config),
  `settings_data_dir`/`settings_saves_dir` (user data — saved patterns), `settings_mkdirs`.
- `src/main.c` — UI state machine (Normal/Edit/Jump + Save/Load/Confirm/Help dialogs),
  render loop (world image + `render_dialog` for the full-screen browsers), input
  handling, mouse pan/zoom + clickable button bar and dialog rows, recenter/follow.

## Changes made this session (newest first, by commit)

- **(Fedora) Kill the white flash on forced repaints (synchronized output).**
  Every "clear the screen, then redraw" path used to hand the terminal the clear
  and the new frame as separate writes, so the blank background was *presented*
  between them — on wheel zoom (image size changes every notch ⇒ full `\033[2J`
  per notch) this strobed white, a photosensitivity hazard. All three such paths
  (`emit_frame`, `render_dialog`, `render_too_small`) are now bracketed in
  **DEC private mode 2026 synchronized output** (`SYNC_BEGIN`/`SYNC_END` =
  `\033[?2026h/l`): the terminal composites everything in the bracket and
  presents it as one frame, so the intermediate blank screen never reaches the
  display. Supported by Ghostty/Kitty/WezTerm/foot/Konsole/iTerm2; unknown-mode
  sequences are ignored elsewhere, and the spec's mandatory timeout means an
  unmatched BEGIN can't wedge a terminal. `render_dialog`'s clear moved after
  its malloc so the early-return path never leaves a frame open. PTY-verified:
  every `2J` sits inside a BSU..ESU bracket (zoom, dialog open, dialog close);
  steady running frames are bracketed with no clear.
- **(Fedora) Linear wheel-zoom + fixed 10 ms speed steps.** Two step-size tweaks:
  `ZOOM_STEP` 2→1 so each wheel notch moves the chunky zoom exactly 1 px/cell
  (20..1; the sub-pixel ladder still doubles per notch), and `adjust_speed` is now
  additive — each `+`/`-` press moves `delay_ms` by a fixed `SPEED_STEP_MS` (10 ms,
  clamped 0..2000; replaces the old ×0.7/×1.4 multiplicative nudge and its
  `SPEED_MIN_DELAY_MS` snap). An off-grid delay from the CLI/settings snaps onto
  the 10 ms grid on the first press. PTY-verified: wheel 10→9→8→7 px and back;
  `-`×3 takes Delay 120→150, `+`×4 takes it 150→110.
- **(Fedora) Start/Pause label toggle + Edit "Clear" button.** Two UI touches:
  - **BTN_PLAY now shows the *action*, not both names.** The button reads
    `Start` while stopped/paused and `Pause` while running (was the static
    `Play/Pause`). `Start`/`Pause` are both five letters, so the static
    `BUTTON_LABELS` slot is a fixed-width stand-in that still gives the bar its
    width and hit boxes; only the drawn text is swapped at render/flash time via
    `play_label`/`button_label`. No layout jitter on toggle.
  - **Edit mode gains a `[ Clear ]` button** (row is now `[ Apply ] [ Discard ]
    [ Clear ] [ Pan: on/off ]`; `edit_c0/c1` widened to 4). `x`/`X` does the same
    from the keyboard. Clear only `engine_clear`s the cells and *stays in Edit*,
    so the transaction still holds: Esc restores the pre-edit world (even after a
    clear), Enter commits the now-empty one. Distinct from Normal-mode `x`
    (`clear_world`, which also resets the restart config + timeline).
  PTY-verified: bar toggles Start↔Pause on `p`; Edit shows `[ Clear ]`; `x` and a
  click both take Live 5→0; Esc after clear restores Live 5.
- **(Fedora) Fix unbounded KGP memory growth + warning-clean dialogs.** Two fixes
  after pulling the KGP work:
  - **KGP no longer leaks images (was ~22 GB in Ghostty).** The encoder emitted
    `a=T` with *no image id* every frame, so the terminal allocated a fresh image
    + placement per generation and never freed them — unbounded growth on a
    running sim. `kitty_canvas_encode` now reuses a single fixed image/placement
    id (`KGP_IMAGE_ID`) and prepends a delete of the previous frame
    (`\033_Ga=d,d=i,i=1,q=2\033\\`) before the transmit, so the terminal replaces
    the image in place. `q=2` on both the delete and the transmit suppresses the
    per-frame `OK` acknowledgment an id would otherwise trigger (which would
    pollute stdin). Delete + transmit go out in one write, composited together
    with no flash. The dedup path (emit only when the image changed) still holds,
    so idle frames cost nothing. New test assertions lock in the delete / fixed
    id / `q=2` so the leak can't silently return.
  - **Dropped a dead loop** in `kitty_canvas_set_alive` (an empty `for` whose
    condition was never true) and **removed two `-Wformat-truncation` warnings**
    in `dlg_title`/`dlg_dim` by appending straight through `appendf` (which
    truncates safely on `cap - *n`) instead of a fixed-size `snprintf` temp.
  Build is warning-clean; `test-kitty` passes with the new assertions.
- **(Mac, Ghostty) Add Kitty Graphics Protocol (KGP) canvas with sixel fallback.**
  New `src/kitty.{c,h}` parallel to the sixel canvas: RGB pixel buffer → grid
  lines pre-drawn → live cells fill white cells → at encode-time cursor outline
  drawn temporarily, zlib-compressed (best), base64-encoded → KGP APC sequence
  `\033_Ga=T,f=24,s=W,v=H,o=z,z=-1,C=1;<b64>\033\\`. Auto-detected at startup
  via `terminal_query_kitty()` (sends `\033_Gi=1,a=q;\033\\`, looks for "OK" in
  the reply, accepts both `\033\\` and 8-bit `0x9c` terminators; `GOL_KITTY` env
  var overrides). Prefers KGP, falls back to sixel, exits with a message if
  neither is available. `render()` dispatches the canvas via `void *canvas` +
  function pointer; `render_dialog()` clears the screen before drawing so the
  box is opaque over the KGP layer. Overlay backgrounds use truecolor black
  (`\033[48;2;0;0;0;37m`) because KGP terminals may render ANSI palette black
  as partially transparent over the graphics layer. Unit-tested (`tests/test_kitty.c`);
  confirmed working on Ghostty (native KGP) and iTerm2 (sixel fallback).
  PTY-verified: KGP APC sequence format, zlib round-trip, sixel fallback still
  compiles and links.
- **(Mac, tui-design review) Mouse-only dialogs, Edit paint, Jump wheel/pan.**
  Three changes from a `tui-design` skill-guided review that made the secondary
  pages fully mouse-operable.
  - **Modal mouse exits — click outside or on a footer button to close.** Every
    modal (Save/Load/Confirm) now has a close path that does not require Esc:
    Save gained `[Save] [Cancel]` buttons; Load gained a `[Cancel]` button;
    click-outside-the-box cancels all three (Confirm treated as No). Help already
    dismissed on any click (fixed in a separate commit). The box geometry is
    recorded in `App.dlg_box_*` each dialog frame; `dlg_click_outside`/
    `dlg_action_hit` are shared helpers used by `handle_save_name`,
    `handle_dialog_mouse`, and `handle_confirm_mouse`.
  - **Edit mouse paint + Pan toggle.** A new `[Apply] [Discard] [Pan: on/off]`
    button row replaces the menu bar during UI_EDIT. Left-click toggles a cell;
    left-drag paints (value = opposite of the first cell, Bresenham-interpolated
    so fast strokes leave no gaps). The Pan toggle switches left-drag between
    paint and world-pan so patterns can be drawn beyond the visible area. The
    wheel always zooms. `apply_edit`/`discard_edit` are extracted and shared by
    the keyboard and button paths. `screen_cell_to_world` maps the pointer to
    world coordinates.
  - **Jump wheel-zoom + drag-pan.** `handle_jump` now accepts KEY_MOUSE: the
    wheel zooms and a left-drag pans the world behind the prompt, then clicking
    the Jump button executes and clicking any other bar button cancels and runs
    that action. Number entry stays on the keyboard.
  PTY-verified: single-click toggle on/off, drag paints a line, [Pan] toggles,
  [Apply] exits, click-outside closes Save/Load, Jump wheel zooms from 10px→6px.
- **(Mac, tui-design review) Rounded overlays, semantic colour, styled dialogs.**
  Aesthetic pass over the HUD and dialogs (the second commit of the review).
  - **Rounded Unicode borders** (`╭─╮ │ ╰─╯`) for every floating overlay — modal
    dialogs and the popup toast — replacing ASCII `+---+` and asterisks.
    `overlay_box` builds the multibyte horizontal run once and reuses it per row.
  - **Semantic colour, honouring NO_COLOR.** Top HUD: `State` coloured by
    run-state (green/yellow/dim), `Follow` accented when on, `Cam/Zoom/Delay`
    dimmed as secondary. `build_status` has a styled form whose *visible width*
    matches the plain form (SGR-emitted text strips to the same bytes), so
    centring is unaffected; the styled version is used only when the line fits
    without truncation. Under `NO_COLOR` the colour tokens are emptied but
    bold/dim attributes and the overlay background are kept.
  - **Dialog polish:** bold-cyan titles (`dlg_title`), dimmed secondary text
    (`dlg_dim` for folder paths, footer hints, placeholders, table header), Load
    sort arrow `▲/▼` on the active column with precise hit boxes, Load item
    count, Save/Help footer hints anchored to the box bottom.
  PTY-verified: `╭` present / `+---` gone, styled HUD strips to identical
  visible text, `NO_COLOR` suppresses colour but keeps bold+dim.
- **(Mac, tui-design review) Crash-safe terminal restore, responsive bar,
  too-small notice, Help click fix.** Four robustness fixes (the first commit
  of the review).
  - **Restore the terminal on a crash.** SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE
    now run `on_crash` (show cursor, leave alt screen, then `raise` with
    `SIG_DFL`) so a crash no longer strands the terminal in raw mode — `atexit`
    does not run for signal-killed processes. PTY verified: alt screen off,
    cursor shown, process still re-raises SIGSEGV with correct signal 11.
  - **Responsive button bar.** The 9-button bar (~123 cols) overflows 80–120-col
    terminals; `bar_labels(cols)` picks compact labels
    (`BUTTON_LABELS_SHORT`, ~69 cols) when the full set would not fit. Shared by
    the renderer, geometry/hit-test, and click-flash so they stay in agreement.
    The hint line truncates to width.
  - **Terminal-too-small notice.** `render()` silently skipped the frame when the
    grid was too small; it now shows a centred "Terminal too small (need NNxN)"
    message (the floor is `labels_bar_width(BUTTON_LABELS_SHORT)` ×
    `MIN_USABLE_ROWS`). PTY verified: notice shown at 40 cols.
  - **Help no longer closes on the opening click's release.** `handle_help`
    dismisses only on a real key or a fresh mouse press; the release and motion
    events of the opening click are ignored.
- **(Fedora) Keep the selected button lit in Edit/Jump + transactional Edit
  (Space/Enter/Esc).** Two fixes:
  - **Highlight no longer vanishes** when a click enters Edit or Jump. Those modes
    render the world (not a full-screen dialog), and `append_controls` only lit the
    selection in UI_NORMAL, so the button the cursor landed on went dark until you
    left. Now `show_sel` also covers UI_EDIT / UI_JUMP, and entering either mode
    pins `app->selected` to `BTN_EDIT` / `BTN_JUMP` (so the keyboard `e`/`j` paths
    match the mouse path and the highlight stays put after leaving too).
  - **Edit is now a transaction.** Space is the *only* edit action (toggle the cell
    under the cursor); **Enter = Apply** (keep the edits, they become the new
    restart config + fresh timeline, gen→0) and **Esc = Discard** (restore the
    world snapshot taken on entry). New `App.edit_backup`/`edit_backup_gen`
    (an `EngineSnapshot` captured in the `BTN_EDIT` case, restored by Esc, freed on
    Apply/Discard and at exit). Enter no longer toggles; Tab no longer leaves.
    Hint + Help text updated accordingly.
  PTY-verified: Edit/Jump stay highlighted on entry and after leaving; Space
  toggles (Live 5→6); Esc restores Live to 5; re-enter + Space + Enter keeps Live 6
  and resets Gen to 0.
- **(Fedora) Instant click feedback: highlight the clicked button before its action
  runs.** Clicking a menu button that opens a modal (Save/Load/Jump/Help) used to
  leave the bar showing the *old* selection until the modal closed, because
  `handle_mouse` set `app->selected` then immediately called `activate_button`, and
  the loop's next `render` drew the dialog without repainting the bar. New
  **`flash_selected_button(app)`** repaints the bar (current selection in reverse
  video) and flushes it *now*, called on a bar click just before `activate_button`.
  The bar sits below the dialog box, so the moved highlight stays visible for the
  whole modal. PTY-verified: clicking Save immediately shows `[ Save (S) ]`
  highlighted (and clears the prior Play highlight) while the Save dialog is open.
- **(Fedora) Button-bar overhaul: Play/Pause toggle, per-button shortcuts, Help +
  Quit buttons.** The bar is now **Play/Pause · Step · Reset · Edit · Jump · Save ·
  Load · Help · Quit** (9 buttons). Changes:
  - **Start + Pause merged** into one `BTN_PLAY` toggle (`activate_button_at`:
    RUNNING → PAUSED, anything else → RUNNING).
  - **Every label carries its shortcut** in parens (`" Play/Pause (P) "`,
    `" Step (N) "`, `" Reset (R) "`, `" Edit (E) "`, `" Jump (J) "`, `" Save (S) "`,
    `" Load (L) "`, `" Help (?) "`, `" Quit (Q) "`). Pressing that key from
    UI_NORMAL fires the same action — `handle_normal`'s KEY_OTHER now dispatches
    P/N/R/E/J/S/L/? through the shared **`activate_button_at(app,btn)`** (the menu
    click path and the keyboard path share it so they never drift). c/f/x and +/-
    remain the non-menu world keys.
  - **Bottom hint trimmed** to only non-menu ops + the menu-nav aids: "Drag pan
    Wheel zoom  +/- speed  c center  f follow  x clear  |  Tab select  Space
    activate" (Jump/Save/Load/Quit dropped — they're on the bar now).
  - **New `[ Help (?) ]` button + `UI_HELP` MODAL overlay** (a fourth `render_dialog`
    branch: a compact static controls list, capped to the box's `chh` rows).
    `handle_help` dismisses on any key/click (Ctrl-C still quits); reached only from
    UI_NORMAL, returns there and forces a world repaint to erase the box.
  - **`[ Quit (Q) ]` button back** (`BTN_QUIT` → `running=false`); mouse-only users
    no longer need to know 'q'. The old "there is no Quit button" comment is gone.
  PTY-verified: all 9 labels centred on the bar; trimmed hint; 'p' toggles
  RUNNING↔PAUSED; '?' opens Help (Controls text) and Space/any click closes it;
  Help-button and Quit-button mouse clicks work; 'q'/Quit exits.
- **(Fedora) Unify the overlays under one "popup" model + shared `overlay_box`.**
  Per the STICKY/TIMED/MODAL taxonomy (all three float over the world; they differ
  only in *when they close*): the status HUD is STICKY (redrawn each frame), the
  Save/Load toast is TIMED (`popup.{c,h}`, `POPUP_TTL_MS`), and the Save/Load/
  Confirm windows are MODAL (closed by a mode transition). They now share one
  rendering primitive, **`overlay_box(buf,cap,n, x0,y0,w,h, corner,horiz,vert)`**
  (main.c): draws an opaque, bg-filled box floating over the world with the given
  border chars (`corner==0` ⇒ borderless fill). The toast uses `'*','*','*'` (its
  asterisk notification box) and `render_dialog`'s frame uses `'+','-','|'`,
  replacing the hand-rolled `hbar`/`sp` border loop. The close-policy taxonomy is
  documented at the top of `popup.h`. PTY-verified identical output: Load list
  ASCII border (rows 7/44 + 36 sides) + entries, toast star box (rows 40/42) +
  text, Confirm ASCII border + Yes/No.
- **(Fedora) Floating HUD + auto-hiding Save/Load toast (`popup.{c,h}`).** The
  status line and the Save/Load result message used to sit in a controls block
  *below* the world; the message only cleared on the next keypress, so mouse-only
  use left it forever. Now both float **over** the world image (drawn on an opaque
  black cell bg so they read over the pixels):
  - **Status HUD** pinned to the world's **top-left** row, redrawn every frame
    (`build_status` → plain text; `emit_frame` overlays it at `\033[1;1H`). Width
    is grow-only (`App.hud_w`) so a shorter status never leaves a stale tail when
    the image is not repainted.
  - **Save/Load toast** floats at the world's **bottom-right** (`img_rows`, right-
    aligned) and **auto-hides after `POPUP_TTL_MS` (5s)** regardless of input. It
    is a new `Popup` type in `popup.{c,h}` (transient toast: text + monotonic
    stamp; `popup_show/visible/expire/remaining_ms/clear`). `write_current_to`,
    `do_load_path`, and the delete path call `popup_show` instead of `app->msg`
    (which now only carries the *inline* Save-dialog error). The main loop wakes on
    `popup_remaining_ms` when otherwise idle, and on expiry forces a world repaint
    (`sx_drawn=false`) to erase where the toast floated.
  - Below the image there is now just a blank spacer + button bar + hint, so the
    bar moved up one row: `compute_button_geometry` uses `img_rows + 1` and the
    click-swallow area is `>= bar_row - 1`.
  Follow-up polish (same session): the **HUD is centred** on the top row (start
  col from `(cols - barw)/2`); the **toast is an asterisk-bordered box** at the
  bottom-right (three rows: `*`-border / `* text *` / `*`-border, bottom edge on
  the image's last row) for a "notification" highlight; and the **button bar and
  hint are both centred**, drawn at absolute rows `img_rows+2` and `img_rows+4`
  with a blank row between them (`append_controls` now takes `img_rows, cols`,
  wipes the controls area with `ANSI_CLR_BELOW`, then places the two centred rows;
  `button_bar_width()` is shared by the renderer and `compute_button_geometry` so
  clicks still land).
  PTY-verified: HUD centred at row 1 tracks STOPPED→RUNNING and Gen; toast box
  lands bottom-right, survives a keypress, auto-hides after 5s with one erase
  repaint; centred bar + hint (blank row between) with buttons still clickable
  (Start→RUNNING, Pause→PAUSED).
- **(Fedora) Confirm dialog Yes/No are now selectable, clickable buttons.** The
  `UI_CONFIRM` modal used to show a bare `y = yes   n / Esc = no` hint. It now
  renders two `[ Yes ]` / `[ No ]` buttons (reverse-video highlight on the
  selected one, like the main menu bar), driven by `App.confirm_sel` (0=Yes,
  1=No; **defaults to No** since every confirm guards a destructive/overwriting
  action). Tab toggles, Left→Yes / Right→No, Enter/Space activates the highlight,
  `y`/`n` remain shortcuts, Esc = No. `render_dialog` records the two hit boxes
  (`dlg_confirm_row`, `dlg_yes_c0/c1`, `dlg_no_c0/c1`); `handle_confirm_mouse`
  hit-tests a left click. PTY-verified: highlight toggles via Tab/arrows, Enter on
  Yes overwrites (mtime changes) + dialog closes, click-No cancels back to the
  Save-name dialog, click-Yes overwrites.
- **(Fedora) Clickable buttons + a real Save/Load browser; saves split from
  config.** Usability pass:
  - **Buttons are mouse-clickable.** `emit_frame` records the bar row and each
    button's column span (`compute_button_geometry`, 0-based to match decoded SGR
    mouse); `handle_mouse` hit-tests a left click on the bar → select+activate.
    Clicks in the controls area never pan. Added **Save** and **Load** buttons
    (bar is now Start Pause Step Reset Edit Jump Save Load).
  - **Save/Load dialogs** replace the old blind filename prompt (`UI_FILE` → three
    modes `UI_SAVE_NAME`, `UI_LOAD_LIST`, `UI_CONFIRM`). `render_dialog()` draws a
    **centred modal window** — a black ASCII-bordered box floating over the world
    (not a full-screen takeover): it overlays only the box cells (the world shows
    around it), records mouse hit boxes in the box's screen coordinates, and the
    covered image is force-redrawn on return (`sx_drawn=false`, so the next
    `emit_frame` clears + repaints). `dlg_row()` places content lines inside.
    *Save*: type a name (sanitised: no empty/leading-'.'/'/'), `.rle` auto-appended,
    overwrite confirm. *Load*: a scrollable, sortable list (columns Name/Size/
    Modified; `n`/`s`/`m` or click the header to sort, again reverses; default
    Modified-desc), Enter/click a row loads, `d` deletes (confirm), `/` or the
    `[Type a path…]` line loads an arbitrary path (so external files like
    ~/Downloads/foo.rle still work), replace-world confirm when the world is
    non-empty. All rows/headers mouse-clickable (shared hit-test). Dir shown in
    the header. Keyboard `s`/`l` still open them.
  - **Saves live in `$XDG_DATA_HOME/game-of-life/saves/`** (data), separate from
    settings in `$XDG_CONFIG_HOME` (config). The startup default moved there as
    **`default.rle`** (loaded via `load_rle_file`; `build_initial` no longer reads
    `default.cells`). New `settings_data_dir`/`settings_saves_dir`/`settings_mkdirs`.
    CMake installs **all bundled patterns** (`saves/*.rle`) into the saves dir so
    they show up in the Load browser (never clobbering an existing file); uninstall
    removes exactly the shipped set (baked-in name list) + legacy `default.cells`
    but never the user's own saves. PTY-verified: save→file on disk, load list +
    sort + arrow-nav + delete + overwrite/replace confirms + mouse clicks on
    buttons, sort headers, type-path, and rows.
- **(Fedora) Responsive quit during a long jump + multi-core stepping.** For big
  worlds (a 250k-cell Turing machine jumping thousands of gens took 10+ min and
  wouldn't quit):
  1. **Quit stays responsive.** `run_forward` was polling only once per
     JUMP_CHUNK=256 generations; on a 250k-cell world a chunk is ~30s, so q/Ctrl-C
     went unheard that long. It now services input+redraw on a **wall-clock
     interval** (`JUMP_SERVICE_MS=40`, checked via a cheap `CLOCK_MONOTONIC` read
     each generation — cheap at both extremes, vs a `read()` syscall per gen which
     would cripple a fast glider jump). During a jump **q / Ctrl-C quit the whole
     program immediately** (highest priority; input drained before drawing), while
     **Esc** aborts only the jump. PTY-verified: q exits in ~0.12s mid-jump on a
     1.5M-cell world; Esc leaves the app running.
  2. **Multi-core `sparse_step` (OpenMP).** For worlds ≥ `SPARSE_PARALLEL_MIN`
     (20000) cells the neighbour tally fans across cores. Design: split the world
     into horizontal **y-bands**; each thread owns disjoint *output* rows [b0,b1)
     and reads a one-row halo, so a cell's ±1-row neighbourhood is fully covered by
     one thread — **no shared writes, no locks, no merge** (each thread has a
     private count map + result set; `w->live` is read-only), and the union of the
     per-band results equals the serial output bit-for-bit. Output rows run
     [ymin-1, ymax+1] because a cell can be born one row beyond the extent. Live
     cells are compacted into a contiguous array first so threads scan `len`, not
     the (post-death, often much larger) table `cap`. CMake `find_package(OpenMP)`;
     without it, `#ifdef _OPENMP` compiles the serial path. Validated bit-identical
     to a naive dense oracle over 25 steps at T=1..32; ~2.6× at 8 threads (sparse
     hashing is memory-bandwidth-bound, so it plateaus — the real order-of-magnitude
     win for repetitive patterns is still Hashlife, Tier-1). Set `OMP_NUM_THREADS`
     to tune. `sparse_step` dispatches: serial reference (`sparse_step_serial`) for
     small worlds or if a parallel allocation fails (world left untouched).
- **(Fedora) Sub-pixel zoom + RLE `-f` straight to the sparse engine.** Two changes
  so patterns much larger than the screen display whole:
  1. **Sub-pixel zoom.** The old zoom floor was 1 screen-pixel per cell, so a
     pattern wider than the display could never be seen whole. Added
     `cells_per_px` to `App` (world cells packed into one screen pixel, OR-
     downsampled): the zoom ladder now runs `cell_px` 20..2..1 (chunky) then
     `cells_per_px` 1,2,4,…256 (sub-pixel). Wheel zoom (`zoom_step`/`zoom_infinite`)
     walks the whole ladder cursor-anchored; `render()` maps each live cell to
     `(x-cam)/cpp` on a canvas sized in screen pixels (`ccols=vw/cpp`); pan
     (`screen_delta_to_world`) and viewport (`infinite_viewport`) go through
     `screen_px_to_world`. Status shows `Zoom: Npx` (chunky) or `Zoom: 1px=Nc`
     (sub-pixel). `fit_view_to` now picks a sub-pixel level for oversized patterns
     instead of clamping at 1px.
  2. **RLE `-f` bypasses the dense seed board.** `-f *.rle` now loads straight into
     the sparse engine via the shared `load_rle_file()` (used by both startup `-f`
     and in-app `l`), so a pattern with a huge-but-sparse bounding box is only
     limited by live-cell count, not by a dense board sized to its bbox. `.cells`
     `-f`/default still use the dense seed board (`build_initial`→`config_load_file`,
     which now also **grows the board to fit** so big `.cells` like glider-gun's
     36-wide layout aren't clipped to the 30-wide seed region). Removed the old
     `load_pattern_file` board-grow shim.
  PTY-verified: a 8339×6299 / 1.51M-cell pattern (`-f`) loads `Live: 1509100` at
  `Zoom: 1px=8c`, fully visible; wheel ladder 10..1px..1px=32c reverses cleanly;
  glider/pulsar keep `Zoom: 10px`; glider-gun.cells now `Live: 36` (was 28).
- **(Fedora) Large patterns load fully + zoom-to-fit.** (superseded in part by the
  entry above; `fit_view_to`/`load_rle_file` now also do sub-pixel.) Two follow-up
  bugs on big patterns: `-f` clipped into the seed board; both paths opened at the
  default 10px zoom. See that entry for the current design.
- **(Fedora) RLE save/load, clear, runtime speed.** `s`/`l` open a filename prompt
  (UI_FILE) to save/load community-standard **RLE** (`rle.{c,h}`; the format Golly/
  LifeWiki use). Load replaces the world, centres it, snapshots restart. `x` clears
  to a blank slate. `+`/`-` nudge `delay_ms` (0..2000ms, shown on the status line).
  terminal.c now records the raw byte for **every** key so filenames can contain
  'q' (only Ctrl-C quits in text entry). rle.c unit-tested (canonical glider parse,
  round-trip, empty); PTY-tested in-app (save→clear→load, q-in-filename).
  `patterns/` then shipped every pattern in **both** `.cells` and `.rle` (later
  superseded: the `.cells` copies were dropped and the `.rle` files moved to
  `saves/`, installed into the user's saves dir). Startup `-f`/default dispatch on extension via
  `load_pattern_file()` in main.c: `.rle` → rle.c (centred into the seed board like
  a `.cells` pattern), anything else → config.c. So `-f foo.rle` and `-f foo.cells`
  seed identically (verified equal for blinker/glider/glider-gun/pulsar/lwss/acorn).
  Before this, `-f *.rle` was fed to the `.cells` plaintext parser and rendered
  garbage (the RLE header + `b`/`o` tokens became "live" cells).
- **(Fedora) Large patterns load fully + zoom-to-fit.** Two follow-up bugs on big
  patterns: (1) `-f` laid the pattern into the fixed `-w/-h` seed board (default
  30×20) and **clipped** it — `load_pattern_file` now grows the board to the
  pattern's extent so nothing is lost (also fixes glider-gun, 36 wide, clipping at
  30); (2) both load paths opened at the default 10px zoom, so an 833-wide pattern
  drew 8330px and you saw only a fragment. New `fit_view_to(app,cx,cy,w,h)` picks a
  zoom so the whole bbox fits the terminal's pixel area, but **only ever zooms out**
  from `INFINITE_CELL_PX` (small patterns keep the chunky default; big ones go down
  to the 1px floor) and centres on the pattern. Wired into `seed_from_board` (`-f`/
  default, via `engine_bounds`) and `do_load` (in-app `l`). PTY-verified: a 833×629
  / 15091-cell pattern loads `Live: 15091` at `Zoom: 1px`; glider/pulsar/glider-gun
  stay at `Zoom: 10px`.
- **(Fedora) Jump: rewind + fast-forward, with an engine seam for Hashlife.**
  New `LifeEngine`/`EngineSnapshot` abstraction (`engine.{c,h}`) and a bounded
  history ring (`history.{c,h}`). `j` / the **Jump** button opens a prompt: type
  an absolute generation and leap there. Backward = restore the nearest retained
  snapshot (instant) or replay from the gen-0 restart config; forward = advance in
  interruptible JUMP_CHUNK=256 chunks (Esc/q aborts, progress shown, follow honored)
  so a long jump never freezes the UI. Life is irreversible, so rewind never
  computes a predecessor — it recalls or re-derives one. `sparse_initial` (a whole
  world) became `restart` (an `EngineSnapshot`); Reset/edit-commit reset the ring.
  Unit-tested (engine determinism, glider displacement, snapshot==recompute,
  ring evict/floor/branch-clear); PTY-tested (fwd 40/2000, instant back 8, back 0).
  Buttons: Start Pause Step Reset Edit **Jump** (6).
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
- ~~Jump (rewind + fast-forward)~~ **DONE (2026-07-06)** — history ring + chunked
  interruptible fast-forward, on a `LifeEngine` seam ready for Hashlife. See changes.
- ~~Runtime speed control (`+`/`-`)~~ **DONE (2026-07-06)**.
- ~~Clear-to-empty (`x`)~~ **DONE (2026-07-06)**.
- ~~RLE save/load (`s`/`l`)~~ **DONE (2026-07-06)** — `rle.{c,h}`.
- **Advice to the iTerm2 user (not code):** update iTerm2 to ≥ 3.7.0beta1 — the
  actual fix for the image-retention memory blow-up.

### Tier 1 — quick wins (next up)
1. **Hashlife backend.** The `engine.{c,h}` seam is in place: implement a hashed
   -quadtree engine behind `LifeEngine` so `engine_advance` can leap over huge
   generation counts (guns/breeders) that the sparse stepper handles only in
   O(N²). `EngineSnapshot` would become a canonical node id (O(1) to keep, making
   the history ring nearly free). Big, self-contained; do when far-forward Jump on
   growing patterns starts to hurt.

### Tier 2 — nice to have
2. **Keyboard zoom.** Zoom without a mouse. Tiny — but `+`/`-` are now the speed
   keys, so use different keys (e.g. `,`/`.` or `<`/`>`).
3. **A few bundled `.rle` patterns / a load menu.** Save/load exists; shipping a
   couple of famous `.rle`s (gun, breeder) or a pick-list would make it discoverable.

### Tier 3 — perf, only if it actually bites
4. **Encode sixel directly from the live-cell set.** Removes the residual O(pixels)
   encode cost at huge screen / 1px zoom (~1.7 ms/frame today). Only worth doing if
   big-screen encode ever becomes the bottleneck; the direct-render change already
   killed the O(viewport) hash-lookup cost that was the real problem.

## Gotchas / constraints for future work

- **Keep the build warning-clean** under `-Wall -Wextra -Wpedantic`.
- **The parallel `sparse_step` MUST stay bit-identical to the serial one.** It is a
  performance optimisation, never a semantic change. Its correctness rests on the
  y-band invariant (a thread owns disjoint output rows [b0,b1) and tallies input
  rows [b0-1, b1], so its neighbour counts are complete and no two threads write
  the same cell) and on output rows covering [ymin-1, ymax+1] (births one row past
  the extent). If you touch it, re-run the dense-oracle parity test across several
  `OMP_NUM_THREADS` values. OpenMP is optional (`find_package(OpenMP)`; guarded by
  `#ifdef _OPENMP`) — the serial path must always compile and be the fallback.
- **A long jump must service input on a wall-clock interval** (`JUMP_SERVICE_MS`),
  not every N generations — one generation of a huge world can take ~0.1s, so a
  fixed chunk makes quit unresponsive; conversely a `read()` per generation
  cripples a fast small-pattern jump. During a jump, q/Ctrl-C quit the program
  (drain input before drawing so quit wins), Esc aborts only the jump.
- **One world only (unbounded/sparse).** Don't reintroduce Finite/Toroidal, a
  `WorldType`, or Canvas mode — the product is the infinite sandbox. `Board` is a
  transient seed buffer, not the sim state.
- **`terminal_char()` now returns the raw byte for every key** (set before the
  switch in terminal.c), so text-entry modes (UI_FILE) can read 'q' etc. even
  though it also maps to `KEY_QUIT`. In text entry, distinguish quit by the byte
  (`0x03` = Ctrl-C) rather than by `KEY_QUIT`.
- **Go through the engine seam.** main.c/history.c call `engine_*`, not `sparse_*`
  — keep it that way so a Hashlife backend can drop in behind `LifeEngine`. Rewind
  never reverse-computes (Life is irreversible): it recalls a snapshot or replays
  from gen 0. A jump forward MUST stay chunked+interruptible (growing patterns are
  O(N²) in the sparse engine and would otherwise freeze the UI).
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
