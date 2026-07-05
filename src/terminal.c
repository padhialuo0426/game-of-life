#define _DEFAULT_SOURCE /* termios, poll, ioctl/TIOCGWINSZ, struct winsize */

#include "terminal.h"

#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static bool g_raw_active = false;

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
    g_raw_active = true;
    return true;
}

void terminal_restore(void) {
    if (g_raw_active) {
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

/* Decode an escape sequence that has already had its leading ESC consumed.
   Reads the remaining bytes and maps arrow keys. A lone ESC (no continuation)
   is reported as KEY_ESC. */
static Key decode_escape(void) {
    char seq[2];
    if (!read_byte_timed(&seq[0])) {
        return KEY_ESC; /* bare Escape press */
    }
    if (seq[0] != '[' && seq[0] != 'O') {
        return KEY_OTHER;
    }
    if (!read_byte_timed(&seq[1])) {
        return KEY_OTHER;
    }
    switch (seq[1]) {
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

    switch (c) {
        case '\t':   return KEY_TAB;
        case '\r':   /* fall through */
        case '\n':   return KEY_ENTER;
        case ' ':    return KEY_SPACE;
        case 'q':
        case 'Q':
        case 0x03:   return KEY_QUIT; /* Ctrl-C */
        case 0x1b:   return decode_escape();
        default:     return KEY_OTHER;
    }
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
