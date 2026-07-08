#define _DEFAULT_SOURCE /* sigaction, SIGWINCH */

#include "board.h"
#include "config.h"
#include "engine.h"
#include "history.h"
#include "kitty.h"
#include "popup.h"
#include "rle.h"
#include "settings.h"
#include "sixel.h"
#include "terminal.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
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

/* Synchronized output (DEC private mode 2026). Everything written between
   SYNC_BEGIN and SYNC_END is composited by the terminal and presented as one
   frame, so a clear-then-redraw never shows the intermediate blank screen (a
   strobing white flash on every zoom notch otherwise — a photosensitivity
   hazard). Terminals that don't know the mode ignore both sequences, and the
   spec mandates a timeout so an unmatched BEGIN can't wedge the display. */
#define SYNC_BEGIN "\033[?2026h"
#define SYNC_END   "\033[?2026l"

/* Background for text overlays that float on top of the world image (the top
   status HUD and the bottom-right popup toast): opaque black cell with bright
   text so it stays readable over any pixels underneath. This is opacity, not
   decoration, so it stays even under NO_COLOR (dropping it would leave overlay
   text unreadable over the pixels beneath).

   Uses truecolor black (R=0,G=0,B=0) rather than the ANSI palette black
   (\033[40m) because KGP terminals (Ghostty et al.) may treat the ANSI
   "default" background as partially transparent over KGP images, while an
   explicit RGB colour is always opaque. */
#define HUD_BG           "\033[48;2;0;0;0;37m"

/* Rounded single-line box-drawing glyphs (UTF-8 bytes), used for every floating
   overlay's border. Safe on the modern sixel terminals this program requires
   (iTerm2/Konsole/WezTerm/foot). Explicit byte escapes so they don't depend on
   the compiler's execution charset. */
#define BX_H  "\xe2\x94\x80" /* ─ */
#define BX_V  "\xe2\x94\x82" /* │ */
#define BX_TL "\xe2\x95\xad" /* ╭ */
#define BX_TR "\xe2\x95\xae" /* ╮ */
#define BX_BR "\xe2\x95\xaf" /* ╯ */
#define BX_BL "\xe2\x95\xb0" /* ╰ */

/* Return to HUD_BG's white foreground / turn attributes off, without disturbing
   the opaque background — used to close a coloured or dimmed span mid-overlay. */
#define SGR_FG_RESET "\033[22;37m" /* bold+dim off, fg back to white */

/* Semantic emphasis for overlay text. Colour is suppressed under NO_COLOR (set in
   main); bold/dim are attributes (monochrome-safe) and always kept. */
static const char *SGR_TITLE = "\033[1;36m";  /* bold cyan — dialog titles */
static const char *SGR_ACCENT = "\033[36m";   /* cyan — active/"on" values */
static const char *SGR_OK = "\033[32m";       /* green — running */
static const char *SGR_WARN = "\033[33m";     /* yellow — paused */
#define SGR_DIM "\033[2m"                      /* dim — secondary metadata */

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

/* Below this many rows the image area is too small to be useful; the minimum
   usable width is derived at runtime from the compact button bar (see render) so
   the world is only drawn when even the narrow bar fits. */
#define MIN_USABLE_ROWS 8

/* Pixels per cell for the viewport in sixel mode. The viewport is sized so cells
   render around this size; the mouse wheel changes it to zoom (smaller = more
   cells visible). INFINITE_CELL_PX is the default. */
#define INFINITE_CELL_PX 10
#define INFINITE_CELL_MIN 1  /* 1 device pixel per cell — the chunky-zoom floor */
#define INFINITE_CELL_MAX SIXEL_CELL_MAX
#define ZOOM_STEP 1 /* pixels per wheel notch */
/* Sub-pixel zoom: once at 1px/cell, further zoom-out packs cells_per_px world
   cells into each screen pixel (OR-downsampled) so huge patterns fit whole. This
   caps how far out you can go — 256 cells/px lets a viewport span ~256x the
   screen's pixel width. */
#define CELLS_PER_PX_MAX 256

/* Jump / rewind. The history ring keeps the last HISTORY_CAP generations so a
   recent rewind is instant; a jump forward (or a deep rewind that must replay
   from generation 0) advances continuously, servicing input on a wall-clock
   interval (see run_forward / JUMP_SERVICE_MS) so a long jump never freezes the
   UI and quit is honoured mid-jump. */
#define HISTORY_CAP 1024
#define JUMP_FIELD_MAX 1000000000L /* cap the typed target to keep it sane */

/* Runtime speed control: the per-generation delay moves in fixed SPEED_STEP_MS
   nudges between "as fast as possible" (0 ms) and SPEED_MAX_DELAY_MS. */
#define SPEED_MAX_DELAY_MS 2000
#define SPEED_STEP_MS 10 /* delay change per +/- keypress */

/* Longest pattern filename the file prompt accepts. */
#define FILE_BUF_MAX 255

/* Top-level interaction mode. */
typedef enum {
    UI_NORMAL,
    UI_EDIT,
    UI_JUMP,
    UI_SAVE_NAME,  /* type a filename to save the current pattern */
    UI_LOAD_LIST,  /* browse/sort/pick a saved pattern (or type a path) */
    UI_CONFIRM,    /* a modal y/n over a save/load action */
    UI_HELP        /* a modal controls reference; closes on any key/click */
} UiMode;

/* Load-list sort key and a saved-pattern entry. */
typedef enum { SORT_NAME, SORT_SIZE, SORT_MTIME } SortKey;
typedef struct {
    char name[256]; /* filename within the saves dir (includes ".rle") */
    long size;      /* bytes */
    long mtime;     /* modification time (unix seconds) */
} SaveEntry;

/* What a UI_CONFIRM y/n decides. */
typedef enum {
    CONFIRM_OVERWRITE, /* a save would clobber an existing file */
    CONFIRM_REPLACE,   /* a load would replace a non-empty world */
    CONFIRM_DELETE     /* delete the selected saved pattern */
} ConfirmAction;

/* Simulation sub-state, meaningful in UI_NORMAL. */
typedef enum { SIM_STOPPED, SIM_RUNNING, SIM_PAUSED } SimState;

/* Bottom button bar. Order matters: it is the left-to-right layout. Each label
   carries its keyboard shortcut in parentheses; the same key triggers the button
   from UI_NORMAL (see handle_normal). BTN_PLAY is a toggle: it reads "Pause" while
   the sim runs and "Start" otherwise (see play_label). "Start" and "Pause" are
   both five letters, so the label below is a fixed-width stand-in that gives the
   bar its width — the live text is substituted at render time and never changes
   width. Quit is a button too (mouse-only users need not know 'q'). */
typedef enum {
    BTN_PLAY, BTN_STEP, BTN_RESET, BTN_EDIT, BTN_JUMP,
    BTN_SAVE, BTN_LOAD, BTN_HELP, BTN_QUIT,
    BTN_COUNT
} Button;

static const char *const BUTTON_LABELS[BTN_COUNT] = {
    " Start (P) ", " Step (N) ", " Reset (R) ", " Edit (E) ", " Jump (J) ",
    " Save (S) ", " Load (L) ", " Help (?) ", " Quit (Q) "};

/* Compact labels (no " (X)" shortcut suffix, no surrounding pad) used when the
   full bar would overflow the terminal width. Parallel to BUTTON_LABELS; the
   renderer, the geometry/hit-test, and the click-flash all pick the same array
   from the current width (see bar_labels), so they never disagree within a frame. */
static const char *const BUTTON_LABELS_SHORT[BTN_COUNT] = {
    "Start", "Step", "Reset", "Edit", "Jump",
    "Save", "Load", "Help", "Quit"};

