/* SPDX-License-Identifier: MPL-2.0 */
/* A three-band crossover for the isolator and for effects limited to some
 * bands, like a DJM's LOW / MID / HI Beat FX buttons.
 *
 * Linkwitz-Riley, fourth order, at two frequencies: each band is two cascaded
 * Butterworth sections, and the low band also passes the upper crossover's
 * allpass so all three share one phase. Their sum is then an allpass of the
 * input: flat in level, so turning every band back to unity is not a coloured
 * copy of the track, only a phase-shifted one. A caller that needs the input
 * bit for bit bypasses the split rather than summing it back.
 *
 * Header-only, libc-free, no allocation; one splitter per stereo signal.
 */
#ifndef FX_BANDS_H
#define FX_BANDS_H
#include "rx3_fx_color.h"

/* Where the bands meet. Isolators sit roughly here; a kick lives below the
   first and a hi-hat above the second. */
#define FX_BANDS_LOW_MID  250.0f
#define FX_BANDS_MID_HIGH 2500.0f

struct fx_bands {
    struct fx_svf low[2][2], high[2][2], mid[2][2], top[2][2], phase[2];
    struct fx_svf_coef first, second;
};

struct fx_band_frame { struct fx_frame low, mid, high; };

static inline void fx_bands_init(struct fx_bands *bands, float rate)
{
    for (unsigned int i = 0; i < sizeof(*bands); i++) ((unsigned char *)bands)[i] = 0;
    fx_svf_design(&bands->first, FX_BANDS_LOW_MID, 0.70710678f, rate);
    fx_svf_design(&bands->second, FX_BANDS_MID_HIGH, 0.70710678f, rate);
}

static inline void fx_bands_reset(struct fx_bands *bands)
{
    struct fx_svf_coef first = bands->first, second = bands->second;
    for (unsigned int i = 0; i < sizeof(*bands); i++) ((unsigned char *)bands)[i] = 0;
    bands->first = first;
    bands->second = second;
}

/* One sample of one channel into its three bands. */
static inline void fx_bands_channel(struct fx_bands *b, unsigned int c, float x,
                                    float *low, float *mid, float *high)
{
    float l = fx_svf_run(&b->low[c][1], &b->first, fx_svf_run(&b->low[c][0], &b->first, x, 0u), 0u);
    float h = fx_svf_run(&b->high[c][1], &b->first, fx_svf_run(&b->high[c][0], &b->first, x, 2u), 2u);
    *mid = fx_svf_run(&b->mid[c][1], &b->second, fx_svf_run(&b->mid[c][0], &b->second, h, 0u), 0u);
    *high = fx_svf_run(&b->top[c][1], &b->second, fx_svf_run(&b->top[c][0], &b->second, h, 2u), 2u);
    /* The second crossover's allpass, low + high - k * band, on the low band. */
    float band = fx_svf_run(&b->phase[c], &b->second, l, 1u);
    *low = l - 2.0f * b->second.k * band;
}

static inline void fx_bands_split(struct fx_bands *bands, struct fx_frame in, struct fx_band_frame *out)
{
    fx_bands_channel(bands, 0u, in.left, &out->low.left, &out->mid.left, &out->high.left);
    fx_bands_channel(bands, 1u, in.right, &out->low.right, &out->mid.right, &out->high.right);
}

/* ------------------------------------------------- an effect on some bands */

/* Runs an effect on the selected bands only, and passes the others by.
 *
 * Splitting costs the input its bit-exactness (the bands sum to an allpass),
 * so a deck with every band selected is not split at all. Moving between the
 * two is a change of phase, which is only made while the caller says its
 * effect is idle, and then as a 5 ms crossfade between the plain and the
 * split signal, which are both dry at that moment. */
#define FX_BAND_FADE 220u
#define FX_BAND_CHUNK 256u
typedef void (*fx_band_effect)(void *context, struct fx_frame *io, unsigned int frames);

struct fx_band_insert {
    struct fx_bands bands;
    unsigned int mask;          /* in use: RX3 band bits, 7 = every band */
    unsigned int split;         /* the split path is the target */
    unsigned int fade;          /* frames left in a change of path */
    struct fx_frame selected[FX_BAND_CHUNK], rest[FX_BAND_CHUNK], plain[FX_BAND_CHUNK];
};

static inline void fx_band_insert_init(struct fx_band_insert *insert, float rate)
{
    for (unsigned int i = 0; i < sizeof(*insert); i++) ((unsigned char *)insert)[i] = 0;
    fx_bands_init(&insert->bands, rate);
    insert->mask = 7u;
}

static inline void fx_band_insert_process(struct fx_band_insert *insert, struct fx_frame *io,
                                          unsigned int frames, unsigned int wanted, int idle,
                                          fx_band_effect effect, void *context)
{
    wanted &= 7u;
    if (!wanted) wanted = 7u;
    if (idle && wanted != insert->mask) {
        unsigned int split = wanted != 7u;
        if (split != insert->split) {
            if (split) fx_bands_reset(&insert->bands);
            insert->fade = FX_BAND_FADE;
        }
        insert->split = split;
        insert->mask = wanted;
    }
    if (!insert->split && !insert->fade) { effect(context, io, frames); return; }
    for (unsigned int done = 0; done < frames; ) {
        unsigned int n = frames - done < FX_BAND_CHUNK ? frames - done : FX_BAND_CHUNK;
        struct fx_frame *x = io + done;
        for (unsigned int i = 0; i < n; i++) {
            struct fx_band_frame b;
            fx_bands_split(&insert->bands, x[i], &b);
            struct fx_frame sel = {0.0f, 0.0f}, rest = {0.0f, 0.0f};
            const struct fx_frame *band[3] = {&b.low, &b.mid, &b.high};
            for (unsigned int k = 0; k < 3u; k++) {
                struct fx_frame *to = (insert->mask & (1u << k)) ? &sel : &rest;
                to->left += band[k]->left; to->right += band[k]->right;
            }
            insert->plain[i] = x[i];
            insert->selected[i] = sel;
            insert->rest[i] = rest;
        }
        effect(context, insert->selected, n);
        for (unsigned int i = 0; i < n; i++) {
            struct fx_frame split = {insert->selected[i].left + insert->rest[i].left,
                                     insert->selected[i].right + insert->rest[i].right};
            if (insert->fade) {
                /* Toward the split path when splitting, away from it if not. */
                float t = (float)insert->fade * (1.0f / (float)FX_BAND_FADE);
                float w = insert->split ? 1.0f - t : t;
                split.left = insert->plain[i].left + (split.left - insert->plain[i].left) * w;
                split.right = insert->plain[i].right + (split.right - insert->plain[i].right) * w;
                insert->fade--;
            }
            x[i] = split;
        }
        done += n;
    }
}

#endif /* FX_BANDS_H */
