#ifndef GAME_OF_LIFE_KITTY_H
#define GAME_OF_LIFE_KITTY_H

#include <stddef.h>

/* Incremental Kitty Graphics Protocol canvas — the KGP counterpart to
   SixelCanvas (see sixel.h). A cols x rows grid of cells (each cell_px x cell_px
   pixels) that starts all-dead with faint grid lines between cells, and that you
   plot live cells and an optional cursor onto before encoding. Uses KGP with
   zlib-compressed 24-bit RGB. The API surface mirrors SixelCanvas so the renderer
   can dispatch at a handful of call sites. */
typedef struct KittyCanvas KittyCanvas;

/* Create a canvas of cols x rows cells at cell_px screen-pixels each: all cells
   dead (near-black), with 1-pixel grid lines between cells when cell_px >= 5.
   Returns NULL on bad arguments or allocation failure. */
KittyCanvas *kitty_canvas_new(int cols, int rows, int cell_px);

/* Mark the cell at (col, row) alive (near-white); coordinates outside the canvas
   are silently ignored. */
void kitty_canvas_set_alive(KittyCanvas *canvas, int col, int row);

/* Outline the cell at (col, row) in the cursor colour; out-of-range ignored.
   Drawn during encode() — the outline pixels are saved/restored so the canvas
   stays valid for re-encode. */
void kitty_canvas_set_cursor(KittyCanvas *canvas, int col, int row);

/* Encode the canvas as a KGP escape sequence (APC … ST). The payload is
   zlib-compressed 24-bit RGB, base64-encoded. Returns a newly malloc'd buffer
   (with a trailing NUL for convenience, not counted in *out_len) and writes its
   length to *out_len. Returns NULL on allocation failure. The caller frees. */
char *kitty_canvas_encode(KittyCanvas *canvas, size_t *out_len);

/* Release a canvas created by kitty_canvas_new. */
void kitty_canvas_free(KittyCanvas *canvas);

#endif /* GAME_OF_LIFE_KITTY_H */
