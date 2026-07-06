#define _DEFAULT_SOURCE /* sigaction, SIGWINCH */

#include "board.h"
#include "config.h"
#include "engine.h"
#include "history.h"
#include "settings.h"
#include "sixel.h"
#include "terminal.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ANSI escape helpers. */
#define ANSI_CLEAR       "\033[2J"
#define ANSI_HOME        "\033[H"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_REVERSE     "\033[7m"
#define ANSI_RESET       "\033[0m"
#define ANSI_CLR_BELOW   "\033[J"  /* erase from cursor to end of screen */

/* End-of-line: erase to the end of the line, then break. Ensures a shorter new
   line fully overwrites a longer previous one (no leftover characters). */
#define EOL              "\033[K\n"

/* Like EOL but without the newline: used for the very last line of a frame so we
   never emit a '\n' on the bottom row, which would scroll the screen (and, in
   sixel mode, push the image into scrollback where it is retained). */
#define CLR_EOL          "\033[K"

#define MIN_DIM 3
/* Absolute safety caps for the initial seed region (a random/imported pattern
   is laid out in this box before it is handed to the unbounded world). The
   world itself is unbounded thereafter. */
#define HARD_MAX_W 1000
#define HARD_MAX_H 1000
#define BLINK_MS 400
#define MAX_DELAY_MS 3600000 /* 1 hour; keeps the poll timeout within int */

/* Sixel (real-pixel) rendering. The board is drawn as a bitmap instead of text
   cells, so its size is limited by pixels rather than by the character grid. */
#define SIXEL_UI_ROWS 6   /* char rows reserved below the image for the UI */
#define SIXEL_CELL_MAX 20 /* max pixels per cell (keeps small views sane) */

/* Pixels per cell for the viewport in sixel mode. The viewport is sized so cells
   render around this size; the mouse wheel changes it to zoom (smaller = more
   cells visible). INFINITE_CELL_PX is the default. */
#define INFINITE_CELL_PX 10
#define INFINITE_CELL_MIN 1  /* 1 device pixel per cell — the real-pixel floor */
#define INFINITE_CELL_MAX SIXEL_CELL_MAX
#define ZOOM_STEP 2 /* pixels per wheel notch */

/* Jump / rewind. The history ring keeps the last HISTORY_CAP generations so a
   recent rewind is instant; a jump forward (or a deep rewind that must replay
   from generation 0) advances JUMP_CHUNK generations at a time, checking for a
   cancel key between chunks so a long jump never freezes the UI. */
#define HISTORY_CAP 1024
#define JUMP_CHUNK 256
#define JUMP_FIELD_MAX 1000000000L /* cap the typed target to keep it sane */

/* Top-level interaction mode. */
typedef enum { UI_NORMAL, UI_EDIT, UI_JUMP } UiMode;

/* Simulation sub-state, meaningful in UI_NORMAL. */
typedef enum { SIM_STOPPED, SIM_RUNNING, SIM_PAUSED } SimState;

/* Bottom button bar. Order matters: it is the left-to-right layout. There is no
   Quit button — 'q' (or Ctrl-C) quits from any screen. */
typedef enum {
    BTN_START, BTN_PAUSE, BTN_STEP, BTN_RESET, BTN_EDIT, BTN_JUMP,
    BTN_COUNT
} Button;

static const char *const BUTTON_LABELS[BTN_COUNT] = {
    " Start ", " Pause ", " Step ", " Reset ", " Edit ", " Jump "};

typedef struct {
    UiMode mode;
    SimState sim;
    long gen;
    int selected;               /* button index (UI_NORMAL) */
    int cursor_x, cursor_y;     /* world cursor (UI_EDIT) */
    bool blink_on;              /* cursor blink phase (UI_EDIT) */

    int sx_last_w, sx_last_h;   /* last sixel image pixel size (clear on change) */
    uint64_t sx_last_hash;      /* hash of the last emitted image bytes */
    bool sx_drawn;              /* an image has been emitted at least once */

    /* The unbounded world: a Life engine (sparse today) with a pannable,
       zoomable viewport rendered straight from its live cells. */
    LifeEngine *engine;         /* the world */
    EngineSnapshot *restart;    /* generation-0 config restored by Reset */
    History *history;           /* recent generations, for instant rewind */
    int cam_x, cam_y;           /* viewport top-left in world coordinates */
    int view_w, view_h;         /* current viewport size in cells */
    int infinite_cell_px;       /* pixels per cell (mouse-wheel zoom level) */
    bool follow;                /* auto-recentre the camera on the pattern */

    /* Jump (UI_JUMP): type a target generation to leap to (forward or back). */
    long jump_field;            /* the number being typed */
    bool jump_editing;          /* field typed into since it got focus */
    bool jumping;               /* a chunked jump is in progress (progress UI) */
    long jump_target;           /* its destination (for the progress readout) */

    /* Left-drag panning. While the left button is held, the camera is recomputed
       from the anchor captured on press so the grabbed point stays under the
       cursor. */
    bool dragging;
    int drag_mx, drag_my;       /* mouse cell where the drag started */
    int drag_cam_x, drag_cam_y; /* camera position when the drag started */

    long delay_ms;
    bool running;
} App;

