/* SPDX-License-Identifier: MPL-2.0 */
/* Arithmetic the effect engines share. The hook links with -nostdlib and rbp's
 * libm is not one of its imports, so every curve here is a polynomial or a bit
 * manipulation, accurate to what an audio parameter needs and no further. No
 * state, no allocation; every function is safe on the audio thread.
 *
 * The same header compiles on the computer for the offline tests, which is
 * the point of keeping it free of player types.
 */
#ifndef FX_MATH_H
#define FX_MATH_H

#define FX_PI 3.14159265358979f

static inline float fx_absf(float x) { return x < 0.0f ? -x : x; }
static inline float fx_minf(float a, float b) { return a < b ? a : b; }
static inline float fx_maxf(float a, float b) { return a > b ? a : b; }
static inline float fx_clampf(float x, float lo, float hi)
{ return x < lo ? lo : (x > hi ? hi : x); }
static inline float fx_lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* 2^x for x in [-30, 30]. The exponent goes straight into the bits; the
 * fraction uses a cubic fitted on [0,1), worst error about 1e-4 relative,
 * which is 0.2 cents on a frequency. */
static inline float fx_exp2f(float x)
{
    x = fx_clampf(x, -30.0f, 30.0f);
    int whole = (int)x;
    if ((float)whole > x) whole--;
    float f = x - (float)whole;
    float p = 1.0f + f * (0.6960656421638072f +
                     f * (0.224494337302845f + f * 0.07944023841053369f));
    union { float f; unsigned int u; } bits;
    bits.f = p;
    bits.u += (unsigned int)whole << 23;
    return bits.f;
}

/* tan(x) for x in [0, 1.45]: the [3/2] Pade form, within 3% at the top of the
 * range. It only sets a filter's warped cutoff, where that is a few cents. */
static inline float fx_tanf(float x)
{
    float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

/* sin on a phase in [0,1): a parabola refined once, under 0.1% error. Used by
 * the LFOs, never on audio. */
static inline float fx_sin_turns(float phase)
{
    float x = phase - (float)(int)phase;
    x = x < 0.5f ? x : x - 1.0f;            /* [-0.5, 0.5) */
    float y = 8.0f * x - 16.0f * x * fx_absf(x);
    return 0.225f * (y * fx_absf(y) - y) + y;
}

/* A rational soft clip, transparent below about -12 dBFS and bounded by 1. */
static inline float fx_softclip(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Feedback paths decay into denormals on a quiet input, and the A9's VFP
 * runs those through a slow path. Below -300 dB is silence. */
static inline float fx_flush(float x)
{ return (x > 1e-15f || x < -1e-15f) ? x : 0.0f; }

static inline unsigned int fx_xorshift(unsigned int *state)
{
    unsigned int x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}
/* Uniform in [-1, 1). */
static inline float fx_noise(unsigned int *state)
{ return (float)(int)fx_xorshift(state) * (1.0f / 2147483648.0f); }

/* frames = value * per, without an integer divide (the A9 has none). */
static inline unsigned int fx_frames_of(float beats, float frames_per_beat)
{
    float frames = beats * frames_per_beat;
    return frames < 1.0f ? 1u : (unsigned int)(frames + 0.5f);
}

/* A linear ramp of a gain, advanced once per frame. */
struct fx_ramp { float value, target, step; };
static inline void fx_ramp_to(struct fx_ramp *r, float target, unsigned int frames)
{
    r->target = target;
    r->step = frames ? (target - r->value) / (float)frames : 0.0f;
    if (!frames) r->value = target;
}
static inline float fx_ramp_next(struct fx_ramp *r)
{
    if (r->step != 0.0f) {
        r->value += r->step;
        if ((r->step > 0.0f && r->value >= r->target) ||
            (r->step < 0.0f && r->value <= r->target)) {
            r->value = r->target;
            r->step = 0.0f;
        }
    }
    return r->value;
}
static inline int fx_ramp_idle(const struct fx_ramp *r) { return r->step == 0.0f; }

/* A one-pole smoother for a control a person moves: no zipper, ~5 ms. */
static inline float fx_smooth(float *state, float target, float coefficient)
{ *state += (target - *state) * coefficient; return *state; }

#endif /* FX_MATH_H */
