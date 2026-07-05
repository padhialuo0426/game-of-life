#define _DEFAULT_SOURCE /* sigaction, SIGWINCH */

#include "board.h"
#include "config.h"
#include "settings.h"
#include "sixel.h"
#include "terminal.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define MIN_DIM 3
/* Absolute safety caps (bound memory even on an absurdly large terminal). The
   effective per-run maximum is the smaller value that fits the terminal; see
   fit_limits(). */
#define HARD_MAX_W 1000
#define HARD_MAX_H 1000
/* Screen space the UI needs around the grid, used to fit the board to the
   terminal. Horizontally: the two border columns, plus two columns per cell.
   Vertically: top+bottom border, status, buttons, hint, two blank separators,
   and one spare row so the final newline never scrolls the screen. */
#define GRID_H_OVERHEAD 2
#define GRID_V_OVERHEAD 8
#define BLINK_MS 400
#define MAX_DELAY_MS 3600000 /* 1 hour; keeps the poll timeout within int */

/* Sixel (real-pixel) rendering. When the terminal supports sixel graphics the
   board is drawn as a bitmap instead of text cells, so its size is limited by
   pixels rather than by the character grid. */
#define SIXEL_UI_ROWS 6   /* char rows reserved below the image for the UI */
#define SIXEL_CELL_MAX 20 /* max pixels per cell (keeps small boards sane) */

/* Top-level interaction mode. */
typedef enum { UI_NORMAL, UI_EDIT, UI_CANVAS } UiMode;

/* Simulation sub-state, meaningful in UI_NORMAL. */
typedef enum { SIM_STOPPED, SIM_RUNNING, SIM_PAUSED } SimState;

/* Bottom button bar. Order matters: it is the left-to-right layout. */
typedef enum {
    BTN_START, BTN_STEP, BTN_PAUSE, BTN_STOP, BTN_EDIT, BTN_CANVAS, BTN_QUIT,
    BTN_COUNT
} Button;

static const char *const BUTTON_LABELS[BTN_COUNT] = {
    " Start ", " Step ", " Pause ", " Stop ", " Edit ", " Canvas ", " Quit "};

typedef struct {
    Board initial; /* starting config, restored by Stop */
    Board a, b;    /* double-buffered simulation boards */
    Board *cur, *next;

    UiMode mode;
    SimState sim;
    long gen;
    int selected;               /* button index (UI_NORMAL) */
    int cursor_x, cursor_y;     /* grid cursor (UI_EDIT) */
    bool blink_on;              /* cursor blink phase (UI_EDIT) */
    int pending_w, pending_h;   /* target size (UI_CANVAS) */
    bool pending_wrap;          /* target topology (UI_CANVAS) */
    bool wrap;                  /* current topology: true=toroidal, false=finite */
    int max_w, max_h;           /* largest board that fits the terminal now */

    bool sixel;                 /* draw the board as a sixel bitmap */
    int sx_last_w, sx_last_h;   /* last sixel image pixel size (clear on change) */

    long delay_ms;
    bool running;
    Settings *settings; /* persisted on change (canvas apply) */
} App;

static const char *sim_label(SimState s) {
    switch (s) {
        case SIM_RUNNING: return "RUNNING";
        case SIM_PAUSED:  return "PAUSED ";
        default:          return "STOPPED";
    }
}

static const char *topo_label(bool wrap) {
    return wrap ? "Toroidal" : "Finite";
}

/* Set by the SIGWINCH handler; the main loop notices it and refits the board. */
static volatile sig_atomic_t g_resized = 0;

static void on_winch(int sig) {
    (void)sig;
    g_resized = 1;
}

/* Restore the terminal and exit on a terminating signal. */
static void on_term(int sig) {
    terminal_restore();
    _Exit(128 + sig);
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
    bool wrap;
    const char *config_path;
} Options;