static const char *sim_label(SimState s) {
    switch (s) {
        case SIM_RUNNING: return "RUNNING";
        case SIM_PAUSED:  return "PAUSED ";
        default:          return "STOPPED";
    }
}

/* Set by the SIGWINCH handler; the main loop notices it and redraws (the
   viewport is recomputed every frame, so nothing else is needed). */
static volatile sig_atomic_t g_resized = 0;

static void on_winch(int sig) {
    (void)sig;
    g_resized = 1;
}

/* Restore the terminal and exit on a terminating signal. Re-show the cursor
   (terminal_restore leaves the alternate screen and restores termios, but does
   not touch the cursor). write() is async-signal-safe. */
static void on_term(int sig) {
    ssize_t r = write(STDOUT_FILENO, ANSI_SHOW_CURSOR, sizeof(ANSI_SHOW_CURSOR) - 1);
    (void)r;
    terminal_restore();
    _Exit(128 + sig);
}

/* Clamp a dimension into the supported range. */
static int clamp_dim(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------ */
/* Command-line options                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int width;
    int height;
    long delay_ms;
    double density;
    unsigned int seed;
    bool seed_set;
    const char *config_path;
} Options;

static void print_usage(const char *prog) {
    printf("Conway's Game of Life — unbounded sandbox (interactive)\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -w, --width N     seed region width in cells   (default 30)\n");
    printf("  -h, --height N    seed region height in cells  (default 20)\n");
    printf("                    (the box a random/loaded pattern starts in; the\n");
    printf("                     world itself is unbounded)\n");
    printf("  -d, --delay MS    delay between frames in ms (default 120)\n");
    printf("  -p, --density F   random initial density 0..1 (default 0.25)\n");
    printf("  -s, --seed N      random seed (default: time-based)\n");
    printf("  -f, --file PATH   load initial config from a pattern file\n");
    printf("                    (default: ~/.config/game-of-life/default.cells if\n");
    printf("                     present, otherwise a random board)\n");
    printf("      --help        show this help and exit\n\n");
    printf("Settings such as the seed-region size are remembered between runs in\n");
    printf("~/.config/game-of-life/settings.json.\n\n");
    printf("Buttons: Start Pause Step Reset Edit Jump   (q quits from anywhere)\n");
    printf("Normal:  drag pan   wheel zoom   c center   f follow   j jump\n");
    printf("         Tab select   Space/Enter activate   q quit\n");
    printf("Edit:    arrows move cursor   Space toggle cell   Tab/Esc leave\n");
    printf("Jump:    type a generation (forward or back)   Enter jump   Esc cancel\n");
}

static bool parse_long(int argc, char **argv, int *i, long *out) {
    if (*i + 1 >= argc) return false;
    char *end = NULL;
    long v = strtol(argv[*i + 1], &end, 10);
    if (end == argv[*i + 1] || *end != '\0') return false;
    *out = v;
    (*i)++;
    return true;
}

static bool parse_double(int argc, char **argv, int *i, double *out) {
    if (*i + 1 >= argc) return false;
    char *end = NULL;
    double v = strtod(argv[*i + 1], &end);
    if (end == argv[*i + 1] || *end != '\0') return false;
    *out = v;
    (*i)++;
    return true;
}

static int parse_args(int argc, char **argv, Options *opt) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        long lv;
        double dv;
        if (strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else if (strcmp(a, "-w") == 0 || strcmp(a, "--width") == 0) {
            if (!parse_long(argc, argv, &i, &lv) || lv < MIN_DIM || lv > HARD_MAX_W) goto bad;
            opt->width = (int)lv;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--height") == 0) {
            if (!parse_long(argc, argv, &i, &lv) || lv < MIN_DIM || lv > HARD_MAX_H) goto bad;
            opt->height = (int)lv;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--delay") == 0) {
            if (!parse_long(argc, argv, &i, &lv) || lv < 0 || lv > MAX_DELAY_MS) goto bad;
            opt->delay_ms = lv;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--density") == 0) {
            if (!parse_double(argc, argv, &i, &dv) || dv < 0.0 || dv > 1.0) goto bad;
            opt->density = dv;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--seed") == 0) {
            if (!parse_long(argc, argv, &i, &lv)) goto bad;
            opt->seed = (unsigned int)lv;
            opt->seed_set = true;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) {
            if (i + 1 >= argc) goto bad;
            opt->config_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\nTry '%s --help'.\n", a, argv[0]);
            return -1;
        }
        continue;
    bad:
        fprintf(stderr, "Invalid value for option: %s\n", a);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

static void appendf(char *buf, size_t cap, size_t *n, const char *fmt, ...) {
    if (*n >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (written > 0) {
        *n += (size_t)written;
        if (*n > cap) *n = cap;
    }
}

/* Size of the viewport in cells, chosen to fill the terminal at the current zoom
   (you pan to see beyond it). Mirrors the reservations made by the renderer so
   the image plus controls fit without scrolling. */
static void infinite_viewport(const App *app, int *vw, int *vh) {
    const int px = app->infinite_cell_px;
    int cols, rows, xpx, ypx;
    if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
        int cch = ypx / rows, ccw = xpx / cols;
        if (cch > 0 && ccw > 0) {
            int avail_w = xpx - ccw;
            int avail_h = ypx - SIXEL_UI_ROWS * cch;
            /* One cell per pixel is the finest the display can show, so the
               viewport cap is the terminal's own detected pixel size: vw fills
               the width at any zoom on any display (Retina, 4K, 8K, …) with no
               fixed limit. avail_w/avail_h come from the ioctl ws_xpixel/ypixel
               (u16-bounded); an over-large image simply fails to allocate and the
               frame is skipped rather than capping zoom. */
            *vw = clamp_dim(avail_w / px, MIN_DIM, avail_w);
            *vh = clamp_dim(avail_h / px, MIN_DIM, avail_h);
            return;
        }
    }
    *vw = 30; /* pixel size briefly unavailable (e.g. mid-resize) */
    *vh = 20;
}

