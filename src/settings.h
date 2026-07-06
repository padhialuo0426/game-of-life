#ifndef GAME_OF_LIFE_SETTINGS_H
#define GAME_OF_LIFE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

/* Persistent user settings, stored as JSON in the config directory. These are
   the parameters remembered across runs (board size, world type, etc.). */
typedef struct {
    int width;
    int height;
    bool wrap;      /* legacy: true = toroidal (kept for backward compatibility) */
    int world;      /* 0 = finite, 1 = toroidal, 2 = infinite */
    int delay_ms;
    double density; /* initial random density when no pattern is loaded */
} Settings;

/* Fill `s` with the built-in defaults. */
void settings_defaults(Settings *s);

/* Write the config directory path ($XDG_CONFIG_HOME or ~/.config, plus
   "/game-of-life") into `buf`. Returns false if neither env var is set. */
bool settings_config_dir(char *buf, size_t cap);

/* Write the user-data directory ($XDG_DATA_HOME or ~/.local/share, plus
   "/game-of-life") into `buf`. Saved patterns live under here, kept separate
   from the config directory. Returns false if neither env var is set. */
bool settings_data_dir(char *buf, size_t cap);

/* Write the saves directory (<data-dir>/saves) into `buf`. This is where the
   in-app Save/Load browse and where the default pattern (default.rle) lives.
   Returns false if the data directory cannot be determined. */
bool settings_saves_dir(char *buf, size_t cap);

/* Create `path` and any missing parent directories (like `mkdir -p`, mode 0755).
   Returns true on success (or if it already exists). */
bool settings_mkdirs(const char *path);

/* Write the full path to settings.json into `buf`. Returns false if the config
   directory cannot be determined. */
bool settings_file_path(char *buf, size_t cap);

/* Load settings.json, overwriting the fields of `s` that are present in the
   file. Returns true if the file was read, false if it is missing/unreadable
   (in which case `s` is left unchanged). */
bool settings_load(Settings *s);

/* Write `s` to settings.json, creating the config directory if needed.
   Returns true on success. */
bool settings_save(const Settings *s);

#endif /* GAME_OF_LIFE_SETTINGS_H */