typedef struct {
    UiMode mode;
    SimState sim;
    long gen;
    int selected;               /* button index (UI_NORMAL) */
    int cursor_x, cursor_y;     /* world cursor (UI_EDIT) */
    bool blink_on;              /* cursor blink phase (UI_EDIT) */

    int sx_last_w, sx_last_h;   /* last image pixel size (clear on change) */
    uint64_t sx_last_hash;      /* hash of the last emitted image bytes */
    bool sx_drawn;              /* an image has been emitted at least once */
    bool text_dirty;            /* stale full-screen text (a closed dialog / the
                                   too-small notice) to erase on the next KGP
                                   frame — with per-row EL, never a 2J, which
                                   would delete the image and flash (see
                                   emit_frame) */

    /* Graphics protocol: prefer KGP when the terminal supports it, otherwise
       fall back to sixel. Set once at startup via terminal_query_kitty(). */
    bool use_kitty;

    /* Button-bar hit boxes, recomputed each frame so the bar is mouse-clickable.
       Coordinates are 0-based to match the decoded SGR mouse position. bar_row is
       -1 when no clickable bar is on screen (e.g. inside a dialog). */
    int bar_row;
    int btn_col0[BTN_COUNT], btn_col1[BTN_COUNT]; /* [col0, col1) per button */

    /* The unbounded world: a Life engine (sparse today) with a pannable,
       zoomable viewport rendered straight from its live cells. */
    LifeEngine *engine;         /* the world */
    EngineSnapshot *restart;    /* generation-0 config restored by Reset */
    EngineSnapshot *edit_backup;/* world snapshot taken on entering UI_EDIT, so
                                   Esc can Discard the edits (NULL outside edit) */
    long edit_backup_gen;       /* the generation at that point */
    History *history;           /* recent generations, for instant rewind */
    int cam_x, cam_y;           /* viewport top-left in world coordinates */
    int view_w, view_h;         /* current viewport size in cells */
    int infinite_cell_px;       /* screen pixels per world cell when zoomed in
                                   (>=1); the chunky-zoom level */
    int cells_per_px;           /* world cells per screen pixel when zoomed out
                                   (>=1); the sub-pixel-zoom level. Exactly one of
                                   infinite_cell_px / cells_per_px is >1 at a time.
                                   When >1, each screen pixel is lit if ANY cell in
                                   its cells_per_px x cells_per_px block is alive. */
    bool follow;                /* auto-recentre the camera on the pattern */

    /* Jump (UI_JUMP): type a target generation to leap to (forward or back). */
    long jump_field;            /* the number being typed */
    bool jump_editing;          /* field typed into since it got focus */
    bool jumping;               /* a chunked jump is in progress (progress UI) */
    long jump_target;           /* its destination (for the progress readout) */

    /* Text entry shared by UI_SAVE_NAME (a filename) and the UI_LOAD_LIST
       "type a path" input. */
    char file_buf[FILE_BUF_MAX + 1];
    int file_len;
    char msg[320];              /* inline error shown inside the Save dialog */

    /* World-view toast: Save/Load results float at the bottom-right of the world
       and auto-hide after POPUP_TTL_MS (see popup.c). hud_w is the widest the top
       status bar has ever been (grow-only) so a shorter status never leaves a
       stale tail over the world when the image is not repainted. */
    Popup popup;
    int hud_w;

    /* Load browser (UI_LOAD_LIST). */
    SaveEntry *saves;           /* scanned *.rle in the saves dir */
    int save_count;
    int save_sel;               /* highlighted entry */
    int save_top;               /* first visible entry (scroll offset) */
    int save_visible;           /* entries shown by the last dialog frame */
    SortKey sort_key;
    bool sort_desc;
    bool load_typing;           /* the "type a path" input is active */
    /* Dialog hit boxes (0-based), recomputed each dialog frame for the mouse. */
    int dlg_sort_row, dlg_sort_c0[3], dlg_sort_c1[3];
    int dlg_typepath_row;
    int dlg_list_row0;          /* screen row of the first visible entry */

    /* Confirm (UI_CONFIRM): a Yes/No over a pending action. */
    ConfirmAction confirm_action;
    char confirm_path[1300];    /* the file the action targets */
    char confirm_msg[320];      /* the question shown */
    UiMode confirm_return;      /* mode to return to on cancel/after */
    int confirm_sel;            /* highlighted button: 0 = Yes, 1 = No */
    /* Yes/No hit boxes (0-based), recomputed each confirm frame for the mouse. */
    int dlg_confirm_row, dlg_yes_c0, dlg_yes_c1, dlg_no_c0, dlg_no_c1;

    /* Edit mouse (UI_EDIT): a bottom [Apply] [Discard] [Clear] [Pan: x] button
       row, a left-drag that paints cells (or pans the world when edit_pan is on),
       and the paint stroke's state so motion events interpolate into a line. */
    bool edit_pan;              /* when set, left-drag pans instead of painting */
    int edit_bar_row;           /* screen row of the Edit button row (-1 if none) */
    int edit_c0[4], edit_c1[4]; /* [Apply][Discard][Clear][Pan] hit boxes (0-based) */
    bool painting;              /* a left paint-stroke is in progress */
    bool paint_val;             /* value cells are set to for the current stroke */
    int paint_lx, paint_ly;     /* last painted world cell (stroke interpolation) */

    /* Modal-box geometry (0-based screen rect) and footer action buttons, so a
       click outside the box cancels and the [action]/[Cancel] footer is clickable.
       dlg_ok is the affirmative action (its span is empty when a dialog has none,
       e.g. Load/Help). Set each dialog frame in render_dialog. */
    int dlg_box_x0, dlg_box_y0, dlg_box_w, dlg_box_h;
    int dlg_act_row, dlg_ok_c0, dlg_ok_c1, dlg_cancel_c0, dlg_cancel_c1;

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

/* Restore the terminal on a crash (SIGSEGV/SIGABRT/…), then re-raise the signal
   with the default disposition so the process still crashes "properly" (correct
   exit status / core dump) — but from a sane terminal instead of one stuck in raw
   mode + alt screen with the cursor hidden. atexit() does NOT run for a
   signal-killed process, so without this a crash wrecks the user's terminal.
   write()/signal()/raise() are all async-signal-safe. */
static void on_crash(int sig) {
    ssize_t r = write(STDOUT_FILENO, ANSI_SHOW_CURSOR, sizeof(ANSI_SHOW_CURSOR) - 1);
    (void)r;
    terminal_restore();
    signal(sig, SIG_DFL);
    raise(sig);
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
    printf("Normal:  drag pan   wheel zoom   +/- speed   c center   f follow\n");
    printf("         j jump   x clear   s/l save/load RLE   Tab select   q quit\n");
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

/* Convert a length in screen pixels to a length in world cells at the current
   zoom. Scale is infinite_cell_px screen-pixels per world cell (chunky) or
   cells_per_px world cells per screen-pixel (sub-pixel); exactly one is >1, so
   this is spx * cells_per_px / infinite_cell_px. */
static int screen_px_to_world(const App *app, int spx) {
    return spx * app->cells_per_px / app->infinite_cell_px;
}

/* Human-readable zoom for the status line: "Npx" when chunky (N screen pixels per
   cell), or "1px=Nc" when zoomed out sub-pixel (one screen pixel covers N cells). */
static void zoom_label(const App *app, char *buf, size_t cap) {
    if (app->cells_per_px > 1) {
        snprintf(buf, cap, "1px=%dc", app->cells_per_px);
    } else {
        snprintf(buf, cap, "%dpx", app->infinite_cell_px);
    }
}

/* Size of the viewport in world cells, chosen to fill the terminal at the current
   zoom (you pan to see beyond it). Mirrors the reservations made by the renderer
   so the image plus controls fit without scrolling. At sub-pixel zoom the
   viewport spans many more cells than the display has pixels. */
static void infinite_viewport(const App *app, int *vw, int *vh) {
    int cols, rows, xpx, ypx;
    if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
        int cch = ypx / rows, ccw = xpx / cols;
        if (cch > 0 && ccw > 0) {
            int avail_w = xpx - ccw;
            int avail_h = ypx - SIXEL_UI_ROWS * cch;
            /* World cells spanning the drawable pixel area. avail_w/avail_h come
               from the ioctl ws_xpixel/ypixel (u16-bounded); with cells_per_px
               capped at CELLS_PER_PX_MAX this stays well within int. */
            *vw = clamp_dim(screen_px_to_world(app, avail_w), MIN_DIM, 1 << 22);
            *vh = clamp_dim(screen_px_to_world(app, avail_h), MIN_DIM, 1 << 22);
            return;
        }
    }
    *vw = 30; /* pixel size briefly unavailable (e.g. mid-resize) */
    *vh = 20;
}

/* A cell-plot target, dispatched-to at call sites. */
typedef void (*canvas_set_fn)(void *canvas, int col, int row);

/* Plot each live cell onto the canvas, translating world coordinates to
   viewport-local cells. Passed to sparse_query so only the live population is
   touched, never every cell of the (possibly huge) viewport. */
typedef struct {
    void *canvas;
    canvas_set_fn set_alive;
    int cam_x, cam_y;
    int cpp;                /* cells_per_px: world cells mapped to one canvas cell */
} PlotCtx;

static void plot_cell_cb(int x, int y, void *ud) {
    PlotCtx *p = (PlotCtx *)ud;
    /* At sub-pixel zoom (cpp > 1) a cpp x cpp block of world cells collapses onto
       one canvas cell; set_alive is idempotent, so the pixel lights if any cell
       in the block is alive (OR-downsampling). x,y are inside [cam, cam+vw), so
       the subtraction is non-negative and the division needs no flooring fix. */
    p->set_alive(p->canvas, (x - p->cam_x) / p->cpp,
                 (y - p->cam_y) / p->cpp);
}

/* Build the mode-appropriate status line for the top HUD. With styled=false the
   text is plain (no SGR), so strlen equals its printed width — the caller uses
   that for centring/padding. With styled=true the SAME visible characters are
   emitted with semantic colour/dim inserted (State value coloured by run-state,
   secondary fields dimmed, Follow accented when on); the visible width is
   unchanged, so both calls agree. The styled form is used only when the line fits
   without truncation (truncating across SGR is avoided). */
static void build_status(const App *app, char *buf, size_t cap, bool styled) {
    if (app->jumping) {
        snprintf(buf, cap, "JUMPING   Gen: %ld / %ld   Live: %zu   (Esc to stop)",
                 app->gen, app->jump_target, engine_population(app->engine));
    } else if (app->mode == UI_JUMP) {
        snprintf(buf, cap, "JUMP   Go to generation: %ld   (now: %ld)",
                 app->jump_field, app->gen);
    } else if (app->mode == UI_EDIT) {
        snprintf(buf, cap, "EDIT   Cursor: (%d,%d)   Live: %zu",
                 app->cursor_x, app->cursor_y, engine_population(app->engine));
    } else if (!styled) {
        char zbuf[16];
        zoom_label(app, zbuf, sizeof(zbuf));
        snprintf(buf, cap,
                 "State: %s   Gen: %ld   Cam: (%d,%d)   Live: %zu   "
                 "Zoom: %s   Delay: %ldms   Follow: %s",
                 sim_label(app->sim), app->gen, app->cam_x, app->cam_y,
                 engine_population(app->engine), zbuf,
                 app->delay_ms, app->follow ? "on" : "off");
    } else {
        char zbuf[16];
        zoom_label(app, zbuf, sizeof(zbuf));
        const char *scol = app->sim == SIM_RUNNING ? SGR_OK
                         : app->sim == SIM_PAUSED  ? SGR_WARN : SGR_DIM;
        const char *fcol = app->follow ? SGR_ACCENT : SGR_DIM;
        /* Same field order and visible text as the plain form (State, Gen, Cam,
           Live, Zoom, Delay, Follow) so the widths agree. State is coloured by
           run-state; Cam and Zoom/Delay are dimmed as secondary (Live stays bright
           between them); Follow's value is accented when on. Each span closes with
           SGR_FG_RESET, which keeps the opaque HUD background intact. */
        snprintf(buf, cap,
                 "State: %s%s" SGR_FG_RESET "   Gen: %ld   "
                 SGR_DIM "Cam: (%d,%d)" SGR_FG_RESET "   Live: %zu   "
                 SGR_DIM "Zoom: %s   Delay: %ldms" SGR_FG_RESET
                 "   Follow: %s%s" SGR_FG_RESET,
                 scol, sim_label(app->sim), app->gen, app->cam_x, app->cam_y,
                 engine_population(app->engine), zbuf, app->delay_ms,
                 fcol, app->follow ? "on" : "off");
    }
}

/* Total printed width of a button bar drawn from `labels`: each "[label]" is
   strlen+2 wide, joined by single spaces. */
static int labels_bar_width(const char *const *labels) {
    int w = 0;
    for (int b = 0; b < BTN_COUNT; b++) {
        w += (int)strlen(labels[b]) + 2;
        if (b) w += 1; /* separator space */
    }
    return w;
}

/* Pick the label set that fits `cols`: the full labels (with shortcut hints) when
   the bar fits, otherwise the compact set so it never overflows a narrow window.
   Every renderer/hit-test calls this with the current width, so they agree. */
static const char *const *bar_labels(int cols) {
    return labels_bar_width(BUTTON_LABELS) <= cols ? BUTTON_LABELS
                                                   : BUTTON_LABELS_SHORT;
}

/* The live text for the BTN_PLAY toggle: "Pause" while the sim is running,
   "Start" otherwise. Both are five letters, so this is the same width as the
   BTN_PLAY placeholder in BUTTON_LABELS / _SHORT — the bar layout and hit boxes
   (computed from those static labels) stay valid regardless of run state.
   `full` picks the padded/shortcut form to match the chosen label set. */
static const char *play_label(const App *app, bool full) {
    bool running = app->sim == SIM_RUNNING;
    if (full) return running ? " Pause (P) " : " Start (P) ";
    return running ? "Pause" : "Start";
}

/* The label to draw for button `b` from set `labels`: the BTN_PLAY toggle's live
   text, or the static label for every other button. */
static const char *button_label(const App *app, const char *const *labels, int b) {
    if (b == BTN_PLAY) return play_label(app, labels == BUTTON_LABELS);
    return labels[b];
}

/* The controls that sit *below* the world image: a centred button bar and, one
   blank row lower, a centred hint line. The status line moved to a HUD floating
   over the top of the world (see build_status / emit_frame). Drawn at absolute
   positions (cursor is at the row just below the image on entry): wipe the whole
   controls area first, then place the two centred rows. */
static void append_controls(App *app, char *buf, size_t cap, size_t *n,
                            int img_rows, int cols) {
    appendf(buf, cap, n, ANSI_CLR_BELOW);

    if (app->mode == UI_EDIT) {
        /* Edit has its own clickable action row instead of the main menu bar:
           [ Apply ] [ Discard ] [ Clear ] [ Pan: on/off ] (Pan highlighted when
           active). Records edit_bar_row / edit_c0..c1 for handle_edit_mouse. */
        const char *eb[4] = {"[ Apply ]", "[ Discard ]", "[ Clear ]",
                             app->edit_pan ? "[ Pan: on ]" : "[ Pan: off ]"};
        int tot = 0;
        for (int i = 0; i < 4; i++) tot += (int)strlen(eb[i]);
        tot += 3 * 2; /* three 2-space separators */
        int eleft = (cols - tot) / 2; if (eleft < 0) eleft = 0;
        app->edit_bar_row = img_rows + 1; /* 0-based, matches decoded mouse rows */
        appendf(buf, cap, n, "\033[%d;%dH", img_rows + 2, eleft + 1);
        int col = eleft;
        for (int i = 0; i < 4; i++) {
            if (i) { appendf(buf, cap, n, "  "); col += 2; }
            app->edit_c0[i] = col;
            app->edit_c1[i] = col + (int)strlen(eb[i]);
            if (i == 3 && app->edit_pan)
                appendf(buf, cap, n, ANSI_REVERSE "%s" ANSI_RESET, eb[i]);
            else
                appendf(buf, cap, n, "%s", eb[i]);
            col += (int)strlen(eb[i]);
        }
    } else {
        app->edit_bar_row = -1;
        /* Button bar, centred on the row one blank line below the image. Labels
           shrink to their compact form when the full bar would not fit the width. */
        const char *const *labels = bar_labels(cols);
        int bleft = (cols - labels_bar_width(labels)) / 2; if (bleft < 0) bleft = 0;
        appendf(buf, cap, n, "\033[%d;%dH", img_rows + 2, bleft + 1);
        /* Highlight the selected button in Normal, and while a world-overlay mode
           (Jump) is active, so a click that entered that mode keeps the cursor on
           the button it landed on. */
        bool show_sel = app->mode == UI_NORMAL || app->mode == UI_JUMP;
        for (int b = 0; b < BTN_COUNT; b++) {
            if (b) appendf(buf, cap, n, " ");
            const char *lab = button_label(app, labels, b);
            if (show_sel && b == app->selected) {
                appendf(buf, cap, n, ANSI_REVERSE "[%s]" ANSI_RESET, lab);
            } else {
                appendf(buf, cap, n, "[%s]", lab);
            }
        }
    }

    /* Hint, centred one blank row below the bar (no trailing newline, so writing
       it never scrolls the screen). */
    char hint[256];
    if (app->jumping) {
        snprintf(hint, sizeof(hint), "Jumping to generation %ld...   Esc to stop",
                 app->jump_target);
    } else if (app->mode == UI_JUMP) {
        snprintf(hint, sizeof(hint), "Type a generation   Wheel zoom  Drag pan   "
                 "Enter jump   Esc cancel");
    } else if (app->mode == UI_EDIT) {
        snprintf(hint, sizeof(hint), app->edit_pan
                 ? "PAN mode: drag to pan   Wheel zoom   click [Pan] to draw again"
                 : "Click/drag to draw   Wheel zoom   x clear   arrows+Space also work");
    } else {
        /* Only operations that are NOT on the menu bar (the bar carries its own
           shortcut letters); plus the Tab/Space menu-navigation aids. */
        snprintf(hint, sizeof(hint), "Drag pan  Wheel zoom  +/- speed  "
                 "c center  f follow  x clear   |   Tab select  Space activate");
    }
    /* Truncate to the width so a long hint never overflows a narrow terminal. */
    int hvis = (int)strlen(hint); if (hvis > cols) hvis = cols;
    int hleft = (cols - hvis) / 2; if (hleft < 0) hleft = 0;
    appendf(buf, cap, n, "\033[%d;%dH%.*s", img_rows + 4, hleft + 1, hvis, hint);
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

/* Record where the (centred) button bar and each button land on screen (0-based,
   matching the decoded SGR mouse position) so handle_mouse can hit-test clicks.
   The bar sits one blank row below the image (screen row img_rows+2 → 0-based
   bar_row img_rows+1) and is centred, so the columns mirror exactly what
   append_controls emits: bleft, then "[label]" (strlen+2 wide) joined by single
   spaces. */
static void compute_button_geometry(App *app, int img_rows, int cols) {
    app->bar_row = img_rows + 1;
    const char *const *labels = bar_labels(cols);
    int col = (cols - labels_bar_width(labels)) / 2; if (col < 0) col = 0;
    for (int b = 0; b < BTN_COUNT; b++) {
        int w = (int)strlen(labels[b]) + 2; /* the bracketed "[label]" */
        app->btn_col0[b] = col;
        app->btn_col1[b] = col + w;
        col += w + 1; /* + separator space */
    }
}

/* Repaint the button bar with the current selection highlighted and flush it to
   the terminal right now, reusing the geometry from the last emit_frame (bar_row).
   A mouse click on a menu button should give instant feedback — the cursor moving
   onto that button — before the button's action runs. For an action that opens a
   modal (Save/Load/Jump/Help) the main loop's next render draws the dialog and
   never repaints the bar, so without this the highlight would only appear after
   the modal closes. No-op if no bar is on screen (already inside a dialog). */
static void flash_selected_button(const App *app) {
    if (app->bar_row < 0) return;
    int cols, rows;
    if (!terminal_size(&cols, &rows)) return;
    char buf[1024]; size_t n = 0;
    const char *const *labels = bar_labels(cols);
    int bleft = (cols - labels_bar_width(labels)) / 2; if (bleft < 0) bleft = 0;
    /* Position at the bar row (1-based = bar_row+1) and reset any stale SGR. */
    appendf(buf, sizeof(buf), &n, ANSI_RESET "\033[%d;%dH", app->bar_row + 1, bleft + 1);
    for (int b = 0; b < BTN_COUNT; b++) {
        if (b) appendf(buf, sizeof(buf), &n, " ");
        const char *lab = button_label(app, labels, b);
        if (b == app->selected)
            appendf(buf, sizeof(buf), &n, ANSI_REVERSE "[%s]" ANSI_RESET, lab);
        else
            appendf(buf, sizeof(buf), &n, "[%s]", lab);
    }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}

/* Shared primitive behind every floating overlay (the modal dialogs and the popup
   toast): draw an opaque, bg-filled box at 1-based (x0,y0) of size w x h over the
   world, with a rounded single-line border. The border glyphs are multibyte
   UTF-8, so the interior run is built once and reused per row (rather than a
   single memset). The caller writes the box's content on top afterwards. */
static void overlay_box(char *buf, size_t cap, size_t *n,
                        int x0, int y0, int w, int h) {
    if (w < 1 || h < 1) return;
    int iw = w - 2; if (iw < 0) iw = 0; if (iw > 512) iw = 512;
    char hrun[512 * 3 + 1]; /* iw horizontal glyphs (3 bytes each) */
    size_t hl = 0;
    for (int i = 0; i < iw; i++) { memcpy(hrun + hl, BX_H, 3); hl += 3; }
    hrun[hl] = '\0';
    char blank[512 + 1];
    memset(blank, ' ', (size_t)iw); blank[iw] = '\0';
    for (int r = 0; r < h; r++) {
        appendf(buf, cap, n, "\033[%d;%dH" HUD_BG, y0 + r, x0);
        if (r == 0) {
            appendf(buf, cap, n, BX_TL "%s" BX_TR, hrun);
        } else if (r == h - 1) {
            appendf(buf, cap, n, BX_BL "%s" BX_BR, hrun);
        } else {
            appendf(buf, cap, n, BX_V "%s" BX_V, blank);
        }
        appendf(buf, cap, n, ANSI_RESET);
    }
}

/* Emit an already-encoded sixel image plus the controls below it. Re-emits the
   image only when it actually changed since the last frame (see the dedup note),
   so idle frames cost nothing and do not pile images into terminals that retain
   them (e.g. older iTerm2). Takes ownership of `img` and frees it. */
static void emit_frame(App *app, char *img, size_t img_len,
                       int img_w, int img_h, int cch) {
    const int img_rows = (img_h + cch - 1) / cch; /* char rows the image spans */
    int cols = 0, rows = 0;
    if (!terminal_size(&cols, &rows)) { cols = 80; rows = 24; }
    compute_button_geometry(app, img_rows, cols);

    const uint64_t hash = fnv1a(img, img_len);
    const bool image_changed = !app->sx_drawn || hash != app->sx_last_hash ||
                               img_w != app->sx_last_w || img_h != app->sx_last_h;
    fputs(SYNC_BEGIN, stdout); /* present the whole frame at once (no flash) */
    if (image_changed) {
        /* Sixel only: clear the screen when the image size changed — sixel
           pixels persist on the cells they covered, so a now-smaller image
           would leave stale pixels around it. Never do this on the KGP path:
           ED 2 deletes the terminal's images, and the big payloads at 1px /
           sub-pixel zoom decode too slowly to be re-placed in the same refresh,
           so every zoom notch flashed the background through (KGP needs no
           clear anyway — retransmitting the fixed id replaces the placement
           wholesale, leaving nothing stale). */
        if (img_w != app->sx_last_w || img_h != app->sx_last_h) {
            if (!app->use_kitty) fputs(ANSI_CLEAR, stdout);
            app->sx_last_w = img_w;
            app->sx_last_h = img_h;
        }
        fputs(ANSI_HOME, stdout);
        fwrite(img, 1, img_len, stdout);
        app->sx_last_hash = hash;
        app->sx_drawn = true;
    }
    free(img);

    /* KGP: erase stale full-screen text (a just-closed dialog, the too-small
       notice) row by row with EL, after the image is back on screen. EL erases
       glyphs but — unlike ED 2 — leaves the graphics layer alone, and erased
       cells are transparent over the z=-1 image, so the world shows through
       where the dialog was. The sixel path handles the same situation with the
       full clear above. */
    if (app->use_kitty && app->text_dirty) {
        char er[16 * 128];
        size_t en = 0;
        for (int r = 1; r <= rows && en + 16 < sizeof(er); r++)
            appendf(er, sizeof(er), &en, "\033[%d;1H\033[K", r);
        fwrite(er, 1, en, stdout);
    }
    app->text_dirty = false;

    /* Overlays floating on top of the world image, redrawn every frame so they
       stay current and survive an image repaint (which paints over them):
         - a status HUD centred on the world's top row, and
         - a transient popup toast in an asterisk-bordered box at the bottom-right.
       Both use an opaque cell background so they read over any pixels beneath. */
    char ov[2048];
    size_t on = 0;

    int maxw = cols - 2; if (maxw < 1) maxw = 1; /* room for the two pad spaces */
    char status[256];
    build_status(app, status, sizeof(status), false); /* plain: for the width */
    int full = (int)strlen(status);
    int svis = full; if (svis > maxw) svis = maxw;
    if (svis > app->hud_w) app->hud_w = svis; /* grow-only, no shrink artifacts */
    if (app->hud_w > maxw) app->hud_w = maxw;
    /* Centre the bar on the top row: 1 lead + hud_w content + 1 trail space. */
    int barw = app->hud_w + 2;
    int hstart = (cols - barw) / 2 + 1; if (hstart < 1) hstart = 1; /* 1-based */
    appendf(ov, sizeof(ov), &on, "\033[1;%dH" HUD_BG " ", hstart);
    if (full <= maxw) {
        /* Fits without truncation: draw the semantically-coloured version (same
           visible width as the plain text measured above). */
        char styled[512];
        build_status(app, styled, sizeof(styled), true);
        appendf(ov, sizeof(ov), &on, "%s" SGR_FG_RESET, styled);
    } else {
        appendf(ov, sizeof(ov), &on, "%.*s", svis, status);
    }
    for (int i = svis; i < app->hud_w; i++) appendf(ov, sizeof(ov), &on, " ");
    appendf(ov, sizeof(ov), &on, " " ANSI_RESET);

    /* Toast: a rounded box floating near the world's bottom-right, for a
       "notification" highlight. Three rows (top border, text, bottom border),
       inset up-and-left from the very corner by a small margin so it reads as a
       floating card rather than being jammed into the edge. */
    if (popup_visible(&app->popup)) {
        const int MX = 4, MY = 2;            /* right / bottom margins (cells) */
        int plen = (int)strlen(app->popup.text);
        int boxw = plen + 4;                 /* border ' ' text ' ' border */
        if (boxw > cols) boxw = cols;
        int x0 = cols - boxw + 1 - MX;        /* 1-based left column */
        if (x0 < 1) x0 = 1;
        int ytop = img_rows - 2 - MY;         /* top row; bottom == img_rows - MY */
        if (ytop < 1) ytop = 1;
        int inner = boxw - 4; if (inner < 0) inner = 0; /* text width inside the box */
        overlay_box(ov, sizeof(ov), &on, x0, ytop, boxw, 3);
        appendf(ov, sizeof(ov), &on, "\033[%d;%dH" HUD_BG "%-*.*s" ANSI_RESET,
                ytop + 1, x0 + 2, inner, inner, app->popup.text);
    }
    fwrite(ov, 1, on, stdout);

    /* Controls below the image (centred bar + hint). We run on the alternate
       screen buffer (no scrollback), so scrolled sixel images are discarded
       rather than retained — no per-frame scrollback clearing is needed here. */
    char ctl[2048];
    size_t n = 0;
    appendf(ctl, sizeof(ctl), &n, "\033[%d;1H", img_rows + 1);
    append_controls(app, ctl, sizeof(ctl), &n, img_rows, cols);
    fwrite(ctl, 1, n, stdout);

    fputs(SYNC_END, stdout);
    fflush(stdout);
}

/* Human-readable file size (B/K/M). */
static void human_size(long bytes, char *buf, size_t cap) {
    if (bytes < 1024) snprintf(buf, cap, "%ldB", bytes);
    else if (bytes < 1024 * 1024) snprintf(buf, cap, "%.1fK", bytes / 1024.0);
    else snprintf(buf, cap, "%.1fM", bytes / (1024.0 * 1024.0));
}

/* Local "YYYY-MM-DD HH:MM" for a unix time. */
static void fmt_mtime(long t, char *buf, size_t cap) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tm);
}