/* Plot each live cell onto the sixel canvas, translating world coordinates to
   viewport-local cells. Passed to sparse_query so only the live population is
   touched, never every cell of the (possibly huge) viewport. */
typedef struct {
    SixelCanvas *canvas;
    int cam_x, cam_y;
} PlotCtx;

static void plot_cell_cb(int x, int y, void *ud) {
    PlotCtx *p = (PlotCtx *)ud;
    sixel_canvas_set_alive(p->canvas, x - p->cam_x, y - p->cam_y);
}

/* Append the controls block below the sixel image: status line, a blank line,
   the button bar, a blank line, and the hint line. */
static void append_controls(const App *app, char *buf, size_t cap, size_t *n) {
    if (app->jumping) {
        appendf(buf, cap, n,
                " JUMPING   Gen: %ld / %ld   Live: %zu   (Esc to stop)" EOL EOL,
                app->gen, app->jump_target, engine_population(app->engine));
    } else if (app->mode == UI_JUMP) {
        appendf(buf, cap, n, " JUMP   Go to generation: " ANSI_REVERSE "%ld" ANSI_RESET
                "   (now: %ld)" EOL EOL, app->jump_field, app->gen);
    } else if (app->mode == UI_EDIT) {
        appendf(buf, cap, n,
                " EDIT   Cursor: (%d,%d)   Live: %zu" EOL EOL,
                app->cursor_x, app->cursor_y, engine_population(app->engine));
    } else {
        appendf(buf, cap, n,
                " State: %s   Gen: %ld   Cam: (%d,%d)   Live: %zu   Zoom: %dpx   Follow: %s" EOL EOL,
                sim_label(app->sim), app->gen, app->cam_x, app->cam_y,
                engine_population(app->engine), app->infinite_cell_px,
                app->follow ? "on" : "off");
    }

    /* Button bar; selection is highlighted only in normal mode. */
    appendf(buf, cap, n, " ");
    for (int b = 0; b < BTN_COUNT; b++) {
        if (app->mode == UI_NORMAL && b == app->selected) {
            appendf(buf, cap, n, ANSI_REVERSE "[%s]" ANSI_RESET, BUTTON_LABELS[b]);
        } else {
            appendf(buf, cap, n, "[%s]", BUTTON_LABELS[b]);
        }
        appendf(buf, cap, n, " ");
    }
    appendf(buf, cap, n, EOL EOL);

    /* The hint is the last line of the frame, so it ends with CLR_EOL (no
       newline) to avoid scrolling the screen on the bottom row. */
    if (app->jumping) {
        appendf(buf, cap, n, " Jumping to generation %ld…   Esc to stop" CLR_EOL,
                app->jump_target);
    } else if (app->mode == UI_JUMP) {
        appendf(buf, cap, n,
                " Type a generation (forward or back)   Backspace delete   "
                "Enter jump   Esc cancel" CLR_EOL);
    } else if (app->mode == UI_EDIT) {
        appendf(buf, cap, n,
                " Arrows move cursor   Space/Enter toggle   Tab/Esc leave   q quit" CLR_EOL);
    } else {
        appendf(buf, cap, n,
                " Drag pan   Wheel zoom   c center   f follow   j jump   Tab select   Space/Enter activate   q quit" CLR_EOL);
    }
}

