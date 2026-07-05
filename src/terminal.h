#ifndef GAME_OF_LIFE_TERMINAL_H
#define GAME_OF_LIFE_TERMINAL_H

#include <stdbool.h>

/* Logical key events produced by the input layer. */
typedef enum {
    KEY_NONE = 0, /* no input was available within the poll timeout */
    KEY_TAB,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_ENTER,
    KEY_SPACE,
    KEY_ESC,
    KEY_QUIT, /* 'q' or Ctrl-C */
    KEY_OTHER
} Key;

/* Put the terminal into raw mode (no echo, no line buffering) and hide the
   cursor. Returns false if stdin/stdout is not a terminal. The previous
   settings are remembered and restored by terminal_restore(). */
bool terminal_init(void);

/* Restore the terminal to the state captured by terminal_init(). Safe to call
   multiple times and from a signal handler. */
void terminal_restore(void);

/* Wait up to timeout_ms for a key. Returns KEY_NONE if the timeout elapses
   with no input. A negative timeout blocks indefinitely. */
Key terminal_read_key(int timeout_ms);

/* Query the terminal window size in character cells. On success writes the
   column and row counts and returns true; returns false if the size is not
   available (e.g. output is not a terminal). */
bool terminal_size(int *cols, int *rows);

/* Query the terminal window size in pixels (ws_xpixel/ws_ypixel). Needed to
   place and scale sixel graphics. On success writes the pixel width/height and
   returns true; returns false if the terminal does not report a pixel size
   (many do not) or the values are zero. */
bool terminal_pixel_size(int *xpx, int *ypx);

/* Detect whether the terminal understands sixel graphics. Sends a Primary
   Device Attributes query and looks for attribute 4 in the reply, with a short
   timeout. Must be called while in raw mode (after terminal_init) so the reply
   is not echoed or line-buffered. The environment variable GOL_SIXEL overrides
   detection: "0" forces off, "1" forces on (no query is sent). */
bool terminal_query_sixel(void);

#endif /* GAME_OF_LIFE_TERMINAL_H */