/* Window colours: opaque black background (truecolor, see HUD_BG note), bright
   text. */
#define DLG_BG "\033[48;2;0;0;0;37m"

/* Print one content line inside the modal at content-line `line` (0-based within
   the window's content area whose top-left is 0-based (cy, cx)). `text` may embed
   its own SGR (e.g. reverse for a selection); it is drawn over the already-black
   box, so short lines need no padding. */
static void dlg_row(char *buf, size_t cap, size_t *n, int cy, int cx, int line,
                    const char *text) {
    appendf(buf, cap, n, "\033[%d;%dH" DLG_BG "%s" ANSI_RESET,
            cy + line + 1, cx + 1, text);
}

/* A dialog title: bold + accent colour. */
static void dlg_title(char *buf, size_t cap, size_t *n, int cy, int cx, int line,
                      const char *text) {
    appendf(buf, cap, n, "\033[%d;%dH" DLG_BG "%s%s" ANSI_RESET,
            cy + line + 1, cx + 1, SGR_TITLE, text);
}

/* A dimmed content line (secondary metadata / footer hints). */
static void dlg_dim(char *buf, size_t cap, size_t *n, int cy, int cx, int line,
                    const char *text) {
    appendf(buf, cap, n, "\033[%d;%dH" DLG_BG SGR_DIM "%s" ANSI_RESET,
            cy + line + 1, cx + 1, text);
}