/* FNV-1a hash of a byte buffer, used to detect whether the rendered image is
   identical to the previous frame's (so we can skip re-emitting it). */
static uint64_t fnv1a(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Emit an already-encoded sixel image plus the controls below it. Re-emits the
   image only when it actually changed since the last frame (see the dedup note),
   so idle frames cost nothing and do not pile images into terminals that retain
   them (e.g. older iTerm2). Takes ownership of `img` and frees it. */
static void emit_frame(App *app, char *img, size_t img_len,
                       int img_w, int img_h, int cch) {
    const int img_rows = (img_h + cch - 1) / cch; /* char rows the image spans */

    const uint64_t hash = fnv1a(img, img_len);
    const bool image_changed = !app->sx_drawn || hash != app->sx_last_hash ||
                               img_w != app->sx_last_w || img_h != app->sx_last_h;
    if (image_changed) {
        /* Clear the screen only when the image size changed (avoids leaving stale
           pixels around a now-smaller image without flickering every frame). */
        if (img_w != app->sx_last_w || img_h != app->sx_last_h) {
            fputs(ANSI_CLEAR, stdout);
            app->sx_last_w = img_w;
            app->sx_last_h = img_h;
        }
        fputs(ANSI_HOME, stdout);
        fwrite(img, 1, img_len, stdout);
        app->sx_last_hash = hash;
        app->sx_drawn = true;
    }
    free(img);

    /* Controls, positioned just below the image. We run on the alternate screen
       buffer (no scrollback), so scrolled sixel images are discarded rather than
       retained — no per-frame scrollback clearing is needed here. */
    char ctl[2048];
    size_t n = 0;
    appendf(ctl, sizeof(ctl), &n, "\033[%d;1H", img_rows + 1);
    append_controls(app, ctl, sizeof(ctl), &n);
    appendf(ctl, sizeof(ctl), &n, ANSI_CLR_BELOW);
    fwrite(ctl, 1, n, stdout);

    fflush(stdout);
}

/* Render the world: build the image straight from the sparse live-cell set,
   plotting only the O(population) cells that fall inside the viewport instead of
   probing every one of its (up to millions of) cells. Returns false if the
   layout cannot be computed (no pixel size, allocation failed); the caller then
   simply skips the frame. */
static bool render(App *app) {
    int cols, rows, xpx, ypx;
    if (!terminal_size(&cols, &rows) || !terminal_pixel_size(&xpx, &ypx)) {
        return false;
    }
    const int cch = ypx / rows;
    const int ccw = xpx / cols;
    if (cch <= 0 || ccw <= 0) return false;

    int vw, vh;
    infinite_viewport(app, &vw, &vh);
    app->view_w = vw;
    app->view_h = vh;

    const int cell = app->infinite_cell_px < 1 ? 1 : app->infinite_cell_px;

    SixelCanvas *canvas = sixel_canvas_new(vw, vh, cell);
    if (canvas == NULL) return false;

    PlotCtx ctx = {canvas, app->cam_x, app->cam_y};
    engine_query(app->engine, app->cam_x, app->cam_y,
                 app->cam_x + vw, app->cam_y + vh, plot_cell_cb, &ctx);

    if (app->mode == UI_EDIT && app->blink_on) {
        sixel_canvas_set_cursor(canvas, app->cursor_x - app->cam_x,
                                app->cursor_y - app->cam_y);
    }

    size_t img_len = 0;
    char *img = sixel_canvas_encode(canvas, &img_len);
    sixel_canvas_free(canvas);
    if (img == NULL) return false;

    emit_frame(app, img, img_len, vw * cell, vh * cell, cch);
    return true;
}

/* ------------------------------------------------------------------ */
/* World helpers                                                      */
/* ------------------------------------------------------------------ */

/* Advance one generation and record it so the step can be rewound. */
static void step_once(App *app) {
    engine_advance(app->engine, 1);
    app->gen++;
    history_record(app->history, app->gen, engine_snapshot(app->engine));
}

/* Seed the world from a dense board (world coordinates match board coordinates),
   snapshot it as the restart config, and centre the viewport on it. */
static void seed_from_board(App *app, const Board *b) {
    engine_clear(app->engine);
    for (int y = 0; y < b->height; y++) {
        for (int x = 0; x < b->width; x++) {
            if (board_get(b, x, y)) engine_set(app->engine, x, y, true);
        }
    }
    engine_snapshot_free(app->restart);
    app->restart = engine_snapshot(app->engine);
    history_clear(app->history);
    app->gen = 0;

    int vw, vh;
    infinite_viewport(app, &vw, &vh);
    app->view_w = vw;
    app->view_h = vh;
    app->cam_x = b->width / 2 - vw / 2;
    app->cam_y = b->height / 2 - vh / 2;
}

/* Centre the viewport on the live cells' bounding box. No-op when the world is
   empty. Uses the viewport size from the last rendered frame, which is current
   except across a zoom/resize that has not been drawn yet. */
static void recenter_camera(App *app) {
    int minx, miny, maxx, maxy;
    if (!engine_bounds(app->engine, &minx, &miny, &maxx, &maxy)) return;
    const int mid_x = minx + (maxx - minx) / 2;
    const int mid_y = miny + (maxy - miny) / 2;
    app->cam_x = mid_x - app->view_w / 2;
    app->cam_y = mid_y - app->view_h / 2;
}

/* Enter the jump prompt (type a target generation). */
static void enter_jump(App *app) {
    app->mode = UI_JUMP;
    app->jump_field = app->gen;
    app->jump_editing = false;
}

/* Advance from the current generation to `target` (> current) in interruptible
   chunks, so a long fast-forward neither freezes the UI nor floods it. Records
   the trailing HISTORY_CAP generations so a later small rewind stays instant.
   Esc or q stops early, leaving the world at whatever generation it reached. */
static void run_forward(App *app, long target) {
    app->jumping = true;
    app->jump_target = target;
    while (app->gen < target && app->running) {
        long remaining = target - app->gen;
        long n = remaining < JUMP_CHUNK ? remaining : JUMP_CHUNK;
        for (long i = 0; i < n; i++) {
            engine_advance(app->engine, 1);
            app->gen++;
            if (target - app->gen < (long)HISTORY_CAP) {
                history_record(app->history, app->gen, engine_snapshot(app->engine));
            }
        }
        if (app->follow) recenter_camera(app);
        if (app->gen < target) {
            render(app); /* show the fast-forward animation + progress line */
            Key k = terminal_read_key(0);
            if (k == KEY_ESC || k == KEY_QUIT) break; /* abort the jump */
        }
    }
    app->jumping = false;
}

/* Jump to an absolute generation, forward or backward. A generation still in the
   history ring is restored instantly; otherwise restore the nearest earlier base
   (a ring snapshot, or the generation-0 restart config) and replay forward from
   there. Backward is possible only because Life is irreversible — we never
   compute a predecessor, we recall or re-derive it. */
static void jump_to(App *app, long target) {
    if (target < 0) target = 0;
    if (target == app->gen) return;

    /* Best base <= target with the fewest replayed steps: the current state (if
       not past target), the nearest retained snapshot, else generation 0. */
    long base_gen = 0;              /* the restart config is generation 0 */
    const EngineSnapshot *base = app->restart;
    bool from_current = false;

    long hg = 0;
    const EngineSnapshot *hs = history_floor(app->history, target, &hg);
    if (hs != NULL && hg >= base_gen) { base = hs; base_gen = hg; }
    if (app->gen <= target && app->gen >= base_gen) { from_current = true; base_gen = app->gen; }

    if (!from_current) {
        engine_restore(app->engine, base);
        app->gen = base_gen;
    }
    if (app->gen != target) run_forward(app, target);
    app->sim = SIM_PAUSED;
}

/* ------------------------------------------------------------------ */
/* Per-mode input handling                                            */
/* ------------------------------------------------------------------ */

static void activate_button(App *app) {
    switch (app->selected) {
        case BTN_START:
            if (app->sim != SIM_RUNNING) app->sim = SIM_RUNNING;
            break;
        case BTN_PAUSE:
            if (app->sim == SIM_RUNNING) app->sim = SIM_PAUSED;
            break;
        case BTN_STEP:
            step_once(app);
            if (app->follow) recenter_camera(app);
            app->sim = SIM_PAUSED;
            break;
        case BTN_RESET:
            /* Reload the initial configuration (generation 0). */
            app->sim = SIM_STOPPED;
            engine_restore(app->engine, app->restart);
            app->gen = 0;
            history_clear(app->history);
            break;
        case BTN_EDIT:
            app->mode = UI_EDIT;
            app->sim = SIM_STOPPED;
            app->blink_on = true;
            /* Start the cursor at the centre of the current view. */
            app->cursor_x = app->cam_x + app->view_w / 2;
            app->cursor_y = app->cam_y + app->view_h / 2;
            break;
        case BTN_JUMP:
            enter_jump(app);
            break;
    }
}

/* Convert a delta measured in screen character cells into a delta in world
   cells. The mouse reports character cells, while the renderer draws
   infinite_cell_px pixels per world cell, so scale through the pixel size of a
   character cell (character-cell pixels / world-cell pixels). */
static void screen_delta_to_world(const App *app, int dcol, int drow,
                                   int *wdx, int *wdy) {
    int cols, rows, xpx, ypx;
    if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
        int ccw = xpx / cols, cch = ypx / rows;
        if (ccw > 0 && cch > 0) {
            *wdx = (dcol * ccw) / app->infinite_cell_px;
            *wdy = (drow * cch) / app->infinite_cell_px;
            return;
        }
    }
    *wdx = dcol; /* pixel size unavailable: fall back to raw cell delta */
    *wdy = drow;
}

