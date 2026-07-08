#ifndef GAME_OF_LIFE_CONFIG_H
#define GAME_OF_LIFE_CONFIG_H

#include "core/board.h"

/* Load an initial configuration from a plaintext pattern file into `board`.

   File format (a subset of the classic ".cells" plaintext format):
     - Lines beginning with '!' or '#' are comments and ignored.
     - In pattern lines, '.' or space means a dead cell; any other printable
       character (typically 'O' or '*') means a live cell.
     - The pattern is centered inside the already-initialised `board`. Cells of
       the pattern that fall outside the board are clipped.

   The board is cleared first. Returns true on success. On failure, `errbuf`
   (if non-NULL) receives a human-readable message. */
bool config_load_file(Board *board, const char *path, char *errbuf, size_t errbuf_size);

#endif /* GAME_OF_LIFE_CONFIG_H */