/* Draw a modal's clickable footer buttons on content-line `line`: an optional
   affirmative [ok] (pass NULL to omit) followed by a [cancel], and record their
   0-based screen hit boxes so the mouse can activate them. Gives every dialog a
   visible mouse exit (the keyboard Enter/Esc still work). */
static void dlg_actions(App *app, char *buf, size_t cap, size_t *n, int cy, int cx,
                        int line, const char *ok, const char *cancel) {
    app->dlg_act_row = cy + line;
    app->dlg_ok_c0 = app->dlg_ok_c1 = -1;
    char s[256];
    int p = 0, col = cx;
    if (ok != NULL) {
        int w = (int)strlen(ok);
        app->dlg_ok_c0 = col; app->dlg_ok_c1 = col + w;
        p += snprintf(s + p, sizeof(s) - (size_t)p, "%s   ", ok);
        col += w + 3;
    }
    int cw = (int)strlen(cancel);
    app->dlg_cancel_c0 = col; app->dlg_cancel_c1 = col + cw;
    snprintf(s + p, sizeof(s) - (size_t)p, "%s", cancel);
    dlg_row(buf, cap, n, cy, cx, line, s);
}

/* Draw a centred modal window (save / load / confirm) as a black bordered box
   floating over the world image, and record the mouse hit boxes in screen
   coordinates. The world around the box is left untouched; the image the box
   covers is force-redrawn on the next normal frame (sx_drawn cleared). */
static void render_dialog(App *app) {
    int cols, rows;
    if (!terminal_size(&cols, &rows)) { cols = 80; rows = 24; }
    if (cols < 24) cols = 24;
    if (rows < 10) rows = 10;

    /* Centre the box with a margin so the world shows around it. */
    int mxn = cols / 8; if (mxn < 3) mxn = 3; if (mxn > 12) mxn = 12;
    int myn = rows / 6; if (myn < 1) myn = 1; if (myn > 6) myn = 6;
    int w = cols - 2 * mxn; if (w > 504) w = 504; if (w < 34) w = (cols < 34 ? cols : 34);
    int h = rows - 2 * myn; if (h < 9) h = (rows < 9 ? rows : 9);
    int x0 = (cols - w) / 2; if (x0 < 0) x0 = 0; /* box top-left, 0-based */
    int y0 = (rows - h) / 2; if (y0 < 0) y0 = 0;
    int cx = x0 + 2;   /* content left, 0-based (1 border + 1 pad) */
    int cw = w - 4;    /* content width */
    int cy = y0 + 1;   /* content top row, 0-based */
    int chh = h - 2;   /* content rows */
    if (cw < 10) cw = 10;

    char dir[768];
    if (!settings_saves_dir(dir, sizeof(dir))) snprintf(dir, sizeof(dir), "(unknown)");

    size_t bcap = (size_t)(h + 4) * (size_t)(w * 2 + 160) + 4096;
    char *buf = malloc(bcap);
    if (buf == NULL) return;

    /* Clear the screen before the dialog. With KGP the world image lives on a
       separate graphics layer that text-cell backgrounds do not occlude, so a
       dialog drawn "over" the world would be see-through on that layer. A full
       clear removes the KGP image; the next world frame re-emits it. The clear
       and the dialog go out in one synchronized frame so the blank screen in
       between is never presented. */
    fputs(SYNC_BEGIN ANSI_CLEAR, stdout);

    size_t n = 0;
    app->bar_row = -1;
    app->dlg_sort_row = app->dlg_typepath_row = app->dlg_list_row0 = -1;
    app->dlg_confirm_row = -1;
    app->dlg_act_row = app->dlg_ok_c0 = app->dlg_ok_c1 = -1;
    app->dlg_cancel_c0 = app->dlg_cancel_c1 = -1;
    /* Box rect (0-based) for click-outside-to-cancel. */
    app->dlg_box_x0 = x0; app->dlg_box_y0 = y0;
    app->dlg_box_w = w;   app->dlg_box_h = h;

    /* 1) Frame: black fill + rounded border, overlaid on the world (shared with
       the toast via overlay_box; 1-based coords, so the 0-based x0/y0 get +1). */
    overlay_box(buf, bcap, &n, x0 + 1, y0 + 1, w, h);

    /* 2) Content, written over the black box. */
    char line[1024];
    if (app->mode == UI_SAVE_NAME) {
        dlg_title(buf, bcap, &n, cy, cx, 0, "Save pattern");
        snprintf(line, sizeof(line), "Folder: %.*s", cw - 8, dir);
        dlg_dim(buf, bcap, &n, cy, cx, 1, line);
        snprintf(line, sizeof(line), "Save as: \033[7m%s \033[0m" DLG_BG
                 SGR_DIM ".rle", app->file_buf);
        dlg_row(buf, bcap, &n, cy, cx, 3, line);
        /* Clickable footer buttons (mouse exit) + dim keyboard hint above them;
           any result message above that. */
        dlg_actions(app, buf, bcap, &n, cy, cx, chh - 1, "[ Save ]", "[ Cancel ]");
        dlg_dim(buf, bcap, &n, cy, cx, chh - 2,
                "Enter save   Esc cancel   (.rle added automatically)");
        if (app->msg[0]) {
            snprintf(line, sizeof(line), "%.*s", cw, app->msg);
            dlg_row(buf, bcap, &n, cy, cx, chh - 4, line);
        }
    } else if (app->mode == UI_HELP) {
        /* A compact controls reference. Kept short so it fits the box on small
           terminals (content rows = chh). Closes on any key/click (handle_help).
           Flush-left lines are section headers (accented); "  "-indented lines are
           entries. The title and the closing hint are drawn separately. */
        dlg_title(buf, bcap, &n, cy, cx, 0,
                  "Conway's Game of Life \342\200\224 Controls");
        static const char *const help[] = {
            "",
            "Menu (click, or Tab/arrows then Space):",
            "  Start/Pause (P)  run or pause",
            "  Step (N)         advance one generation",
            "  Reset (R)        back to generation 0",
            "  Edit (E)         Space toggle, x clear, Enter apply, Esc discard",
            "  Jump (J)         leap to any generation",
            "  Save (S) / Load (L)   store / recall a pattern",
            "  Help (?)  Quit (Q)",
            "",
            "World:",
            "  Drag = pan     Wheel = zoom",
            "  + / - = speed  c = center  f = follow",
            "  x = clear the world",
        };
        int nlines = (int)(sizeof(help) / sizeof(help[0]));
        for (int i = 0; i < nlines && i + 1 < chh - 1; i++) {
            if (help[i][0] != '\0' && help[i][0] != ' ') {
                /* Section header: accent (colour, not bold — avoid over-bolding). */
                snprintf(line, sizeof(line), "%s%.*s" SGR_FG_RESET,
                         SGR_ACCENT, cw, help[i]);
            } else {
                snprintf(line, sizeof(line), "%.*s", cw, help[i]);
            }
            dlg_row(buf, bcap, &n, cy, cx, i + 1, line);
        }
        dlg_dim(buf, bcap, &n, cy, cx, chh - 1, "Press any key or click to close.");
    } else if (app->mode == UI_CONFIRM) {
        int midr = chh / 2 - 1; if (midr < 1) midr = 1;
        snprintf(line, sizeof(line), "%.*s", cw, app->confirm_msg);
        dlg_row(buf, bcap, &n, cy, cx, midr, line);

        /* Two selectable, clickable buttons: [ Yes ]  [ No ]. The highlighted
           one (confirm_sel) is drawn in reverse video like the main menu bar. */
        const char *yes = "[ Yes ]", *no = "[ No ]";
        int gap = 4;
        int yw = (int)strlen(yes), nw = (int)strlen(no);
        app->dlg_confirm_row = cy + midr + 2;
        app->dlg_yes_c0 = cx;            app->dlg_yes_c1 = cx + yw;
        app->dlg_no_c0  = cx + yw + gap; app->dlg_no_c1  = cx + yw + gap + nw;
        snprintf(line, sizeof(line),
                 "%s%s" ANSI_RESET DLG_BG "    %s%s" ANSI_RESET DLG_BG,
                 app->confirm_sel == 0 ? ANSI_REVERSE : "", yes,
                 app->confirm_sel == 1 ? ANSI_REVERSE : "", no);
        dlg_row(buf, bcap, &n, cy, cx, midr + 2, line);
        dlg_dim(buf, bcap, &n, cy, cx, midr + 4,
                "Tab/Arrows move   Enter select   y/n shortcut   Esc = no");
    } else { /* UI_LOAD_LIST */
        snprintf(line, sizeof(line), "Load pattern  (%d)", app->save_count);
        dlg_title(buf, bcap, &n, cy, cx, 0, line);
        snprintf(line, sizeof(line), "Folder: %.*s", cw - 8, dir);
        dlg_dim(buf, bcap, &n, cy, cx, 1, line);

        /* Sort row (content line 3): the active key is reverse-highlighted and
           carries a ▲/▼ direction arrow. The arrow is one display cell but 3
           UTF-8 bytes, so the hit box for the active label is widened by 2 cells
           (" ▼") and visc is advanced by that display width, keeping the mouse
           hit-test aligned with what is drawn. */
        app->dlg_sort_row = cy + 3;
        const char *ar = app->sort_desc ? "\xe2\x96\xbc" : "\xe2\x96\xb2"; /* ▼ ▲ */
        int p = 0, visc = cx; /* visc: 0-based screen col of next visible char */
        p += snprintf(line + p, sizeof(line) - (size_t)p, "Sort: ");
        visc += 6;
        const char *sn[3] = {"Name", "Size", "Modified"};
        for (int k = 0; k < 3; k++) {
            bool active = (int)app->sort_key == k;
            int wl = (int)strlen(sn[k]) + 2 + (active ? 2 : 0); /* +" ▼" cells */
            app->dlg_sort_c0[k] = visc;
            app->dlg_sort_c1[k] = visc + wl;
            if (active) {
                p += snprintf(line + p, sizeof(line) - (size_t)p,
                              "\033[7m[%s %s]\033[0m" DLG_BG " ", sn[k], ar);
            } else {
                p += snprintf(line + p, sizeof(line) - (size_t)p, "[%s] ", sn[k]);
            }
            visc += wl + 1;
        }
        dlg_row(buf, bcap, &n, cy, cx, 3, line);

        /* Type-a-path row (content line 4). */
        app->dlg_typepath_row = cy + 4;
        if (app->load_typing) {
            snprintf(line, sizeof(line),
                     "Path: \033[7m%s \033[0m" DLG_BG SGR_DIM "  Enter load  Esc back",
                     app->file_buf);
            dlg_row(buf, bcap, &n, cy, cx, 4, line);
        } else {
            dlg_dim(buf, bcap, &n, cy, cx, 4, "[Type a path...]  (press /)");
        }

        /* Header (content line 5, dimmed) + list (from content line 6). */
        int namew = cw - 30; if (namew < 8) namew = 8;
        snprintf(line, sizeof(line), "%-*.*s %8s  %s", namew, namew,
                 "NAME", "SIZE", "MODIFIED");
        dlg_dim(buf, bcap, &n, cy, cx, 5, line);

        app->dlg_list_row0 = cy + 6;
        int listvis = chh - 6 - 2; /* leave the hint row + the button row */
        if (listvis < 1) listvis = 1;
        app->save_visible = listvis;
        if (app->save_sel < app->save_top) app->save_top = app->save_sel;
        if (app->save_sel >= app->save_top + listvis)
            app->save_top = app->save_sel - listvis + 1;
        if (app->save_top < 0) app->save_top = 0;

        if (app->save_count == 0) {
            dlg_dim(buf, bcap, &n, cy, cx, 6,
                    "(no saved patterns — press / to type a path)");
        } else {
            for (int i = 0; i < listvis; i++) {
                int idx = app->save_top + i;
                if (idx >= app->save_count) break;
                SaveEntry *se = &app->saves[idx];
                char szb[24], tb[24], nm[260];
                human_size(se->size, szb, sizeof(szb));
                fmt_mtime(se->mtime, tb, sizeof(tb));
                snprintf(nm, sizeof(nm), "%.*s", namew, se->name);
                if (idx == app->save_sel) {
                    snprintf(line, sizeof(line),
                             "\033[7m%-*s %8s  %s\033[0m" DLG_BG, namew, nm, szb, tb);
                } else {
                    snprintf(line, sizeof(line), "%-*s %8s  %s", namew, nm, szb, tb);
                }
                dlg_row(buf, bcap, &n, cy, cx, 6 + i, line);
            }
        }
        dlg_dim(buf, bcap, &n, cy, cx, chh - 2,
                "Up/Dn move  Enter/click load  d delete  / path  Esc cancel");
        dlg_actions(app, buf, bcap, &n, cy, cx, chh - 1, NULL, "[ Cancel ]");
    }

    fwrite(buf, 1, n, stdout);
    fputs(SYNC_END, stdout);
    fflush(stdout);
    free(buf);

    /* Next normal frame must repaint the image the box covered and erase the
       dialog's text (KGP path; the sixel path's full clear covers it). */
    app->sx_drawn = false;
    app->sx_last_w = app->sx_last_h = -1;
    app->text_dirty = true;
}