/* Mouse-wheel zoom, keeping the world point under the cursor fixed so zoom feels
   centred there. dir = +1 zooms in (larger cells), -1 zooms out. mx,my are the
   cursor's character-cell position. */
static void zoom_infinite(App *app, int dir, int mx, int my) {
    int old_px = app->infinite_cell_px;
    int new_px = clamp_dim(old_px + dir * ZOOM_STEP,
                           INFINITE_CELL_MIN, INFINITE_CELL_MAX);
    if (new_px == old_px) return;

    int cols, rows, xpx, ypx;
    if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
        int ccw = xpx / cols, cch = ypx / rows;
        if (ccw > 0 && cch > 0) {
            /* World cell currently under the cursor; keep it there afterwards. */
            int px_x = mx * ccw, px_y = my * cch;
            int world_x = app->cam_x + px_x / old_px;
            int world_y = app->cam_y + px_y / old_px;
            app->infinite_cell_px = new_px;
            app->cam_x = world_x - px_x / new_px;
            app->cam_y = world_y - px_y / new_px;
            return;
        }
    }
    app->infinite_cell_px = new_px; /* no pixel size: zoom without re-anchoring */
}

/* Handle a mouse event: the wheel zooms (anchored on the cursor), and the left
   button drags to pan (press captures an anchor; drag-motion recomputes the
   camera from it so the grabbed point tracks the cursor; release ends the drag). */
