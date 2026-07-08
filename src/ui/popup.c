#define _POSIX_C_SOURCE 200809L /* clock_gettime, CLOCK_MONOTONIC */

#include "ui/popup.h"

#include <stdarg.h>
#include <stdio.h>

/* Milliseconds elapsed since a monotonic timestamp. */
static long elapsed_ms(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L +
           (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

void popup_show(Popup *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->text, sizeof(p->text), fmt, ap);
    va_end(ap);
    clock_gettime(CLOCK_MONOTONIC, &p->shown);
    p->active = true;
}

bool popup_visible(const Popup *p) {
    return p->active && elapsed_ms(&p->shown) < POPUP_TTL_MS;
}

bool popup_expire(Popup *p) {
    if (p->active && elapsed_ms(&p->shown) >= POPUP_TTL_MS) {
        p->active = false;
        return true;
    }
    return false;
}

long popup_remaining_ms(const Popup *p) {
    if (!p->active) return 0;
    long left = POPUP_TTL_MS - elapsed_ms(&p->shown);
    return left > 0 ? left : 0;
}

void popup_clear(Popup *p) {
    p->active = false;
}
