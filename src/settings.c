#define _POSIX_C_SOURCE 200809L /* mkdir, getenv semantics */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void settings_defaults(Settings *s) {
    s->width = 30;
    s->height = 20;
    s->wrap = false;
    s->world = 0; /* finite */
    s->delay_ms = 120;
    s->density = 0.25;
}

bool settings_config_dir(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(buf, cap, "%s/game-of-life", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, cap, "%s/.config/game-of-life", home);
        return true;
    }
    return false;
}

bool settings_data_dir(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(buf, cap, "%s/game-of-life", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, cap, "%s/.local/share/game-of-life", home);
        return true;
    }
    return false;
}

bool settings_saves_dir(char *buf, size_t cap) {
    char dir[768];
    if (!settings_data_dir(dir, sizeof(dir))) {
        return false;
    }
    snprintf(buf, cap, "%s/saves", dir);
    return true;
}

bool settings_file_path(char *buf, size_t cap) {
    char dir[512];
    if (!settings_config_dir(dir, sizeof(dir))) {
        return false;
    }
    snprintf(buf, cap, "%s/settings.json", dir);
    return true;
}

/* Create `path` and any missing parent directories (like `mkdir -p`). */
static bool mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool settings_mkdirs(const char *path) {
    return mkdir_p(path);
}

/* Locate the value token following "key": in a flat JSON object. Returns a
   pointer just past the colon/whitespace, or NULL if the key is absent. This
   is a deliberately small reader for the fixed-shape file we write ourselves,
   not a general JSON parser. */
static const char *find_value(const char *text, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
        p++;
    }
    return p;
}

bool settings_load(Settings *s) {
    char path[1024];
    if (!settings_file_path(path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    const char *p;
    if ((p = find_value(buf, "width")) != NULL) {
        long v = strtol(p, NULL, 10);
        if (v > 0) s->width = (int)v;
    }
    if ((p = find_value(buf, "height")) != NULL) {
        long v = strtol(p, NULL, 10);
        if (v > 0) s->height = (int)v;
    }
    if ((p = find_value(buf, "wrap")) != NULL) {
        s->wrap = (strncmp(p, "true", 4) == 0);
        s->world = s->wrap ? 1 : 0; /* legacy fallback if "world" is absent */
    }
    if ((p = find_value(buf, "world")) != NULL) {
        long v = strtol(p, NULL, 10);
        if (v >= 0 && v <= 2) s->world = (int)v;
    }
    if ((p = find_value(buf, "delay_ms")) != NULL) {
        long v = strtol(p, NULL, 10);
        if (v >= 0) s->delay_ms = (int)v;
    }
    if ((p = find_value(buf, "density")) != NULL) {
        double v = strtod(p, NULL);
        if (v >= 0.0 && v <= 1.0) s->density = v;
    }
    return true;
}

bool settings_save(const Settings *s) {
    char dir[512];
    char path[1024];
    if (!settings_config_dir(dir, sizeof(dir))) {
        return false;
    }
    if (!mkdir_p(dir)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f,
            "{\n"
            "  \"width\": %d,\n"
            "  \"height\": %d,\n"
            "  \"wrap\": %s,\n"
            "  \"world\": %d,\n"
            "  \"delay_ms\": %d,\n"
            "  \"density\": %.3f\n"
            "}\n",
            s->width, s->height, s->world == 1 ? "true" : "false", s->world,
            s->delay_ms, s->density);
    fclose(f);
    return true;
}