/* Draw a clean centred "terminal too small" notice on the alt screen, so a window
   too small for the world + controls shows guidance instead of a clipped mess or a
   stale frame. Forces a full repaint on the way back to a usable size. */
static void render_too_small(App *app, int cols, int rows, int min_cols) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Terminal too small (need %dx%d)",
             min_cols, MIN_USABLE_ROWS);
    int len = (int)strlen(msg);
    int col = (cols - len) / 2 + 1; if (col < 1) col = 1;
    int row = rows / 2 + 1;         if (row < 1) row = 1;
    char buf[128];
    size_t n = 0;
    appendf(buf, sizeof(buf), &n, SYNC_BEGIN ANSI_CLEAR "\033[%d;%dH%.*s" SYNC_END,
            row, col, cols, msg);
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    app->sx_drawn = false;
    app->sx_last_w = app->sx_last_h = -1;
    app->text_dirty = true;
}

/* Render the world: build the image straight from the sparse live-cell set,
   plotting only the O(population) cells that fall inside the viewport instead of
   probing every one of its (up to millions of) cells. Returns false if the
   layout cannot be computed (no pixel size, allocation failed); the caller then
   simply skips the frame. */
static bool render(App *app) {
    if (app->mode == UI_SAVE_NAME || app->mode == UI_LOAD_LIST ||
        app->mode == UI_CONFIRM || app->mode == UI_HELP) {
        render_dialog(app);
        return true;
    }
    int cols, rows, xpx, ypx;
    if (!terminal_size(&cols, &rows)) {
        return false;
    }
    /* Draw the world only when even the compact button bar fits the width. */
    const int min_cols = labels_bar_width(BUTTON_LABELS_SHORT);
    if (cols < min_cols || rows < MIN_USABLE_ROWS) {
        render_too_small(app, cols, rows, min_cols);
        return true;
    }
    if (!terminal_pixel_size(&xpx, &ypx)) {
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
    const int cpp = app->cells_per_px < 1 ? 1 : app->cells_per_px;

    /* The canvas is measured in screen pixels: vw world cells collapse to
       vw/cpp canvas cells (each `cell` px). One of cell/cpp is always 1.
       Create the canvas with the preferred graphics protocol. */
    const int ccols = (vw + cpp - 1) / cpp;
    const int crows = (vh + cpp - 1) / cpp;
    void *canvas = NULL;
    canvas_set_fn canvas_set;
    if (app->use_kitty) {
        KittyCanvas *kc = kitty_canvas_new(ccols, crows, cell);
        if (kc == NULL) return false;
        canvas = kc;
        canvas_set = (canvas_set_fn)kitty_canvas_set_alive;
    } else {
        SixelCanvas *sc = sixel_canvas_new(ccols, crows, cell);
        if (sc == NULL) return false;
        canvas = sc;
        canvas_set = (canvas_set_fn)sixel_canvas_set_alive;
    }

    PlotCtx ctx = {canvas, canvas_set, app->cam_x, app->cam_y, cpp};
    engine_query(app->engine, app->cam_x, app->cam_y,
                 app->cam_x + vw, app->cam_y + vh, plot_cell_cb, &ctx);

    if (app->mode == UI_EDIT && app->blink_on) {
        int cc = (app->cursor_x - app->cam_x) / cpp;
        int cr = (app->cursor_y - app->cam_y) / cpp;
        if (app->use_kitty)
            kitty_canvas_set_cursor((KittyCanvas *)canvas, cc, cr);
        else
            sixel_canvas_set_cursor((SixelCanvas *)canvas, cc, cr);
    }

    size_t img_len = 0;
    char *img = NULL;
    if (app->use_kitty) {
        img = kitty_canvas_encode((KittyCanvas *)canvas, &img_len);
        kitty_canvas_free((KittyCanvas *)canvas);
    } else {
        img = sixel_canvas_encode((SixelCanvas *)canvas, &img_len);
        sixel_canvas_free((SixelCanvas *)canvas);
    }
    if (img == NULL) return false;

    emit_frame(app, img, img_len, ccols * cell, crows * cell, cch);
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

/* Choose a zoom so a pattern spanning w x h cells fits the terminal's drawable
   pixel area, and centre the camera on world point (cx, cy). Only ever zooms
   *out* from the default (INFINITE_CELL_PX): a small pattern keeps the default
   chunky zoom; a big one first drops to 1px/cell, then to sub-pixel zoom
   (cells_per_px > 1) so even a pattern much wider than the screen fits whole.
   Falls back to the default zoom if the terminal pixel size is unavailable. */
static void fit_view_to(App *app, int cx, int cy, int w, int h) {
    app->infinite_cell_px = INFINITE_CELL_PX;
    app->cells_per_px = 1;
    int cols, rows, xpx, ypx;
    if (w > 0 && h > 0 &&
        terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
        int cch = ypx / rows, ccw = xpx / cols;
        if (cch > 0 && ccw > 0) {
            int avail_w = xpx - ccw;
            int avail_h = ypx - SIXEL_UI_ROWS * cch;
            int fx = avail_w / w, fy = avail_h / h;
            int px = fx < fy ? fx : fy; /* screen px per cell that would fit */
            if (px >= 1) {
                /* Fits at >=1px/cell: chunky zoom, never past the default. */
                app->infinite_cell_px = px > INFINITE_CELL_PX ? INFINITE_CELL_PX : px;
                app->cells_per_px = 1;
            } else {
                /* Too big for 1px/cell: pack ceil(cells/pixel) cells into each
                   screen pixel so the whole bbox fits (capped at the zoom-out
                   floor — a truly enormous pattern still spills and is panned). */
                int nx = (w + avail_w - 1) / avail_w;
                int ny = (h + avail_h - 1) / avail_h;
                int cpp = nx > ny ? nx : ny;
                if (cpp < 1) cpp = 1;
                if (cpp > CELLS_PER_PX_MAX) cpp = CELLS_PER_PX_MAX;
                app->infinite_cell_px = 1;
                app->cells_per_px = cpp;
            }
        }
    }
    int vw, vh;
    infinite_viewport(app, &vw, &vh);
    app->view_w = vw;
    app->view_h = vh;
    app->cam_x = cx - vw / 2;
    app->cam_y = cy - vh / 2;
}

/* Seed the world from a dense board (world coordinates match board coordinates),
   snapshot it as the restart config, and centre the viewport on it, zoomed to
   fit the loaded pattern. */
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

    int minx, miny, maxx, maxy;
    if (engine_bounds(app->engine, &minx, &miny, &maxx, &maxy)) {
        fit_view_to(app, minx + (maxx - minx) / 2, miny + (maxy - miny) / 2,
                    maxx - minx + 1, maxy - miny + 1);
    } else {
        int vw, vh;
        infinite_viewport(app, &vw, &vh);
        app->view_w = vw;
        app->view_h = vh;
        app->cam_x = b->width / 2 - vw / 2;
        app->cam_y = b->height / 2 - vh / 2;
    }
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

/* Empty the world to a blank slate: clears the live cells, makes empty the new
   restart config (so Reset stays blank), resets the generation and history.
   Deliberate — there is no pattern to recover afterwards except by reloading. */
static void clear_world(App *app) {
    engine_clear(app->engine);
    engine_snapshot_free(app->restart);
    app->restart = engine_snapshot(app->engine); /* empty */
    history_clear(app->history);
    app->gen = 0;
    app->sim = SIM_STOPPED;
}

/* Nudge the running speed. dir > 0 speeds up (less delay), dir < 0 slows down.
   Fixed SPEED_STEP_MS per keypress; a delay off the 10 ms grid (from the CLI or
   settings) snaps onto it after the first press. */
static void adjust_speed(App *app, int dir) {
    long d = app->delay_ms - (long)dir * SPEED_STEP_MS;
    d = (d / SPEED_STEP_MS) * SPEED_STEP_MS; /* snap onto the step grid */
    if (d < 0) d = 0;
    if (d > SPEED_MAX_DELAY_MS) d = SPEED_MAX_DELAY_MS;
    app->delay_ms = d;
}

/* Gather every live cell into a growable (x, y) array (for saving). */
typedef struct {
    int *xy;
    size_t n, cap;
    bool oom;
} CellVec;

static void gather_cb(int x, int y, void *ud) {
    CellVec *v = (CellVec *)ud;
    if (v->oom) return;
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        int *nx = realloc(v->xy, nc * 2 * sizeof(int));
        if (nx == NULL) { v->oom = true; return; }
        v->xy = nx;
        v->cap = nc;
    }
    v->xy[2 * v->n] = x;
    v->xy[2 * v->n + 1] = y;
    v->n++;
}

/* Write the whole current world to `path` as RLE, reporting on the status line. */
static void write_current_to(App *app, const char *path) {
    CellVec v = {0};
    engine_query(app->engine, INT_MIN, INT_MIN, INT_MAX, INT_MAX, gather_cb, &v);
    if (v.oom) {
        free(v.xy);
        popup_show(&app->popup, "Save failed: out of memory");
        return;
    }
    char err[128] = {0};
    bool ok = rle_save(path, v.xy, v.n, err, sizeof(err));
    size_t nn = v.n;
    free(v.xy);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (ok) {
        popup_show(&app->popup, "Saved %zu cells to %s", nn, base);
    } else {
        popup_show(&app->popup, "Save failed: %s", err);
    }
}

/* Core RLE loader shared by startup `-f` and the in-app `l`: parse `path`
   straight into the sparse engine (no dense board, so the only limit is the live
   cell count — a pattern with a huge but sparse bounding box loads fine), make it
   the restart config, reset generation/history, and centre + zoom-to-fit. On
   success writes the cell count to *out_count. Returns false (fills errbuf) on a
   parse/read error. */
static bool load_rle_file(App *app, const char *path, size_t *out_count,
                          char *errbuf, size_t errbuf_size) {
    int *cells = NULL;
    size_t count = 0;
    if (!rle_load(path, &cells, &count, errbuf, errbuf_size)) {
        return false;
    }
    /* Pattern is top-left near (0,0); place it at its own coordinates. */
    int w = 0, h = 0;
    for (size_t i = 0; i < count; i++) {
        if (cells[2 * i] + 1 > w) w = cells[2 * i] + 1;
        if (cells[2 * i + 1] + 1 > h) h = cells[2 * i + 1] + 1;
    }
    engine_clear(app->engine);
    for (size_t i = 0; i < count; i++) {
        engine_set(app->engine, cells[2 * i], cells[2 * i + 1], true);
    }
    free(cells);

    engine_snapshot_free(app->restart);
    app->restart = engine_snapshot(app->engine);
    history_clear(app->history);
    app->gen = 0;
    app->sim = SIM_STOPPED;
    fit_view_to(app, w / 2, h / 2, w, h);
    if (out_count) *out_count = count;
    return true;
}

static bool has_rle_ext(const char *path); /* defined below */

/* ---- Saves directory scanning + sorting ---------------------------------- */

#define SAVES_MAX 1000 /* safety cap on entries scanned into the load list */

/* qsort comparator state (single-threaded UI, so file-scope is fine). */
static SortKey g_sort_key;
static bool g_sort_desc;
static int save_cmp(const void *pa, const void *pb) {
    const SaveEntry *a = pa, *b = pb;
    int r = 0;
    switch (g_sort_key) {
        case SORT_NAME: r = strcasecmp(a->name, b->name); break;
        case SORT_SIZE: r = (a->size > b->size) - (a->size < b->size); break;
        case SORT_MTIME: r = (a->mtime > b->mtime) - (a->mtime < b->mtime); break;
    }
    if (r == 0) r = strcmp(a->name, b->name); /* stable tie-break by name */
    return g_sort_desc ? -r : r;
}
static void sort_saves(App *app) {
    g_sort_key = app->sort_key;
    g_sort_desc = app->sort_desc;
    if (app->save_count > 1) {
        qsort(app->saves, (size_t)app->save_count, sizeof(SaveEntry), save_cmp);
    }
}

/* (Re)scan the saves directory into app->saves, then sort and clamp selection. */
static void scan_saves(App *app) {
    free(app->saves);
    app->saves = NULL;
    app->save_count = 0;

    char dir[768];
    if (settings_saves_dir(dir, sizeof(dir))) {
        DIR *d = opendir(dir);
        if (d != NULL) {
            size_t cap = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL && app->save_count < SAVES_MAX) {
                if (e->d_name[0] == '.') continue;      /* skip dotfiles/./.. */
                if (!has_rle_ext(e->d_name)) continue;
                char full[1280];
                snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
                struct stat st;
                if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                if ((size_t)app->save_count == cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    SaveEntry *ns = realloc(app->saves, nc * sizeof(SaveEntry));
                    if (ns == NULL) break;
                    app->saves = ns;
                    cap = nc;
                }
                SaveEntry *se = &app->saves[app->save_count++];
                snprintf(se->name, sizeof(se->name), "%s", e->d_name);
                se->size = (long)st.st_size;
                se->mtime = (long)st.st_mtime;
            }
            closedir(d);
        }
    }
    sort_saves(app);
    if (app->save_sel >= app->save_count) app->save_sel = app->save_count - 1;
    if (app->save_sel < 0) app->save_sel = 0;
    app->save_top = 0;
}

