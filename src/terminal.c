#define _DEFAULT_SOURCE /* termios, poll, ioctl/TIOCGWINSZ, struct winsize */

#include "terminal.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static bool g_raw_active = false;
static int g_last_char = 0; /* raw byte behind the most recent KEY_OTHER */
static MouseEvent g_mouse;  /* details behind the most recent KEY_MOUSE */

/* Enable/disable mouse reporting: button events (1000), motion while a button
   is held so drags are reported (1002), and SGR extended coordinates (1006). */
#define MOUSE_ON  "\033[?1000;1002;1006h"
#define MOUSE_OFF "\033[?1000;1002;1006l"

/* Alternate screen buffer (as used by vim/less/htop). It has no scrollback, so
   sixel images that scroll are discarded instead of piling up in the terminal's
   scrollback (where iTerm2 in particular retains their bitmaps until closed);
   leaving it restores the user's original screen untouched. */
#define ALT_SCREEN_ON  "\033[?1049h"
#define ALT_SCREEN_OFF "\033[?1049l"

/* Full terminal setup/teardown written on enter/leave of raw mode. */
#define TERM_SETUP   ALT_SCREEN_ON MOUSE_ON
#define TERM_CLEANUP MOUSE_OFF ALT_SCREEN_OFF

bool terminal_init(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return false;
    }
    struct termios raw = g_saved;
    /* Disable canonical mode and echo so keys arrive immediately and are not
       printed. Keep signal generation (ISIG) off so we handle Ctrl-C ourselves
       via the read loop. */
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;  /* read() may return with 0 bytes ... */
    raw.c_cc[VTIME] = 0; /* ... immediately; we block via poll() instead. */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return false;
    }
    ssize_t w = write(STDOUT_FILENO, TERM_SETUP, sizeof(TERM_SETUP) - 1);
    (void)w;
    g_raw_active = true;
    return true;
}

void terminal_restore(void) {
    if (g_raw_active) {
        /* Turn off mouse reporting and leave the alternate screen (restoring the
           user's original screen) before restoring the terminal, so the shell is
           not left receiving mouse escapes or stuck on the alt buffer. */
        ssize_t w = write(STDOUT_FILENO, TERM_CLEANUP, sizeof(TERM_CLEANUP) - 1);
        (void)w;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw_active = false;
    }
}

/* Read one byte, waiting up to a few ms for it (escape-sequence bytes arrive
   as a burst but not necessarily in the same read as the ESC). Returns false
   if no byte becomes available in time. */
static bool read_byte_timed(char *out) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 20) <= 0) {
        return false;
    }
    return read(STDIN_FILENO, out, 1) == 1;
}

/* Parse an SGR mouse report, with "ESC [ <" already consumed. The remaining
   bytes are "b;x;y" followed by 'M' (press/drag) or 'm' (release). Fills
   g_mouse and returns KEY_MOUSE, or KEY_NONE on a malformed sequence. */
static Key parse_mouse_sgr(void) {
    int val[3] = {0, 0, 0};
    int idx = 0;
    char c;
    for (;;) {
        if (!read_byte_timed(&c)) return KEY_NONE;
        if (c >= '0' && c <= '9') {
            val[idx] = val[idx] * 10 + (c - '0');
        } else if (c == ';') {
            if (idx < 2) idx++;
        } else if (c == 'M' || c == 'm') {
            int b = val[0];
            g_mouse.x = val[1] - 1; /* SGR coordinates are 1-based */
            g_mouse.y = val[2] - 1;
            g_mouse.motion = (b & 0x20) != 0;
            g_mouse.pressed = (c == 'M');
            g_mouse.button = (b & 0x40) ? (64 + (b & 0x01)) /* wheel */
                                        : (b & 0x03);        /* button */
            return KEY_MOUSE;
        } else {
            return KEY_NONE; /* unexpected byte */
        }
    }
}

/* Decode an escape sequence that has already had its leading ESC consumed.
   Reads the remaining bytes and maps arrow keys and mouse reports. A lone ESC
   (no continuation) is reported as KEY_ESC. */
static Key decode_escape(void) {
    char c0;
    if (!read_byte_timed(&c0)) {
        return KEY_ESC; /* bare Escape press */
    }
    if (c0 != '[' && c0 != 'O') {
        return KEY_OTHER;
    }
    char c1;
    if (!read_byte_timed(&c1)) {
        return KEY_OTHER;
    }
    if (c0 == '[' && c1 == '<') {
        return parse_mouse_sgr();
    }
    switch (c1) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:  return KEY_OTHER;
    }
}

Key terminal_read_key(int timeout_ms) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    int rv = poll(&pfd, 1, timeout_ms);
    if (rv <= 0) {
        return KEY_NONE; /* timeout (0) or interrupted/error (<0) */
    }

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KEY_NONE;
    }

    /* Record the raw byte for every key, so terminal_char() can recover it even
       for keys that map to a named Key (e.g. 'q' behind KEY_QUIT) — text-entry
       modes need the literal character. */
    g_last_char = (unsigned char)c;

    switch (c) {
        case '\t':   return KEY_TAB;
        case '\r':   /* fall through */
        case '\n':   return KEY_ENTER;
        case ' ':    return KEY_SPACE;
        case 0x7f:   /* DEL */
        case 0x08:   return KEY_BACKSPACE;
        case 'q':
        case 'Q':
        case 0x03:   return KEY_QUIT; /* Ctrl-C */
        case 0x1b:   return decode_escape();
        default:     return KEY_OTHER;
    }
}

int terminal_char(void) {
    return g_last_char;
}

MouseEvent terminal_mouse(void) {
    return g_mouse;
}

bool terminal_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        return false;
    }
    if (ws.ws_col == 0 || ws.ws_row == 0) {
        return false;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return true;
}

bool terminal_pixel_size(int *xpx, int *ypx) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        return false;
    }
    if (ws.ws_xpixel == 0 || ws.ws_ypixel == 0) {
        return false; /* terminal does not report a pixel size */
    }
    *xpx = ws.ws_xpixel;
    *ypx = ws.ws_ypixel;
    return true;
}

/* Return true if `resp` (a Primary DA reply like "\033[?62;4;9c") advertises
   attribute 4 (sixel). Scans the ';'-separated numeric tokens for an exact 4. */
static bool da_has_sixel(const char *resp) {
    const char *p = resp;
    while (*p != '\0') {
        if (*p >= '0' && *p <= '9') {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (v == 4) return true;
            p = end; /* advance past the number we just consumed */
        } else {
            p++;
        }
    }
    return false;
}

bool terminal_query_sixel(void) {
    const char *env = getenv("GOL_SIXEL");
    if (env != NULL) {
        if (strcmp(env, "0") == 0) return false;
        if (strcmp(env, "1") == 0) return true;
        /* Any other value falls through to live detection. */
    }

    /* Ask the terminal for its Primary Device Attributes and read the reply. */
    if (write(STDOUT_FILENO, "\033[c", 3) != 3) {
        return false;
    }
    char buf[64];
    size_t n = 0;
    while (n < sizeof(buf) - 1) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 200) <= 0) {
            break; /* no (more) response in time */
        }
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            break;
        }
        buf[n++] = c;
        if (c == 'c') {
            break; /* end of the DA reply */
        }
    }
    buf[n] = '\0';
    return da_has_sixel(buf);
}