static void handle_mouse(App *app) {
    MouseEvent m = terminal_mouse();
    if (m.button == 64 || m.button == 65) { /* wheel up / down */
        if (m.pressed) zoom_infinite(app, m.button == 64 ? +1 : -1, m.x, m.y);
        return;
    }
    if (m.button == 0 && m.pressed && !m.motion) {
        app->dragging = true;
        app->drag_mx = m.x;
        app->drag_my = m.y;
        app->drag_cam_x = app->cam_x;
        app->drag_cam_y = app->cam_y;
    } else if (m.button == 0 && m.motion && app->dragging) {
        int wdx, wdy;
        screen_delta_to_world(app, m.x - app->drag_mx, m.y - app->drag_my,
                              &wdx, &wdy);
        app->cam_x = app->drag_cam_x - wdx;
        app->cam_y = app->drag_cam_y - wdy;
    } else if (!m.pressed) {
        app->dragging = false; /* button released */
    }
}

static void handle_normal(App *app, Key key) {
    /* The mouse pans (left-drag) and zooms (wheel); the arrow keys move the
       button selection. */
    if (key == KEY_MOUSE) {
        handle_mouse(app);
        return;
    }

    switch (key) {
        case KEY_NONE:
            if (app->sim == SIM_RUNNING) {
                step_once(app);
                if (app->follow) recenter_camera(app);
            }
            break;
        case KEY_TAB:
        case KEY_RIGHT:
            app->selected = (app->selected + 1) % BTN_COUNT;
            break;
        case KEY_LEFT:
            app->selected = (app->selected + BTN_COUNT - 1) % BTN_COUNT;
            break;
        case KEY_ENTER:
        case KEY_SPACE:
            activate_button(app);
            break;
        case KEY_OTHER:
            /* 'c' recentres on the pattern once; 'f' toggles a follow mode that
               recentres every generation. */
            {
                const int c = terminal_char();
                if (c == 'c' || c == 'C') {
                    recenter_camera(app);
                } else if (c == 'f' || c == 'F') {
                    app->follow = !app->follow;
                    if (app->follow) recenter_camera(app);
                } else if (c == 'j' || c == 'J') {
                    enter_jump(app);
                }
            }
            break;
        case KEY_QUIT:
            app->running = false;
            break;
        default:
            break;
    }
}

/* Pan the viewport so the edit cursor stays visible. */
static void follow_cursor(App *app) {
    if (app->cursor_x < app->cam_x) app->cam_x = app->cursor_x;
    else if (app->cursor_x >= app->cam_x + app->view_w)
        app->cam_x = app->cursor_x - app->view_w + 1;
    if (app->cursor_y < app->cam_y) app->cam_y = app->cursor_y;
    else if (app->cursor_y >= app->cam_y + app->view_h)
        app->cam_y = app->cursor_y - app->view_h + 1;
}