/* ---- Confirm overlay ----------------------------------------------------- */

static bool do_load_path(App *app, const char *path); /* fwd */

static void enter_confirm(App *app, ConfirmAction action, const char *path,
                          const char *msg, UiMode return_to) {
    app->confirm_action = action;
    snprintf(app->confirm_path, sizeof(app->confirm_path), "%s", path ? path : "");
    snprintf(app->confirm_msg, sizeof(app->confirm_msg), "%s", msg);
    app->confirm_return = return_to;
    app->confirm_sel = 1; /* default to No — these guard destructive actions */
    app->mode = UI_CONFIRM;
}

/* Carry out (or cancel) the pending confirm action and pick the next mode. */
static void resolve_confirm(App *app, bool yes) {
    if (!yes) {
        app->mode = app->confirm_return;
        return;
    }
    switch (app->confirm_action) {
        case CONFIRM_OVERWRITE:
            write_current_to(app, app->confirm_path);
            app->mode = UI_NORMAL;
            break;
        case CONFIRM_REPLACE:
            if (!do_load_path(app, app->confirm_path)) {
                app->mode = app->confirm_return; /* load failed: back to list */
            } /* success sets UI_NORMAL inside do_load_path */
            break;
        case CONFIRM_DELETE:
            if (remove(app->confirm_path) == 0) {
                popup_show(&app->popup, "Deleted");
            } else {
                popup_show(&app->popup, "Delete failed");
            }
            app->mode = UI_LOAD_LIST;
            scan_saves(app);
            break;
    }
}

/* ---- Load ---------------------------------------------------------------- */

/* Load `path`, set the status line, and drop to normal mode on success (leaving
   the mode unchanged on failure so the caller can stay in the list). */