static void print_usage(const char *prog) {
    printf("Conway's Game of Life (interactive)\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -w, --width N     board width in cells   (default 30)\n");
    printf("  -h, --height N    board height in cells  (default 20)\n");
    printf("  -d, --delay MS    delay between frames in ms (default 120)\n");
    printf("  -p, --density F   random initial density 0..1 (default 0.25)\n");
    printf("  -s, --seed N      random seed (default: time-based)\n");
    printf("  -f, --file PATH   load initial config from a pattern file\n");
    printf("                    (default: ~/.config/game-of-life/default.cells if\n");
    printf("                     present, otherwise a random board)\n");
    printf("      --wrap        start in toroidal (wrap-around) topology\n");
    printf("                    (default: finite grid; edges kill escaping cells)\n");
    printf("      --help        show this help and exit\n\n");
    printf("Settings such as board size and world type are remembered between\n");
    printf("runs in ~/.config/game-of-life/settings.json.\n\n");
    printf("Buttons: Start Step Pause Stop Edit Canvas Quit\n");
    printf("Normal:  Tab/Left/Right select   Space/Enter activate   q quit\n");
    printf("Edit:    arrows move cursor   Space toggle cell   Tab/Esc leave\n");
    printf("Canvas:  Left/Right width   Up/Down height   Space finite/toroidal\n");
    printf("         Enter apply   Esc cancel\n");
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
        } else if (strcmp(a, "--wrap") == 0) {
            opt->wrap = true;
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

/* Append the controls block shared by both renderers: status line, a blank
   line, the button bar, a blank line, and the hint line. Occupies five text
   rows (plus the caller's clear-below). */
static void append_controls(const App *app, char *buf, size_t cap, size_t *n) {
    const Board *board = app->cur;

    switch (app->mode) {
        case UI_EDIT:
            appendf(buf, cap, n, " EDIT   Cursor: (%d,%d)   Size: %dx%d" EOL EOL,
                    app->cursor_x, app->cursor_y, board->width, board->height);
            break;
        case UI_CANVAS:
            appendf(buf, cap, n,
                    " CANVAS   New: %d x %d   World: %s   (current %dx%d %s)" EOL EOL,
                    app->pending_w, app->pending_h, topo_label(app->pending_wrap),
                    board->width, board->height, topo_label(app->wrap));
            break;
        default:
            appendf(buf, cap, n, " State: %s   Gen: %ld   Size: %dx%d   World: %s" EOL EOL,
                    sim_label(app->sim), app->gen, board->width, board->height,
                    topo_label(app->wrap));
            break;
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

    switch (app->mode) {
        case UI_EDIT:
            appendf(buf, cap, n,
                    " Arrows move cursor   Space/Enter toggle   Tab/Esc leave   q quit" EOL);
            break;
        case UI_CANVAS:
            appendf(buf, cap, n,
                    " Left/Right width   Up/Down height   Space world   Enter apply   Esc cancel" EOL);
            break;
        default:
            appendf(buf, cap, n,
                    " Tab/Left/Right select   Space/Enter activate   q quit" EOL);
            break;
    }
}

/* Text renderer: a box-drawing grid with two columns per cell. Used when sixel
   is unavailable. */
static void render_chars(const App *app) {
    const Board *board = app->cur;
    const int inner = board->width * 2; /* two columns per cell => square look */
    size_t cap = (size_t)board->width * board->height * 12 +
                 (size_t)inner * 8 + 2048;
    char *buf = malloc(cap);
    if (buf == NULL) return;
    size_t n = 0;

    appendf(buf, cap, &n, ANSI_HOME);

    /* Top border. */
    appendf(buf, cap, &n, "┌");
    for (int i = 0; i < inner; i++) appendf(buf, cap, &n, "─");
    appendf(buf, cap, &n, "┐" EOL);

    /* Grid rows. In edit mode the cell under the cursor blinks: on the "on"
       phase it shows a hollow square outline, on the "off" phase the real cell,
       so the marker flashes while still revealing the cell's true state. */
    for (int y = 0; y < board->height; y++) {
        appendf(buf, cap, &n, "│");
        for (int x = 0; x < board->width; x++) {
            const char *glyph = board_get(board, x, y) ? "██" : "  ";
            bool at_cursor = (app->mode == UI_EDIT &&
                              x == app->cursor_x && y == app->cursor_y);
            if (at_cursor && app->blink_on) {
                appendf(buf, cap, &n, "[]"); /* hollow square marker */
            } else {
                appendf(buf, cap, &n, "%s", glyph);
            }
        }
        appendf(buf, cap, &n, "│" EOL);
    }

    /* Bottom border. */
    appendf(buf, cap, &n, "└");
    for (int i = 0; i < inner; i++) appendf(buf, cap, &n, "─");
    appendf(buf, cap, &n, "┘" EOL);

    append_controls(app, buf, cap, &n);

    /* Erase anything left below (e.g. rows from a previously taller board). */
    appendf(buf, cap, &n, ANSI_CLR_BELOW);

    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    free(buf);
}

/* Sixel renderer: draw the board as a bitmap (one cell = cell_px pixels, chosen
   to fill the available space), then the text controls below it. Returns false
   if the layout cannot be computed (no pixel size, board too tall, etc.), so
   the caller can fall back to the text renderer. */
static bool render_sixel(App *app) {
    const Board *board = app->cur;

    int cols, rows, xpx, ypx;
    if (!terminal_size(&cols, &rows) || !terminal_pixel_size(&xpx, &ypx)) {
        return false;
    }
    const int cch = ypx / rows; /* pixel height of one character cell */
    const int ccw = xpx / cols; /* pixel width  of one character cell */
    if (cch <= 0 || ccw <= 0) return false;

    const int avail_w = xpx - ccw;                 /* ~1 column right margin */
    const int avail_h = ypx - SIXEL_UI_ROWS * cch; /* rows reserved for the UI */
    if (avail_w < board->width || avail_h < board->height) {
        return false; /* cannot fit even one pixel per cell */
    }

    /* Zoom to fit: largest square cell that fits both dimensions. */
    int cell = avail_w / board->width;
    int ch = avail_h / board->height;
    if (ch < cell) cell = ch;
    if (cell < 1) cell = 1;
    if (cell > SIXEL_CELL_MAX) cell = SIXEL_CELL_MAX;

    const int img_w = board->width * cell;
    const int img_h = board->height * cell;
    const int img_rows = (img_h + cch - 1) / cch; /* char rows the image spans */

    const int cx = (app->mode == UI_EDIT) ? app->cursor_x : -1;
    const int cy = (app->mode == UI_EDIT) ? app->cursor_y : -1;
    const bool con = (app->mode == UI_EDIT) && app->blink_on;

    size_t img_len = 0;
    char *img = sixel_render_board(board, cell, cx, cy, con, &img_len);
    if (img == NULL) return false;

    /* Header: clear the screen only when the image size changed (avoids leaving
       stale pixels around a now-smaller image without flickering every frame). */
    if (img_w != app->sx_last_w || img_h != app->sx_last_h) {
        fputs(ANSI_CLEAR, stdout);
        app->sx_last_w = img_w;
        app->sx_last_h = img_h;
    }
    fputs(ANSI_HOME, stdout);
    fwrite(img, 1, img_len, stdout);
    free(img);

    /* Controls, positioned just below the image. */
    char ctl[2048];
    size_t n = 0;
    appendf(ctl, sizeof(ctl), &n, "\033[%d;1H", img_rows + 1);
    append_controls(app, ctl, sizeof(ctl), &n);
    appendf(ctl, sizeof(ctl), &n, ANSI_CLR_BELOW);
    fwrite(ctl, 1, n, stdout);

    fflush(stdout);
    return true;
}

static void render(App *app) {
    if (app->sixel && render_sixel(app)) return;
    render_chars(app);
}

/* ------------------------------------------------------------------ */
/* Board lifecycle helpers                                            */
/* ------------------------------------------------------------------ */

static void step_once(App *app) {
    board_step(app->cur, app->next, app->wrap);
    Board *tmp = app->cur;
    app->cur = app->next;
    app->next = tmp;
    app->gen++;
}

/* Sync the persisted settings with the current board size / world type and
   write them out, so the next run starts from the same configuration. */
static void persist_settings(App *app) {
    if (app->settings == NULL) return;
    app->settings->width = app->cur->width;
    app->settings->height = app->cur->height;
    app->settings->wrap = app->wrap;
    settings_save(app->settings); /* best effort; ignore failure */
}

/* Resize all three boards to nw x nh, preserving the existing initial config
   centered inside the new dimensions. Returns false on allocation failure. */
static bool resize_boards(App *app, int nw, int nh) {
    Board old = app->initial; /* borrows old.cells until we free it below */

    Board ni;
    if (!board_init(&ni, nw, nh)) {
        return false;
    }
    int offx = (nw - old.width) / 2;
    int offy = (nh - old.height) / 2;
    for (int y = 0; y < old.height; y++) {
        for (int x = 0; x < old.width; x++) {
            board_set(&ni, offx + x, offy + y, board_get(&old, x, y));
        }
    }

    board_free(&app->initial); /* frees old.cells */
    board_free(&app->a);
    board_free(&app->b);

    app->initial = ni;
    if (!board_init(&app->a, nw, nh) || !board_init(&app->b, nw, nh)) {
        return false;
    }
    app->cur = &app->a;
    app->next = &app->b;
    board_copy(app->cur, &app->initial);
    app->gen = 0;
    app->sim = SIM_STOPPED;
    /* Keep the edit cursor inside the new bounds. */
    if (app->cursor_x >= nw) app->cursor_x = nw - 1;
    if (app->cursor_y >= nh) app->cursor_y = nh - 1;
    return true;
}

/* ------------------------------------------------------------------ */
/* Per-mode input handling                                            */
/* ------------------------------------------------------------------ */

static void activate_button(App *app) {
    switch (app->selected) {
        case BTN_START:
            if (app->sim != SIM_RUNNING) app->sim = SIM_RUNNING;
            break;
        case BTN_STEP:
            step_once(app);
            app->sim = SIM_PAUSED;
            break;
        case BTN_PAUSE:
            if (app->sim == SIM_RUNNING) app->sim = SIM_PAUSED;
            break;
        case BTN_STOP:
            app->sim = SIM_STOPPED;
            board_copy(app->cur, &app->initial);
            app->gen = 0;
            break;
        case BTN_EDIT:
            app->mode = UI_EDIT;
            app->sim = SIM_STOPPED;
            app->blink_on = true;
            break;
        case BTN_CANVAS:
            app->mode = UI_CANVAS;
            app->pending_w = app->cur->width;
            app->pending_h = app->cur->height;
            app->pending_wrap = app->wrap;
            break;
        case BTN_QUIT:
            app->running = false;
            break;
    }
}

static void handle_normal(App *app, Key key) {
    switch (key) {
        case KEY_NONE:
            if (app->sim == SIM_RUNNING) step_once(app);
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
        case KEY_QUIT:
            app->running = false;
            break;
        default:
            break;
    }
}

static void handle_edit(App *app, Key key) {
    switch (key) {
        case KEY_LEFT:
            if (app->cursor_x > 0) app->cursor_x--;
            break;
        case KEY_RIGHT:
            if (app->cursor_x < app->cur->width - 1) app->cursor_x++;
            break;
        case KEY_UP:
            if (app->cursor_y > 0) app->cursor_y--;
            break;
        case KEY_DOWN:
            if (app->cursor_y < app->cur->height - 1) app->cursor_y++;
            break;
        case KEY_SPACE:
        case KEY_ENTER: {
            bool alive = board_get(app->cur, app->cursor_x, app->cursor_y);
            board_set(app->cur, app->cursor_x, app->cursor_y, !alive);
            break;
        }
        case KEY_TAB:
        case KEY_ESC:
            /* Leave edit mode; the edited board becomes the new initial. */
            board_copy(&app->initial, app->cur);
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
}

static void handle_canvas(App *app, Key key) {
    switch (key) {
        case KEY_LEFT:
            if (app->pending_w > MIN_DIM) app->pending_w--;
            break;
        case KEY_RIGHT:
            if (app->pending_w < app->max_w) app->pending_w++;
            break;
        case KEY_DOWN:
            if (app->pending_h > MIN_DIM) app->pending_h--;
            break;
        case KEY_UP:
            if (app->pending_h < app->max_h) app->pending_h++;
            break;
        case KEY_SPACE:
            app->pending_wrap = !app->pending_wrap; /* finite <-> toroidal */
            break;
        case KEY_ENTER:
            /* Only reallocate (and reset) when the size actually changed, so
               a pure topology switch keeps the running simulation intact. */
            if (app->pending_w != app->cur->width ||
                app->pending_h != app->cur->height) {
                if (!resize_boards(app, app->pending_w, app->pending_h)) {
                    app->running = false; /* allocation failed: bail out */
                }
            }
            app->wrap = app->pending_wrap;
            app->mode = UI_NORMAL;
            persist_settings(app); /* remember size / world type for next run */
            break;
        case KEY_TAB:
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

/* Clamp a dimension into the supported range. */
static int clamp_dim(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Compute the largest board (cells) that fits the current terminal, bounded by
   the absolute safety caps. In sixel mode the limit is the pixel budget (down
   to one pixel per cell); otherwise it is the character grid. Falls back to the
   caps if the size is unavailable (e.g. not a terminal). */
static void fit_limits(bool sixel, int *max_w, int *max_h) {
    if (sixel) {
        int cols, rows, xpx, ypx;
        if (terminal_size(&cols, &rows) && terminal_pixel_size(&xpx, &ypx)) {
            int cch = ypx / rows, ccw = xpx / cols;
            if (cch > 0 && ccw > 0) {
                *max_w = clamp_dim(xpx - ccw, MIN_DIM, HARD_MAX_W);
                *max_h = clamp_dim(ypx - SIXEL_UI_ROWS * cch, MIN_DIM, HARD_MAX_H);
                return;
            }
        }
        /* Pixel size unavailable: fall through to the character-grid formula. */
    }
    int cols, rows;
    if (terminal_size(&cols, &rows)) {
        *max_w = clamp_dim((cols - GRID_H_OVERHEAD) / 2, MIN_DIM, HARD_MAX_W);
        *max_h = clamp_dim(rows - GRID_V_OVERHEAD, MIN_DIM, HARD_MAX_H);
    } else {
        *max_w = HARD_MAX_W;
        *max_h = HARD_MAX_H;
    }
}

/* React to a terminal resize: recompute the fit limits and, if the board no
   longer fits, shrink it to fit (keeping pending canvas values in bounds). */
static void handle_winch(App *app) {
    fit_limits(app->sixel, &app->max_w, &app->max_h);

    int nw = app->cur->width  > app->max_w ? app->max_w : app->cur->width;
    int nh = app->cur->height > app->max_h ? app->max_h : app->cur->height;
    if (nw != app->cur->width || nh != app->cur->height) {
        if (!resize_boards(app, nw, nh)) {
            app->running = false; /* allocation failed: bail out */
            return;
        }
        persist_settings(app);
    }
    if (app->pending_w > app->max_w) app->pending_w = app->max_w;
    if (app->pending_h > app->max_h) app->pending_h = app->max_h;
}

int main(int argc, char **argv) {
    /* Start from persisted settings (or built-in defaults on first run), then
       let command-line options override for this run. */
    Settings settings;
    settings_defaults(&settings);
    bool settings_existed = settings_load(&settings);
    settings.width = clamp_dim(settings.width, MIN_DIM, HARD_MAX_W);
    settings.height = clamp_dim(settings.height, MIN_DIM, HARD_MAX_H);

    Options opt = {
        .width = settings.width, .height = settings.height,
        .delay_ms = settings.delay_ms, .density = settings.density,
        .seed = 0, .seed_set = false, .wrap = settings.wrap, .config_path = NULL,
    };

    int pr = parse_args(argc, argv, &opt);
    if (pr != 0) {
        return pr > 0 ? 0 : 1;
    }

    /* Enter raw mode up front: it is needed both for reading input and for
       querying the terminal (sixel support) before we size the board. */
    if (!terminal_init()) {
        fprintf(stderr, "This program requires an interactive terminal.\n");
        return 1;
    }
    atexit(terminal_restore);
    bool sixel = terminal_query_sixel();

    /* Clamp the board to what fits the terminal now, fold the effective
       (CLI-overridden) parameters back into settings, and on first run create
       the settings file so it exists for next time. */
    int max_w, max_h;
    fit_limits(sixel, &max_w, &max_h);
    settings.width = clamp_dim(opt.width, MIN_DIM, max_w);
    settings.height = clamp_dim(opt.height, MIN_DIM, max_h);
    settings.wrap = opt.wrap;
    settings.delay_ms = opt.delay_ms;
    settings.density = opt.density;
    opt.width = settings.width;
    opt.height = settings.height;
    if (!settings_existed) {
        settings_save(&settings); /* best effort */
    }

    srand(opt.seed_set ? opt.seed : (unsigned int)time(NULL));

    App app = {0};
    app.mode = UI_NORMAL;
    app.sim = SIM_STOPPED;
    app.selected = BTN_START;
    app.delay_ms = opt.delay_ms;
    app.running = true;
    app.cursor_x = opt.width / 2;
    app.cursor_y = opt.height / 2;
    app.blink_on = true;
    app.pending_w = opt.width;
    app.pending_h = opt.height;
    app.wrap = opt.wrap;
    app.pending_wrap = opt.wrap;
    app.max_w = max_w;
    app.max_h = max_h;
    app.sixel = sixel;
    app.settings = &settings;

    if (!board_init(&app.initial, opt.width, opt.height) ||
        !board_init(&app.a, opt.width, opt.height) ||
        !board_init(&app.b, opt.width, opt.height)) {
        fprintf(stderr, "Failed to allocate board\n");
        return 1;
    }
    app.cur = &app.a;
    app.next = &app.b;

    /* Restore the terminal on termination, and refit on window resize. No
       SA_RESTART on SIGWINCH so it interrupts poll() and wakes the loop. */
    struct sigaction sa_term = {0};
    sa_term.sa_handler = on_term;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);
    struct sigaction sa_winch = {0};
    sa_winch.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa_winch, NULL);

    if (!build_initial(&app.initial, &opt)) {
        board_free(&app.initial);
        board_free(&app.a);
        board_free(&app.b);
        return 1;
    }
    board_copy(app.cur, &app.initial);

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

        /* A window resize (SIGWINCH interrupts the poll above) refits the board
           before we redraw. */
        if (g_resized) {
            g_resized = 0;
            handle_winch(&app);
        }

        if (app.mode == UI_EDIT) {
            /* Toggle the blink on timeout; keep the marker solid while the user
               is actively pressing keys. */
            app.blink_on = (key == KEY_NONE) ? !app.blink_on : true;
        }

        switch (app.mode) {
            case UI_EDIT:   handle_edit(&app, key);   break;
            case UI_CANVAS: handle_canvas(&app, key); break;
            default:        handle_normal(&app, key); break;
        }

        if (app.running) {
            render(&app);
        }
    }

    printf(ANSI_SHOW_CURSOR);
    fflush(stdout);
    terminal_restore();

    board_free(&app.initial);
    board_free(&app.a);
    board_free(&app.b);
    return 0;
}
