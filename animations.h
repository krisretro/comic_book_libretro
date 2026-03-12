#ifndef ANIMATIONS_H
#define ANIMATIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ANIM_NONE, ANIM_NEXT, ANIM_PREV } AnimDir;
typedef enum { MODE_NONE, MODE_CURL, MODE_PUSH, MODE_COVER } AnimMode;

static int anim_frame = 0;
static const int ANIM_DURATION = 30;
static bool is_animating = false;

static AnimDir anim_dir = ANIM_NONE;
static AnimMode anim_mode = MODE_CURL;

static uint16_t *old_page = NULL;   /* page sliding out */
static uint16_t *new_page = NULL;   /* page sliding in */

static int anim_w = 0, anim_h = 0;

/* -------------------------------------------------- */
/* RGB565 helpers                                     */
/* -------------------------------------------------- */

#define R565(p) (((p) >> 11) & 0x1F)
#define G565(p) (((p) >> 5)  & 0x3F)
#define B565(p) ((p) & 0x1F)
#define RGB565(r,g,b) (((r)<<11)|((g)<<5)|(b))

static inline uint16_t darken(uint16_t p, float f)
{
    return RGB565(
        (int)(R565(p) * f),
        (int)(G565(p) * f),
        (int)(B565(p) * f)
    );
}

/* -------------------------------------------------- */

static inline bool anim_active(void)
{
    return is_animating;
}

static inline bool anim_finished(void)
{
    return (!is_animating && anim_frame >= ANIM_DURATION);
}

/* -------------------------------------------------- */

void init_animations(int w, int h)
{
    anim_w = w;
    anim_h = h;

    free(old_page);
    free(new_page);

    old_page = calloc(w * h, sizeof(uint16_t));
    new_page = calloc(w * h, sizeof(uint16_t));
}

/* -------------------------------------------------- */
/* IMPORTANT:                                         */
/* old_fb = framebuffer AFTER drawing CURRENT page    */
/* new_fb = framebuffer AFTER drawing TARGET page     */
/* -------------------------------------------------- */

void start_page_turn(
    uint16_t *old_fb,
    uint16_t *new_fb,
    int w, int h,
    bool next,
    int mode
)
{
    if (!old_page || anim_w != w || anim_h != h)
        init_animations(w, h);

    memcpy(old_page, old_fb, w * h * 2);
    memcpy(new_page, new_fb, w * h * 2);

    anim_frame = 0;
    is_animating = true;
    anim_dir = next ? ANIM_NEXT : ANIM_PREV;
    anim_mode = (AnimMode)mode;
}

/* -------------------------------------------------- */

void render_animation(uint16_t *out, int w, int h)
{
    if (!is_animating)
        return;

    float t = (float)anim_frame / (float)ANIM_DURATION;
    float p = sinf(t * 1.570796f); /* smooth ease-out */

    int dir = (anim_dir == ANIM_NEXT) ? 1 : -1;
    int shift = (int)(p * w);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t px;

            /* ---------------- PUSH ---------------- */
            if (anim_mode == MODE_PUSH) {
                int ox = x + dir * shift;
                int nx = ox - dir * w;

                if (ox >= 0 && ox < w)
                    px = old_page[y * w + ox];
                else if (nx >= 0 && nx < w)
                    px = new_page[y * w + nx];
                else
                    px = 0;
            }

            /* ---------------- COVER ---------------- */
            else if (anim_mode == MODE_COVER) {
                /* Next: New page slides in from right to left, covering old page */
                /* Prev: New page slides in from left to right, covering old page */
                int edge = (anim_dir == ANIM_NEXT) ? (w - shift) : shift;

                if (anim_dir == ANIM_NEXT) {
                    if (x < edge) {
                        px = old_page[y * w + x];
                    } else {
                        px = new_page[y * w + (x - edge)];
                    }
                } else {
                    /* Going Backwards */
                    if (x >= edge) {
                        px = old_page[y * w + x];
                    } else {
                        /* Map x directly to the new_page pixels sliding in */
                        int nx = x + (w - edge); 
                        px = (nx >= 0 && nx < w) ? new_page[y * w + nx] : 0;
                    }
                }
            }

            /* ---------------- CURL ---------------- */
            else {
                /* The 'fold' is the moving vertical line of the curl */
                float fold = (anim_dir == ANIM_NEXT)
                           ? w - p * (w * 1.2f)
                           : p * (w * 1.2f);

                float d = x - fold;
                float r = w / 8.0f;

                /* Logic: If we are 'outside' the fold area, show the static pages */
                if ((anim_dir == ANIM_NEXT && x < fold) || (anim_dir == ANIM_PREV && x > fold)) {
                    /* This is the area NOT yet reached by the turn */
                    px = old_page[y * w + x];
                }
                else if (fabsf(d) < r * 3.1415f) {
                    /* This is the actual curl/fold effect */
                    float a = fabsf(d) / r;
                    int sx = (int)(fold + dir * r * sinf(a));

                    if (sx >= 0 && sx < w)
                        px = darken(old_page[y * w + sx], 0.7f + 0.3f * cosf(a));
                    else
                        px = new_page[y * w + x];
                }
                else {
                    /* This is the area revealed 'under' the turn */
                    px = new_page[y * w + x];
                }
            }

            out[y * w + x] = px;
        }
    }

    anim_frame++;
    if (anim_frame >= ANIM_DURATION) {
        is_animating = false;
    }
}

#endif