static bool do_load_path(App *app, const char *path) {
    size_t count = 0;
    char err[128] = {0};
    if (!load_rle_file(app, path, &count, err, sizeof(err))) {
        popup_show(&app->popup, "Load failed: %s", err);
        return false;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    popup_show(&app->popup, "Loaded %zu cells from %s", count, base);
    app->mode = UI_NORMAL;
    return true;
}

/* Load `path`, first confirming if it would replace a non-empty world. */
static void request_load(App *app, const char *path) {
    if (engine_population(app->engine) > 0) {
        enter_confirm(app, CONFIRM_REPLACE, path,
                      "Replace the current world with this pattern? (y/n)",
                      app->mode);
        return;
    }
    do_load_path(app, path);
}

static void load_selected(App *app) {
    if (app->save_count == 0) return;
    char dir[768];
    if (!settings_saves_dir(dir, sizeof(dir))) return;
    char path[1280];
    snprintf(path, sizeof(path), "%s/%s", dir, app->saves[app->save_sel].name);
    request_load(app, path);
}

static void delete_selected(App *app) {
    if (app->save_count == 0) return;
    char dir[768];
    if (!settings_saves_dir(dir, sizeof(dir))) return;
    char path[1280];
    snprintf(path, sizeof(path), "%s/%s", dir, app->saves[app->save_sel].name);
    char q[320];
    snprintf(q, sizeof(q), "Delete \"%.240s\"? (y/n)", app->saves[app->save_sel].name);
    enter_confirm(app, CONFIRM_DELETE, path, q, UI_LOAD_LIST);
}

/* ---- Save ---------------------------------------------------------------- */

/* A save filename must be non-empty, not hidden/traversing, and hold no slash so
   it stays inside the saves directory. */
static bool valid_save_name(const char *s) {
    if (s[0] == '\0' || s[0] == '.') return false;
    for (const char *p = s; *p; p++) {
        if (*p == '/') return false;
    }
    return true;
}

/* Try to save the typed name; opens an overwrite confirm if the file exists. */
static void request_save(App *app) {
    if (!valid_save_name(app->file_buf)) {
        snprintf(app->msg, sizeof(app->msg),
                 "Invalid name (no empty, leading '.', or '/')");
        return;
    }
    char dir[768];
    if (!settings_saves_dir(dir, sizeof(dir)) || !settings_mkdirs(dir)) {
        snprintf(app->msg, sizeof(app->msg), "Cannot create saves directory");
        return;
    }
    char path[1280];
    if (has_rle_ext(app->file_buf)) {
        snprintf(path, sizeof(path), "%s/%s", dir, app->file_buf);
    } else {
        snprintf(path, sizeof(path), "%s/%s.rle", dir, app->file_buf);
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char q[320];
        snprintf(q, sizeof(q), "Overwrite \"%.240s\"? (y/n)", base);
        enter_confirm(app, CONFIRM_OVERWRITE, path, q, UI_SAVE_NAME);
        return;
    }
    write_current_to(app, path);
    app->mode = UI_NORMAL;
}

/* ---- Entering the dialogs ------------------------------------------------ */

static void enter_save(App *app) {
    app->mode = UI_SAVE_NAME;
    app->file_len = 0;
    app->file_buf[0] = '\0';
    app->msg[0] = '\0';
}

static void enter_load(App *app) {
    app->mode = UI_LOAD_LIST;
    app->load_typing = false;
    app->file_len = 0;
    app->file_buf[0] = '\0';
    app->msg[0] = '\0';
    scan_saves(app);
}

/* Enter the jump prompt (type a target generation). */
static void enter_jump(App *app) {
    app->mode = UI_JUMP;
    app->jump_field = app->gen;
    app->jump_editing = false;
}

/* Milliseconds between now and an earlier monotonic timestamp. */
static long millis_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L +
           (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Advance from the current generation to `target` (> current), servicing input
   and redrawing on a wall-clock interval so quit stays responsive no matter how
   slow a single generation is. Records the trailing HISTORY_CAP generations so a
   later small rewind stays instant. During the jump: q / Ctrl-C quit the whole
   program immediately (highest priority — as long as a generation itself hasn't
   frozen, the quit is honoured within JUMP_SERVICE_MS); Esc aborts just the jump,
   leaving the world at whatever generation it reached.

   Servicing is time-based, not every-Nth-generation: a 250k-cell world spends
   ~0.1s per generation (so we would poll far too rarely at a fixed chunk), while
   a glider runs millions of generations a second (so polling every generation
   would drown the run in read() syscalls). Checking a cheap monotonic clock each
   generation and only touching the terminal every JUMP_SERVICE_MS gives bounded
   quit latency at both extremes. */
#define JUMP_SERVICE_MS 40
static void run_forward(App *app, long target) {
    app->jumping = true;
    app->jump_target = target;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (app->gen < target && app->running) {
        engine_advance(app->engine, 1);
        app->gen++;
        if (target - app->gen < (long)HISTORY_CAP) {
            history_record(app->history, app->gen, engine_snapshot(app->engine));
        }
        if (millis_since(&last) >= JUMP_SERVICE_MS) {
            /* Drain input first so a pending quit wins over drawing a frame. */
            for (;;) {
                Key k = terminal_read_key(0);
                if (k == KEY_NONE) break;
                if (k == KEY_QUIT) { app->running = false; break; } /* q / Ctrl-C */
                if (k == KEY_ESC) goto done; /* abort the jump, stay in the app */
            }
            if (!app->running) break;
            if (app->follow) recenter_camera(app);
            render(app); /* progress animation + the "JUMPING …" line */
            clock_gettime(CLOCK_MONOTONIC, &last);
        }
    }
done:
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

/* Perform a button's action. Shared by the menu bar (activate the selected
   button) and the keyboard shortcuts in handle_normal, so the two never drift. */
static void activate_button_at(App *app, int b) {
    switch (b) {
        case BTN_PLAY:
            /* One toggle: running -> paused, anything else -> running. */
            app->sim = (app->sim == SIM_RUNNING) ? SIM_PAUSED : SIM_RUNNING;
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
            app->selected = BTN_EDIT; /* keep the highlight on Edit while editing */
            app->sim = SIM_STOPPED;
            app->blink_on = true;
            app->edit_pan = false;    /* start in draw mode, not pan */
            app->painting = false;
            /* Snapshot the world so Esc can Discard the session's edits; Enter
               Applies (keeps them). */
            engine_snapshot_free(app->edit_backup);
            app->edit_backup = engine_snapshot(app->engine);
            app->edit_backup_gen = app->gen;
            /* Start the cursor at the centre of the current view. */
            app->cursor_x = app->cam_x + app->view_w / 2;
            app->cursor_y = app->cam_y + app->view_h / 2;
            break;
        case BTN_JUMP:
            app->selected = BTN_JUMP; /* keep the highlight on Jump while in the prompt */
            enter_jump(app);
            break;
        case BTN_SAVE:
            enter_save(app);
            break;
        case BTN_LOAD:
            enter_load(app);
            break;
        case BTN_HELP:
            app->mode = UI_HELP;
            break;
        case BTN_QUIT:
            app->running = false;
            break;
    }
}

static void activate_button(App *app) {
    activate_button_at(app, app->selected);
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
            *wdx = screen_px_to_world(app, dcol * ccw);
            *wdy = screen_px_to_world(app, drow * cch);
            return;
        }
    }
    *wdx = dcol; /* pixel size unavailable: fall back to raw cell delta */
    *wdy = drow;
}

/* Advance the zoom one wheel notch. dir = +1 zooms in, -1 zooms out. The zoom
   ladder runs, from most-in to most-out: cell_px 20..2..1 (chunky, ZOOM_STEP=1px
   per notch), then cells_per_px 1,2,4,… (sub-pixel, doubling per notch) up to
   CELLS_PER_PX_MAX. Returns true if the zoom actually changed. */
static bool zoom_step(App *app, int dir) {
    if (dir > 0) { /* zoom in */
        if (app->cells_per_px > 1) {
            app->cells_per_px /= 2; /* toward 1px/cell */
            return true;
        }
        int np = app->infinite_cell_px + ZOOM_STEP;
        if (np > INFINITE_CELL_MAX) np = INFINITE_CELL_MAX;
        if (np == app->infinite_cell_px) return false;
        app->infinite_cell_px = np;
        return true;
    }
    /* zoom out */
    if (app->infinite_cell_px > INFINITE_CELL_MIN) {
        int np = app->infinite_cell_px - ZOOM_STEP;
        if (np < INFINITE_CELL_MIN) np = INFINITE_CELL_MIN;
        app->infinite_cell_px = np;
        return true;
    }
    if (app->cells_per_px < CELLS_PER_PX_MAX) {
        app->cells_per_px *= 2; /* sub-pixel: pack more cells per pixel */
        return true;
    }
    return false;
}

/* Mouse-wheel zoom, keeping the world point under the cursor fixed so zoom feels
   centred there. dir = +1 zooms in, -1 zooms out. mx,my are the cursor's
   character-cell position. */
static void zoom_infinite(App *app, int dir, int mx, int my) {
    int cols, rows, xpx, ypx;
    bool have_px = terminal_size(&cols, &rows) &&
                   terminal_pixel_size(&xpx, &ypx) &&
                   xpx / cols > 0 && ypx / rows > 0;

    if (!have_px) {
        zoom_step(app, dir); /* no pixel size: zoom without re-anchoring */
        return;
    }
    int ccw = xpx / cols, cch = ypx / rows;
    int px_x = mx * ccw, px_y = my * cch;
    /* World cell currently under the cursor. */
    int world_x = app->cam_x + screen_px_to_world(app, px_x);
    int world_y = app->cam_y + screen_px_to_world(app, px_y);
    if (!zoom_step(app, dir)) return;
    /* Keep that world cell under the cursor at the new zoom. */
    app->cam_x = world_x - screen_px_to_world(app, px_x);
    app->cam_y = world_y - screen_px_to_world(app, px_y);
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
        /* Clicks anywhere in the controls area (below the image) never pan the
           world. A click on the bar row selects and activates its button; the
           blank spacer just above it is swallowed too. Everything from the image
           upward is the world, where a press begins a pan. */
        if (app->mode == UI_NORMAL && app->bar_row >= 0 && m.y >= app->bar_row - 1) {
            if (m.y == app->bar_row) {
                for (int b = 0; b < BTN_COUNT; b++) {
                    if (m.x >= app->btn_col0[b] && m.x < app->btn_col1[b]) {
                        app->selected = b;
                        /* Show the selection move immediately, before the action
                           (which may open a modal that hides the bar). */
                        flash_selected_button(app);
                        activate_button(app);
                        break;
                    }
                }
            }
            return;
        }
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
            /* Menu-button shortcuts (P/N/R/E/J/S/L/?) mirror the bar; the rest are
               world-navigation keys not on the bar (c/f/x, +/-). */
            {
                const int c = terminal_char();
                switch (c) {
                    case 'p': case 'P': activate_button_at(app, BTN_PLAY);  break;
                    case 'n': case 'N': activate_button_at(app, BTN_STEP);  break;
                    case 'r': case 'R': activate_button_at(app, BTN_RESET); break;
                    case 'e': case 'E': activate_button_at(app, BTN_EDIT);  break;
                    case 'j': case 'J': activate_button_at(app, BTN_JUMP);  break;
                    case 's': case 'S': activate_button_at(app, BTN_SAVE);  break;
                    case 'l': case 'L': activate_button_at(app, BTN_LOAD);  break;
                    case '?':           activate_button_at(app, BTN_HELP);  break;
                    case 'c': case 'C': recenter_camera(app);              break;
                    case 'f': case 'F':
                        app->follow = !app->follow;
                        if (app->follow) recenter_camera(app);
                        break;
                    case 'x': case 'X': clear_world(app);                  break;
                    case '+': case '=': adjust_speed(app, +1);            break;
                    case '-': case '_': adjust_speed(app, -1);            break;
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

/* Apply the edits: the edited world becomes the new restart config and starts a
   fresh timeline (generation 0). */
static void apply_edit(App *app) {
    engine_snapshot_free(app->restart);
    app->restart = engine_snapshot(app->engine);
    engine_snapshot_free(app->edit_backup);
    app->edit_backup = NULL;
    history_clear(app->history);
    app->gen = 0;
    app->sim = SIM_STOPPED;
    app->mode = UI_NORMAL;
}

/* Discard the edits: restore the world captured when edit mode began. */
static void discard_edit(App *app) {
    engine_restore(app->engine, app->edit_backup);
    app->gen = app->edit_backup_gen;
    engine_snapshot_free(app->edit_backup);
    app->edit_backup = NULL;
    app->sim = SIM_STOPPED;
    app->mode = UI_NORMAL;
}

/* Map a pointer at character-cell (mx,my) to the world cell under it — the inverse
   of the render transform (cam + screen_px_to_world(pixel)). Falls back to a raw
   cam offset if the terminal pixel size is unavailable. */
static void screen_cell_to_world(const App *app, int mx, int my, int *wx, int *wy) {
    int cols, rows, xpx, ypx;
    if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx) &&
        xpx / cols > 0 && ypx / rows > 0) {
        *wx = app->cam_x + screen_px_to_world(app, mx * (xpx / cols));
        *wy = app->cam_y + screen_px_to_world(app, my * (ypx / rows));
    } else {
        *wx = app->cam_x + mx;
        *wy = app->cam_y + my;
    }
}

/* Set every cell on the line from (x0,y0) to (x1,y1) to `val` (Bresenham), so a
   fast paint-drag leaves no gaps between motion events. */
static void paint_line(App *app, int x0, int y0, int x1, int y1, bool val) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        engine_set(app->engine, x0, y0, val);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Edit-mode mouse: the wheel zooms; the [Apply]/[Discard]/[Pan] button row is
   clickable; in the world area a left drag either paints cells (toggle-on-first-
   cell, then that value along the stroke) or, when Pan is on, pans the view. */
static void handle_edit_mouse(App *app) {
    MouseEvent m = terminal_mouse();
    if (m.button == 64 || m.button == 65) { /* wheel zoom */
        if (m.pressed) zoom_infinite(app, m.button == 64 ? +1 : -1, m.x, m.y);
        return;
    }
    /* A click on the Edit button row activates a button; clicks anywhere in the
       controls strip never paint/pan the world. */
    if (m.button == 0 && m.pressed && !m.motion && app->edit_bar_row >= 0 &&
        m.y >= app->edit_bar_row - 1) {
        if (m.y == app->edit_bar_row) {
            for (int i = 0; i < 4; i++) {
                if (m.x >= app->edit_c0[i] && m.x < app->edit_c1[i]) {
                    if (i == 0) apply_edit(app);
                    else if (i == 1) discard_edit(app);
                    else if (i == 2) engine_clear(app->engine); /* clear cells, stay in edit */
                    else app->edit_pan = !app->edit_pan;
                    break;
                }
            }
        }
        return;
    }
    if (app->edit_pan) { /* left-drag pans the world */
        if (m.button == 0 && m.pressed && !m.motion) {
            app->dragging = true;
            app->drag_mx = m.x; app->drag_my = m.y;
            app->drag_cam_x = app->cam_x; app->drag_cam_y = app->cam_y;
        } else if (m.button == 0 && m.motion && app->dragging) {
            int wdx, wdy;
            screen_delta_to_world(app, m.x - app->drag_mx, m.y - app->drag_my,
                                  &wdx, &wdy);
            app->cam_x = app->drag_cam_x - wdx;
            app->cam_y = app->drag_cam_y - wdy;
        } else if (!m.pressed) {
            app->dragging = false;
        }
        return;
    }
    /* Paint: press toggles the cell (and fixes the stroke value = its new state);
       drag continues that value along the pointer path. */
    if (m.button == 0 && m.pressed && !m.motion) {
        int wx, wy;
        screen_cell_to_world(app, m.x, m.y, &wx, &wy);
        app->paint_val = !engine_get(app->engine, wx, wy);
        engine_set(app->engine, wx, wy, app->paint_val);
        app->painting = true;
        app->paint_lx = wx; app->paint_ly = wy;
        app->cursor_x = wx; app->cursor_y = wy;
    } else if (m.button == 0 && m.motion && app->painting) {
        int wx, wy;
        screen_cell_to_world(app, m.x, m.y, &wx, &wy);
        paint_line(app, app->paint_lx, app->paint_ly, wx, wy, app->paint_val);
        app->paint_lx = wx; app->paint_ly = wy;
        app->cursor_x = wx; app->cursor_y = wy;
    } else if (!m.pressed) {
        app->painting = false;
    }
}

static void handle_edit(App *app, Key key) {
    if (key == KEY_MOUSE) { handle_edit_mouse(app); return; }
    switch (key) {
        case KEY_LEFT:  app->cursor_x--; break;
        case KEY_RIGHT: app->cursor_x++; break;
        case KEY_UP:    app->cursor_y--; break;
        case KEY_DOWN:  app->cursor_y++; break;
        case KEY_SPACE: {
            /* Space is the only edit action: toggle the cell under the cursor. */
            bool alive = engine_get(app->engine, app->cursor_x, app->cursor_y);
            engine_set(app->engine, app->cursor_x, app->cursor_y, !alive);
            break;
        }
        case KEY_ENTER: apply_edit(app); return;   /* keep the edits (gen 0) */
        case KEY_ESC:   discard_edit(app); return; /* restore the pre-edit world */
        case KEY_OTHER:
            /* x clears every cell but stays in Edit (Esc still restores the
               pre-edit world, Enter commits the now-empty one). */
            {
                const int c = terminal_char();
                if (c == 'x' || c == 'X') engine_clear(app->engine);
            }
            break;
        case KEY_QUIT:
            app->running = false;
            break;
        default:
            break;
    }
    follow_cursor(app);
}

/* Jump-mode mouse: the wheel zooms and a left drag pans the (static) world behind
   the prompt so you can frame it; a click on the bar's Jump button executes the
   jump, and a click on any other bar button cancels the jump and runs that action.
   Number entry stays on the keyboard. */
static void handle_jump_mouse(App *app) {
    MouseEvent m = terminal_mouse();
    if (m.button == 64 || m.button == 65) { /* wheel zoom */
        if (m.pressed) zoom_infinite(app, m.button == 64 ? +1 : -1, m.x, m.y);
        return;
    }
    if (m.button == 0 && m.pressed && !m.motion && app->bar_row >= 0 &&
        m.y >= app->bar_row - 1) {
        if (m.y == app->bar_row) {
            for (int b = 0; b < BTN_COUNT; b++) {
                if (m.x >= app->btn_col0[b] && m.x < app->btn_col1[b]) {
                    long target = app->jump_field;
                    app->mode = UI_NORMAL;
                    if (b == BTN_JUMP) jump_to(app, target); /* execute */
                    else { app->selected = b; activate_button(app); } /* switch */
                    break;
                }
            }
        }
        return; /* clicks in the controls strip never pan */
    }
    if (m.button == 0 && m.pressed && !m.motion) {
        app->dragging = true;
        app->drag_mx = m.x; app->drag_my = m.y;
        app->drag_cam_x = app->cam_x; app->drag_cam_y = app->cam_y;
    } else if (m.button == 0 && m.motion && app->dragging) {
        int wdx, wdy;
        screen_delta_to_world(app, m.x - app->drag_mx, m.y - app->drag_my, &wdx, &wdy);
        app->cam_x = app->drag_cam_x - wdx;
        app->cam_y = app->drag_cam_y - wdy;
    } else if (!m.pressed) {
        app->dragging = false;
    }
}

/* Type a target generation, then leap there (forward or backward). */
static void handle_jump(App *app, Key key) {
    if (key == KEY_MOUSE) { handle_jump_mouse(app); return; }
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

/* Edit the shared text field (save filename / load path). Sets *quit on Ctrl-C.
   'q'/'Q' are literal characters here (recovered via terminal_char()). */
static void file_buf_key(App *app, Key key, int *quit) {
    *quit = 0;
    if (key == KEY_BACKSPACE) {
        if (app->file_len > 0) app->file_buf[--app->file_len] = '\0';
    } else if (key == KEY_SPACE) {
        if (app->file_len < FILE_BUF_MAX) {
            app->file_buf[app->file_len++] = ' ';
            app->file_buf[app->file_len] = '\0';
        }
    } else if (key == KEY_QUIT) {
        int c = terminal_char();
        if (c == 0x03) {
            *quit = 1;
        } else if (app->file_len < FILE_BUF_MAX) {
            app->file_buf[app->file_len++] = (char)c;
            app->file_buf[app->file_len] = '\0';
        }
    } else if (key == KEY_OTHER) {
        int c = terminal_char();
        if (c >= 0x20 && c < 0x7f && app->file_len < FILE_BUF_MAX) {
            app->file_buf[app->file_len++] = (char)c;
            app->file_buf[app->file_len] = '\0';
        }
    }
}

/* Change the load-list sort key (toggling the order if the key is unchanged;
   otherwise pick a sensible default order: names ascending, size/date newest or
   largest first). */
static void set_sort(App *app, SortKey k) {
    if (app->sort_key == k) {
        app->sort_desc = !app->sort_desc;
    } else {
        app->sort_key = k;
        app->sort_desc = (k != SORT_NAME);
    }
    sort_saves(app);
}

/* Mouse inside the load list: wheel scrolls the selection; a left click on a
   sort header, the type-a-path line, or an entry acts on it (a row click loads). */
/* True if a left press landed outside the current modal box rect (0-based screen),
   so a click on the world dismisses the dialog. */
static bool dlg_click_outside(const App *app, const MouseEvent *m) {
    return m->x < app->dlg_box_x0 || m->x >= app->dlg_box_x0 + app->dlg_box_w ||
           m->y < app->dlg_box_y0 || m->y >= app->dlg_box_y0 + app->dlg_box_h;
}

/* Which footer button a left press hit: 1 = the affirmative [ok], -1 = [Cancel],
   0 = neither. */
static int dlg_action_hit(const App *app, const MouseEvent *m) {
    if (m->y != app->dlg_act_row) return 0;
    if (app->dlg_ok_c0 >= 0 && m->x >= app->dlg_ok_c0 && m->x < app->dlg_ok_c1)
        return 1;
    if (m->x >= app->dlg_cancel_c0 && m->x < app->dlg_cancel_c1) return -1;
    return 0;
}

static void handle_dialog_mouse(App *app) {
    MouseEvent m = terminal_mouse();
    if (m.button == 64 || m.button == 65) { /* wheel */
        if (m.pressed) {
            if (m.button == 64 && app->save_sel > 0) app->save_sel--;
            if (m.button == 65 && app->save_sel < app->save_count - 1) app->save_sel++;
        }
        return;
    }
    if (!(m.button == 0 && m.pressed && !m.motion) || app->load_typing) return;
    /* [Cancel] button or a click outside the box closes the dialog. */
    if (dlg_action_hit(app, &m) == -1 || dlg_click_outside(app, &m)) {
        app->mode = UI_NORMAL;
        return;
    }
    if (m.y == app->dlg_sort_row) {
        for (int k = 0; k < 3; k++) {
            if (m.x >= app->dlg_sort_c0[k] && m.x < app->dlg_sort_c1[k]) {
                set_sort(app, (SortKey)k);
                return;
            }
        }
        return;
    }
    if (m.y == app->dlg_typepath_row) {
        app->load_typing = true;
        app->file_len = 0;
        app->file_buf[0] = '\0';
        return;
    }
    if (app->dlg_list_row0 >= 0 && m.y >= app->dlg_list_row0 &&
        m.y < app->dlg_list_row0 + app->save_visible) {
        int idx = app->save_top + (m.y - app->dlg_list_row0);
        if (idx >= 0 && idx < app->save_count) {
            app->save_sel = idx;
            load_selected(app);
        }
    }
}

/* UI_SAVE_NAME: type a name, Enter saves (may open an overwrite confirm). The
   [Save]/[Cancel] footer buttons and a click outside the box give a mouse exit. */
static void handle_save_name(App *app, Key key) {
    if (key == KEY_MOUSE) {
        MouseEvent m = terminal_mouse();
        if (m.button == 0 && m.pressed && !m.motion) {
            int hit = dlg_action_hit(app, &m);
            if (hit == 1) { if (app->file_len > 0) request_save(app); return; }
            if (hit == -1 || dlg_click_outside(app, &m)) { app->mode = UI_NORMAL; }
        }
        return;
    }
    if (key == KEY_ENTER) {
        if (app->file_len > 0) request_save(app);
        return;
    }
    if (key == KEY_ESC) { app->mode = UI_NORMAL; return; }
    int quit;
    file_buf_key(app, key, &quit);
    if (quit) app->running = false;
}

/* UI_LOAD_LIST: browse/sort/pick a saved pattern, or type an arbitrary path. */
static void handle_load_list(App *app, Key key) {
    if (app->load_typing) {
        if (key == KEY_ENTER) {
            if (app->file_len > 0) request_load(app, app->file_buf);
            app->load_typing = false;
            return;
        }
        if (key == KEY_ESC) { app->load_typing = false; return; }
        int quit;
        file_buf_key(app, key, &quit);
        if (quit) app->running = false;
        return;
    }
    if (key == KEY_MOUSE) { handle_dialog_mouse(app); return; }
    switch (key) {
        case KEY_UP:   if (app->save_sel > 0) app->save_sel--; break;
        case KEY_DOWN: if (app->save_sel < app->save_count - 1) app->save_sel++; break;
        case KEY_ENTER:
        case KEY_SPACE: load_selected(app); break;
        case KEY_ESC:  app->mode = UI_NORMAL; break;
        case KEY_QUIT: app->running = false; break; /* q / Ctrl-C */
        case KEY_OTHER: {
            int c = terminal_char();
            if (c == 'n' || c == 'N') set_sort(app, SORT_NAME);
            else if (c == 's' || c == 'S') set_sort(app, SORT_SIZE);
            else if (c == 'm' || c == 'M') set_sort(app, SORT_MTIME);
            else if (c == 'd' || c == 'D') delete_selected(app);
            else if (c == '/') {
                app->load_typing = true;
                app->file_len = 0;
                app->file_buf[0] = '\0';
            }
            break;
        }
        default: break;
    }
}

/* Hit-test a left click on the confirm dialog's Yes/No buttons. */
static void handle_confirm_mouse(App *app) {
    MouseEvent m = terminal_mouse();
    if (!(m.button == 0 && m.pressed && !m.motion)) return;
    /* A click outside the box is "No" (cancel), like Esc. */
    if (dlg_click_outside(app, &m)) { resolve_confirm(app, false); return; }
    if (m.y != app->dlg_confirm_row) return;
    if (m.x >= app->dlg_yes_c0 && m.x < app->dlg_yes_c1) {
        app->confirm_sel = 0;
        resolve_confirm(app, true);
    } else if (m.x >= app->dlg_no_c0 && m.x < app->dlg_no_c1) {
        app->confirm_sel = 1;
        resolve_confirm(app, false);
    }
}

/* UI_CONFIRM: Tab/Arrows move between [Yes] and [No]; Enter/Space activate the
   highlighted one; y/n are shortcuts; Esc = no; a click hits either button;
   Ctrl-C still quits. */
static void handle_confirm(App *app, Key key) {
    switch (key) {
        case KEY_ESC:   resolve_confirm(app, false); return;
        case KEY_MOUSE: handle_confirm_mouse(app); return;
        case KEY_LEFT:  app->confirm_sel = 0; return;
        case KEY_RIGHT: app->confirm_sel = 1; return;
        case KEY_TAB:   app->confirm_sel ^= 1; return;
        case KEY_ENTER:
        case KEY_SPACE: resolve_confirm(app, app->confirm_sel == 0); return;
        case KEY_QUIT: {
            int c = terminal_char();
            if (c == 0x03) app->running = false; /* Ctrl-C */
            else resolve_confirm(app, false);    /* 'q' = no */
            return;
        }
        case KEY_OTHER: {
            int c = terminal_char();
            if (c == 'y' || c == 'Y') resolve_confirm(app, true);
            else if (c == 'n' || c == 'N') resolve_confirm(app, false);
            return;
        }
        default: return;
    }
}

/* The Help overlay is purely informational: any key or mouse click dismisses it
   back to the world. Ctrl-C still quits the whole program. */
static void handle_help(App *app, Key key) {
    if (key == KEY_NONE) return;
    if (key == KEY_QUIT && terminal_char() == 0x03) {
        app->running = false;
        return;
    }
    if (key == KEY_MOUSE) {
        /* Dismiss only on a fresh left-button *press*. Help is opened by a click,
           so the matching button *release* arrives while we are already in UI_HELP
           — treating that (or motion/wheel) as a dismiss would close Help the
           instant it opened. Ignore everything but a real press. */
        MouseEvent m = terminal_mouse();
        if (!(m.button == 0 && m.pressed && !m.motion)) return;
    }
    app->mode = UI_NORMAL;
    /* The world was covered by the box; force a repaint to erase it. */
    app->sx_drawn = false;
    app->sx_last_w = app->sx_last_h = -1;
}

/* Dispatch a key to the handler for the current mode. */
static void handle_key(App *app, Key key) {
    switch (app->mode) {
        case UI_EDIT:      handle_edit(app, key); break;
        case UI_JUMP:      handle_jump(app, key); break;
        case UI_SAVE_NAME: handle_save_name(app, key); break;
        case UI_LOAD_LIST: handle_load_list(app, key); break;
        case UI_CONFIRM:   handle_confirm(app, key); break;
        case UI_HELP:      handle_help(app, key); break;
        default:           handle_normal(app, key); break;
    }
}

/* ------------------------------------------------------------------ */
/* Setup / main loop                                                  */
/* ------------------------------------------------------------------ */

/* Build the default pattern path: <saves-dir>/default.rle. The startup default
   lives in the saves directory (with the user's own saves), so the config
   directory holds only configuration. Returns false if the saves directory
   cannot be determined. */
static bool default_pattern_path(char *buf, size_t cap) {
    char dir[768];
    if (!settings_saves_dir(dir, sizeof(dir))) {
        return false;
    }
    snprintf(buf, cap, "%s/default.rle", dir);
    return true;
}

/* True if `path` ends (case-insensitively) with `.rle`. */
static bool has_rle_ext(const char *path) {
    size_t n = strlen(path);
    return n >= 4 &&
           (path[n - 4] == '.') &&
           (path[n - 3] == 'r' || path[n - 3] == 'R') &&
           (path[n - 2] == 'l' || path[n - 2] == 'L') &&
           (path[n - 1] == 'e' || path[n - 1] == 'E');
}

/* Load a `.cells` pattern into the dense seed board, or fall back to a random
   fill. RLE files do NOT come here — main routes both an explicit `-f *.rle` and
   the startup default (saves/default.rle) straight into the sparse engine via
   load_rle_file, so a big pattern is never clipped to the -w/-h seed board. This
   path handles only an explicit `.cells` `-f` and the random start. */
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
    board_randomize(initial, opt->density);
    return true;
}

int main(int argc, char **argv) {
    /* Honour NO_COLOR (no-color.org): drop the semantic colours, keeping only
       bold/dim attributes (monochrome-safe) and the structural overlay
       background. */
    if (getenv("NO_COLOR") != NULL) {
        SGR_TITLE = "\033[1m"; /* bold, no colour */
        SGR_ACCENT = SGR_OK = SGR_WARN = "";
    }

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

    /* Pick the graphics protocol: prefer KGP (zlib-compressed, lower bandwidth)
       and fall back to sixel when the terminal does not support it. At least one
       of them is required — the program exits if neither is available. */
    bool has_kitty = terminal_query_kitty();
    bool has_sixel = has_kitty || terminal_query_sixel();
    if (!has_sixel) {
        terminal_restore();
        fprintf(stderr,
                "This program requires a graphics-capable terminal.\n"
                "Sixel: iTerm2, Konsole, WezTerm, foot, xterm -ti vt340\n"
                "  (force with GOL_SIXEL=1)\n"
                "Kitty Graphics Protocol: Kitty, WezTerm, Konsole >= 24.08\n"
                "  (force with GOL_KITTY=1)\n");
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
    app.selected = BTN_PLAY;
    app.delay_ms = opt.delay_ms;
    app.running = true;
    app.blink_on = true;
    app.follow = false;
    app.view_w = opt.width;
    app.view_h = opt.height;
    app.infinite_cell_px = INFINITE_CELL_PX;
    app.cells_per_px = 1;
    app.sort_key = SORT_MTIME; /* load list defaults to newest-first */
    app.sort_desc = true;
    app.use_kitty = has_kitty;

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
    /* Restore the terminal on a crash too (atexit does not run for these). */
    struct sigaction sa_crash = {0};
    sa_crash.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa_crash, NULL);
    sigaction(SIGABRT, &sa_crash, NULL);
    sigaction(SIGBUS, &sa_crash, NULL);
    sigaction(SIGILL, &sa_crash, NULL);
    sigaction(SIGFPE, &sa_crash, NULL);

    /* Seed the world. RLE loads (an explicit `-f *.rle`, or the startup default
       saves/default.rle) go straight into the sparse engine — no dense board, so
       a huge/sparse pattern is never clipped or over-allocated. An explicit
       `.cells` `-f` and the random fallback lay out in a transient dense seed
       board. The world is unbounded from here on. */
    bool seeded = false;
    if (opt.config_path != NULL && has_rle_ext(opt.config_path)) {
        char err[256];
        if (!load_rle_file(&app, opt.config_path, NULL, err, sizeof(err))) {
            terminal_restore();
            printf(ANSI_SHOW_CURSOR);
            fprintf(stderr, "Error loading config: %s\n", err);
            engine_free(app.engine);
            history_free(app.history);
            return 1;
        }
        seeded = true;
    } else if (opt.config_path == NULL) {
        /* No -f: try the default pattern (saves/default.rle); a missing/bad file
           is not an error — it just falls through to the random start. */
        char dpath[1024];
        if (default_pattern_path(dpath, sizeof(dpath)) &&
            load_rle_file(&app, dpath, NULL, NULL, 0)) {
            seeded = true;
        }
    }
    if (!seeded) {
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
    }

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
        /* An idle frame with a live popup must still wake in time to auto-hide it. */
        if (timeout < 0 && app.popup.active) {
            long rem = popup_remaining_ms(&app.popup);
            timeout = rem > 0 ? (int)rem : 1;
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

        /* Auto-hide an expired popup and repaint the world where it floated. */
        if (popup_expire(&app.popup)) {
            app.sx_drawn = false;
            app.sx_last_w = app.sx_last_h = -1;
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
    engine_snapshot_free(app.edit_backup);
    history_free(app.history);
    free(app.saves);
    return 0;
}