static void handle_edit(App *app, Key key) {
    switch (key) {
        case KEY_LEFT:  app->cursor_x--; break;
        case KEY_RIGHT: app->cursor_x++; break;
        case KEY_UP:    app->cursor_y--; break;
        case KEY_DOWN:  app->cursor_y++; break;
        case KEY_SPACE:
        case KEY_ENTER: {
            bool alive = engine_get(app->engine, app->cursor_x, app->cursor_y);
            engine_set(app->engine, app->cursor_x, app->cursor_y, !alive);
            break;
        }
        case KEY_TAB:
        case KEY_ESC:
            /* Leave edit mode; the edited world becomes the new restart config,
               and it starts a fresh timeline. */
            engine_snapshot_free(app->restart);
            app->restart = engine_snapshot(app->engine);
            history_clear(app->history);
            app->gen = 0;
            app->sim = SIM_STOPPED;
            app->mode = UI_NORMAL;
            break;
        case KEY_QUIT:
            app->running = false;
            break;
        default:
            break;
    }
    follow_cursor(app);
}

/* Type a target generation, then leap there (forward or backward). */
static void handle_jump(App *app, Key key) {
    switch (key) {
        case KEY_BACKSPACE:
            app->jump_field /= 10;
            app->jump_editing = true;
            break;
        case KEY_OTHER: {
            const int c = terminal_char();
            if (c >= '0' && c <= '9') {
                long d = c - '0';
                /* First digit after focusing replaces the shown value. */
                long v = app->jump_editing ? app->jump_field * 10 + d : d;
                app->jump_field = v > JUMP_FIELD_MAX ? JUMP_FIELD_MAX : v;
                app->jump_editing = true;
            }
            break;
        }
        case KEY_ENTER: {
            long target = app->jump_field;
            app->mode = UI_NORMAL;
            jump_to(app, target);
            break;
        }
        case KEY_ESC:
            app->mode = UI_NORMAL; /* cancel */
            break;
        case KEY_QUIT:
            app->running = false;
            break;
        default:
            break;
    }
}

/* Dispatch a key to the handler for the current mode. */
static void handle_key(App *app, Key key) {
    switch (app->mode) {
        case UI_EDIT: handle_edit(app, key); break;
        case UI_JUMP: handle_jump(app, key); break;
        default:      handle_normal(app, key); break;
    }
}

/* ------------------------------------------------------------------ */
/* Setup / main loop                                                  */
/* ------------------------------------------------------------------ */

/* Build the default pattern path: <config-dir>/default.cells. Returns false if
   the config directory cannot be determined. */
static bool default_config_path(char *buf, size_t cap) {
    char dir[512];
    if (!settings_config_dir(dir, sizeof(dir))) {
        return false;
    }
    snprintf(buf, cap, "%s/default.cells", dir);
    return true;
}

static bool build_initial(Board *initial, const Options *opt) {
    if (opt->config_path != NULL) {
        /* An explicit -f must succeed; a bad path is a hard error. */
        char err[256];
        if (!config_load_file(initial, opt->config_path, err, sizeof(err))) {
            terminal_restore();
            printf(ANSI_SHOW_CURSOR);
            fprintf(stderr, "Error loading config: %s\n", err);
            return false;
        }
        return true;
    }

    /* No -f: try the default config file, then silently fall back to random. */
    char path[1024];
    if (default_config_path(path, sizeof(path)) &&
        config_load_file(initial, path, NULL, 0)) {
        return true;
    }
    board_randomize(initial, opt->density);
    return true;
}

