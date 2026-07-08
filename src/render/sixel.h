#ifndef GAME_OF_LIFE_SIXEL_H
#define GAME_OF_LIFE_SIXEL_H

#include <stdbool.h>
#include <stddef.h>

#include "core/board.h"

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

/* Incremental sixel canvas: a cols x rows grid of cells (each a
   cell_px x cell_px pixel block) that starts all-dead and that you plot live
   cells and an optional cursor onto before encoding. This lets a caller render
   directly from a sparse world — touching only the O(live) cells in view —
   instead of first materialising a dense board of every viewport cell. */
typedef struct SixelCanvas SixelCanvas;

/* Create a canvas of cols x rows cells at cell_px pixels each: all cells dead,
   with grid lines pre-drawn when cell_px is large enough. Returns NULL on bad
   arguments or allocation failure. */
SixelCanvas *sixel_canvas_new(int cols, int rows, int cell_px);

/* Mark the cell at (col, row) alive; coordinates outside the canvas are
   ignored (so callers can plot without clipping to the viewport first). */
void sixel_canvas_set_alive(SixelCanvas *canvas, int col, int row);

/* Outline the cell at (col, row) in the cursor colour (the edit-mode marker);
   out-of-range coordinates are ignored. */
void sixel_canvas_set_cursor(SixelCanvas *canvas, int col, int row);

/* Encode the canvas as a sixel byte stream (see sixel_render_board for the
   buffer contract). Returns NULL on allocation failure; the caller frees. */
char *sixel_canvas_encode(const SixelCanvas *canvas, size_t *out_len);

/* Release a canvas created by sixel_canvas_new. */
void sixel_canvas_free(SixelCanvas *canvas);

#endif /* GAME_OF_LIFE_SIXEL_H */
