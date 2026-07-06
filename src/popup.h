#ifndef POPUP_H
#define POPUP_H

#include <stdbool.h>
#include <time.h>

/* The UI has three kinds of "popup" — overlays that float on top of the world —
   which differ only in *when they close*:
     - STICKY : never closes on its own. The status HUD; redrawn every frame while
                in world view.
     - TIMED  : closes after a fixed delay. This Popup toast (POPUP_TTL_MS).
     - MODAL  : closes when the user finishes or cancels. The Save/Load/Confirm
                windows.
   All three share one rendering primitive (overlay_box in main.c, which draws the
   bg-filled bordered box over the world). Only the TIMED kind needs the timer
   bookkeeping below, so that is all this module owns.

   A transient "toast": a short message that floats over the world for a fixed
   time, then auto-hides on its own. Used for Save/Load results, which previously
   lingered forever when the user drove the UI with the mouse only. */
#define POPUP_TTL_MS 5000

typedef struct {
    char text[320];
    struct timespec shown;   /* CLOCK_MONOTONIC stamp of when it was shown */
    bool active;
} Popup;

/* Show a printf-style message, starting its TTL now. */
void popup_show(Popup *p, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* True while active and still within POPUP_TTL_MS of being shown. */
bool popup_visible(const Popup *p);

/* If the popup has outlived its TTL, clear it and return true (so the caller can
   force a world repaint to erase where it was drawn). Otherwise return false. */
bool popup_expire(Popup *p);

/* Milliseconds until this popup expires, clamped to >= 0; 0 when inactive or
   already expired. Lets the main loop wake in time to auto-hide an idle popup. */
long popup_remaining_ms(const Popup *p);

/* Hide immediately. */
void popup_clear(Popup *p);

#endif /* POPUP_H */
