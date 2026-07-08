#include "kitty.h"
#include "sixel.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal smoke-test harness for the KGP canvas and the dual-protocol
   dispatch path. */
static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

static void test_kitty_basic(void) {
    KittyCanvas *kc = kitty_canvas_new(5, 3, 4);
    CHECK(kc != NULL, "kitty_canvas_new(5,3,4)");
    if (kc == NULL) return;

    kitty_canvas_set_alive(kc, 2, 1); /* middle cell */
    kitty_canvas_set_cursor(kc, 3, 2);

    size_t len = 0;
    char *img = kitty_canvas_encode(kc, &len);
    CHECK(img != NULL, "kitty_canvas_encode non-NULL");
    CHECK(len > 30, "encoded length non-trivial");
    if (img == NULL) { kitty_canvas_free(kc); return; }

    /* KGP escape looks like: ESC _ G a=T,f=24,s=20,v=12,o=z,C=1;<b64> ESC \ */
    CHECK(img[0] == '\033' && img[1] == '_' && img[2] == 'G',
          "starts with APC G (ESC _ G)");
    CHECK(strstr(img, "a=T") != NULL, "contains a=T");
    CHECK(strstr(img, "f=24") != NULL, "contains f=24");
    CHECK(strstr(img, "s=20") != NULL, "contains s=20 (5*4 px)");
    CHECK(strstr(img, "v=12") != NULL, "contains v=12 (3*4 px)");
    CHECK(strstr(img, "o=z") != NULL, "contains o=z (zlib compression)");
    CHECK(strstr(img, "z=-1") != NULL, "contains z=-1 (behind text)");
    CHECK(strstr(img, "C=1") != NULL, "contains C=1 (don't move cursor)");

    /* Check that it ends with ESC \ . */
    CHECK(len >= 2 && img[len-2] == '\033' && img[len-1] == '\\',
          "ends with ST (ESC \\)");

    /* The payload between ';' and ST should be base64 (printable / = / + / /). */
    char *semi = strchr(img, ';');
    CHECK(semi != NULL, "header ends with ';'");
    if (semi != NULL) {
        for (char *p = semi + 1; p < img + len - 2; p++) {
            if (!(isprint((unsigned char)*p) || *p == '=' || *p == '+' || *p == '/')) {
                fprintf(stderr, "FAIL: non-base64 byte 0x%02x in payload\n",
                        (unsigned char)*p);
                failures++;
                break;
            }
        }
    }

    free(img);
    kitty_canvas_free(kc);
}

static void test_kitty_empty(void) {
    KittyCanvas *kc = kitty_canvas_new(1, 1, 10);
    CHECK(kc != NULL, "1x1 canvas");
    if (kc == NULL) return;
    size_t len = 0;
    char *img = kitty_canvas_encode(kc, &len);
    CHECK(img != NULL, "empty encode non-NULL");
    /* A 1x1 all-dead cell should compress to very little; even so the b64
       payload plus header should be well under 2 kB. */
    CHECK(len < 2048, "empty 1x1 image under 2 kB");
    /* Verify no memory corruption: encode twice. */
    free(img);
    img = kitty_canvas_encode(kc, &len);
    CHECK(img != NULL, "re-encode after cursor restore OK");
    free(img);
    kitty_canvas_free(kc);
}

static void test_kitty_out_of_range(void) {
    KittyCanvas *kc = kitty_canvas_new(3, 3, 5);
    CHECK(kc != NULL, "3x3 canvas");
    if (kc == NULL) return;
    /* These must not crash. */
    kitty_canvas_set_alive(kc, -1, 0);
    kitty_canvas_set_alive(kc, 0, -1);
    kitty_canvas_set_alive(kc, 100, 100);
    kitty_canvas_set_cursor(kc, -1, -1);
    kitty_canvas_set_cursor(kc, 99, 99);
    kitty_canvas_set_alive(kc, 0, 0); /* valid */
    size_t len = 0;
    char *img = kitty_canvas_encode(kc, &len);
    CHECK(img != NULL, "encode after out-of-range sets");
    free(img);
    kitty_canvas_free(kc);
}

static void test_sixel_still_works(void) {
    /* The sixel path must still compile and link. */
    SixelCanvas *sc = sixel_canvas_new(4, 2, 6);
    CHECK(sc != NULL, "sixel_canvas_new still works");
    if (sc == NULL) return;
    sixel_canvas_set_alive(sc, 0, 0);
    sixel_canvas_set_cursor(sc, 1, 1);
    size_t len = 0;
    char *img = sixel_canvas_encode(sc, &len);
    CHECK(img != NULL, "sixel encode still works");
    CHECK(len > 10, "sixel image non-trivial");
    free(img);
    sixel_canvas_free(sc);
}

int main(void) {
    test_kitty_basic();
    test_kitty_empty();
    test_kitty_out_of_range();
    test_sixel_still_works();
    fprintf(stderr, "\n%d failures\n", failures);
    return failures ? 1 : 0;
}