int main(int argc, char **argv) {
    /* Start from persisted settings (or built-in defaults on first run), then
       let command-line options override for this run. */
    Settings settings;
    settings_defaults(&settings);
    settings_load(&settings);
    settings.width = clamp_dim(settings.width, MIN_DIM, HARD_MAX_W);
    settings.height = clamp_dim(settings.height, MIN_DIM, HARD_MAX_H);

    Options opt = {
        .width = settings.width, .height = settings.height,
        .delay_ms = settings.delay_ms, .density = settings.density,
        .seed = 0, .seed_set = false, .config_path = NULL,
    };

    int pr = parse_args(argc, argv, &opt);
    if (pr != 0) {
        return pr > 0 ? 0 : 1;
    }

    /* Enter raw mode up front: it is needed both for reading input and for
       querying the terminal (sixel support) before we render. */
    if (!terminal_init()) {
        fprintf(stderr, "This program requires an interactive terminal.\n");
        return 1;
    }
    atexit(terminal_restore);

    /* Sixel is the only renderer, so it is a hard requirement. */
    if (!terminal_query_sixel()) {
        terminal_restore();
        fprintf(stderr,
                "This program requires a sixel-capable terminal "
                "(e.g. iTerm2, Konsole, WezTerm, foot, xterm -ti vt340).\n"
                "If your terminal supports sixel but is not detected, force it "
                "with GOL_SIXEL=1.\n");
        return 1;
    }

    /* Fold the effective (CLI-overridden) parameters back into settings and
       persist them so the seed-region size, delay and density carry over to the
       next run. This also creates the file on first run. The world is always the
       unbounded one now; keep the fields coherent for older readers. */
    settings.width = clamp_dim(opt.width, MIN_DIM, HARD_MAX_W);
    settings.height = clamp_dim(opt.height, MIN_DIM, HARD_MAX_H);
    settings.delay_ms = opt.delay_ms;
    settings.density = opt.density;
    settings.world = 2; /* infinite */
    settings.wrap = false;
    opt.width = settings.width;
    opt.height = settings.height;
    settings_save(&settings); /* best effort */

    srand(opt.seed_set ? opt.seed : (unsigned int)time(NULL));

    App app = {0};
    app.mode = UI_NORMAL;
    app.sim = SIM_STOPPED;
    app.selected = BTN_START;
    app.delay_ms = opt.delay_ms;
    app.running = true;
    app.blink_on = true;
    app.follow = false;
    app.view_w = opt.width;
    app.view_h = opt.height;
    app.infinite_cell_px = INFINITE_CELL_PX;

    app.engine = engine_new();
    app.history = history_new(HISTORY_CAP);
    app.restart = NULL;
    if (app.engine == NULL || app.history == NULL) {
        fprintf(stderr, "Failed to allocate world engine\n");
        return 1;
    }

    /* Restore the terminal on termination, and wake the loop on window resize. No
       SA_RESTART on SIGWINCH so it interrupts poll() and wakes the loop. */
    struct sigaction sa_term = {0};
    sa_term.sa_handler = on_term;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);
    struct sigaction sa_winch = {0};
    sa_winch.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa_winch, NULL);

    /* Lay the initial pattern out in a dense seed board, hand it to the world,
       then discard the board — the world is unbounded from here on. */
    Board initial;
    if (!board_init(&initial, opt.width, opt.height)) {
        fprintf(stderr, "Failed to allocate board\n");
        engine_free(app.engine);
        history_free(app.history);
        return 1;
    }
    if (!build_initial(&initial, &opt)) {
        board_free(&initial);
        engine_free(app.engine);
        history_free(app.history);
        return 1;
    }
    seed_from_board(&app, &initial);
    board_free(&initial);

    app.cursor_x = app.cam_x + app.view_w / 2;
    app.cursor_y = app.cam_y + app.view_h / 2;

    printf(ANSI_HIDE_CURSOR ANSI_CLEAR);
    render(&app);

    while (app.running) {
        /* Edit mode wakes on a blink timer; a running simulation wakes on the
           frame timer; otherwise block until a key arrives. */
        int timeout;
        if (app.mode == UI_EDIT) {
            timeout = BLINK_MS;
        } else if (app.mode == UI_NORMAL && app.sim == SIM_RUNNING) {
            timeout = (int)app.delay_ms;
        } else {
            timeout = -1;
        }
        Key key = terminal_read_key(timeout);

        /* A window resize (SIGWINCH interrupts the poll above) is handled simply
           by redrawing: the viewport is recomputed from the terminal size every
           frame, so there is nothing to reallocate. */
        if (g_resized) {
            g_resized = 0;
        }

        if (app.mode == UI_EDIT) {
            /* Toggle the blink on timeout; keep the marker solid while the user
               is actively pressing keys. */
            app.blink_on = (key == KEY_NONE) ? !app.blink_on : true;
        }

        handle_key(&app, key);

        /* Coalesce a burst of further pending input into a single render. A fast
           mouse drag emits many motion events; rendering (and, in sixel mode,
           emitting a full-screen image) once per event backs up under load and
           floods the terminal with images. Instead, process everything already
           waiting, then draw once. terminal_read_key(0) polls without blocking
           and returns KEY_NONE when the buffer is empty. */
        while (app.running) {
            Key k = terminal_read_key(0);
            if (k == KEY_NONE) break;
            if (app.mode == UI_EDIT) app.blink_on = true;
            handle_key(&app, k);
        }

        if (app.running) {
            render(&app);
        }
    }

    /* Show the cursor; terminal_restore leaves the alternate screen, which
       restores the user's original screen (and discards our sixel images). */
    printf(ANSI_SHOW_CURSOR);
    fflush(stdout);
    terminal_restore();

    engine_free(app.engine);
    engine_snapshot_free(app.restart);
    history_free(app.history);
    return 0;
}
