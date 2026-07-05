#ifndef GAME_OF_LIFE_SIXEL_H
#define GAME_OF_LIFE_SIXEL_H

#include <stdbool.h>
#include <stddef.h>

#include "board.h"

/* Encode `board` as a sixel graphics image. Each cell is drawn as a
   cell_px x cell_px block of pixels (so the world's size is decoupled from the
   terminal's character grid). When cell_px is large enough, thin grid lines are
   drawn between cells.

   If (cursor_x, cursor_y) is inside the board and cursor_on is true, that cell
   is outlined in the cursor colour (used as the blinking edit-mode marker);
   pass a negative coordinate to draw no cursor.

   Returns a newly malloc'd buffer holding the raw sixel byte stream (an extra
   NUL is appended for convenience but is not counted) and writes its length to
   *out_len. Returns NULL on allocation failure. The caller frees the buffer. */
char *sixel_render_board(const Board *board, int cell_px,
                         int cursor_x, int cursor_y, bool cursor_on,
                         size_t *out_len);

#endif /* GAME_OF_LIFE_SIXEL_H */
