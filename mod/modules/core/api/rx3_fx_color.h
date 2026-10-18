/* SPDX-License-Identifier: MPL-2.0 */
/* Software Sound Color FX.
 *
 * Six engines with the shape of the mixer's own: one bipolar COLOR control,
 * centre is bypass, and a PARAMETER control from 0 to 1. They are written
 * here from their audible description, not taken from the firmware, whose
 * implementation has not been read. Names follow the panel's buttons so a DJ
 * finds them where they expect:
 *
 *   FILTER    left low-pass, right high-pass; PARAMETER is resonance.
 *   DUB ECHO  a beat-synced echo, filtered inside its feedback (left darker,
 *             right thinner); PARAMETER is feedback. Tails survive the return
 *             to centre, as they do on the mixer.
 *   SPACE     a reverb whose return is filtered the same way; PARAMETER is
 *             the decay. Tails survive too.
 *   CRUSH     bit depth and sample-rate reduction on both sides, left dark,
 *             right thin; PARAMETER is the tone filter's weight.
 *   NOISE     filtered white noise over the music, low-passed to the left and
 *             high-passed to the right; PARAMETER is its level.
 *   SWEEP     left a beat-synced gate, right a band-pass sweep; PARAMETER is
 *             the gate rate or the band's width.
 *
 * Everything runs per frame on interleaved stereo float in [-1, 1], allocates
 * nothing, and touches only the state and the delay memory it was given.
 */
#ifndef FX_COLOR_H
#define FX_COLOR_H
#include "rx3_fx_math.h"

struct fx_frame { float left, right; };

enum fx_color_kind {
    FX_COLOR_FILTER, FX_COLOR_DUB_ECHO, FX_COLOR_SPACE,
    FX_COLOR_CRUSH, FX_COLOR_NOISE, FX_COLOR_SWEEP, FX_COLOR_KINDS
};

/* Below this distance from centre the control is at bypass, as a knob's
 * centre detent is. */
#define FX_COLOR_DEADZONE 0.03f
/* Coefficients follow the controls every this many frames. */
#define FX_COLOR_CONTROL_FRAMES 32u

/* Delay memory the caller provides, in stereo frames. One beat at 60 BPM and
 * 48 kHz covers the longest echo; the reverb needs well under that. */
#define FX_COLOR_DELAY_FRAMES 49152u

/* A state-variable filter in its trapezoidal form: stable under modulation,
 * which is what a filter swept by hand needs. One per channel. */
struct fx_svf { float ic1, ic2; };
struct fx_svf_coef { float a1, a2, a3, k; };

static inline void fx_svf_design(struct fx_svf_coef *c, float cutoff, float q, float rate)
{
    float w = FX_PI * fx_clampf(cutoff, 10.0f, 0.45f * rate) / rate;
    float g = fx_tanf(w);
    c->k = 1.0f / fx_maxf(q, 0.5f);
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}

/* mode: 0 low, 1 band, 2 high. */
static inline float fx_svf_run(struct fx_svf *s, const struct fx_svf_coef *c,
                               float v0, unsigned int mode)
{
    float v3 = v0 - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = fx_flush(2.0f * v1 - s->ic1);
    s->ic2 = fx_flush(2.0f * v2 - s->ic2);
    if (mode == 0u) return v2;
    if (mode == 1u) return v1;
    return v0 - c->k * v1 - v2;
}

struct fx_color {
    unsigned int kind;
    float rate;
    /* Controls as last smoothed, so a jump on the screen is not a click. */
    float color, param;
    unsigned int control_left;
    /* Filters: two stereo pairs, the main path and a tone stage. */
    struct fx_svf svf[2][2];
    struct fx_svf_coef coef[2];
    unsigned int mode[2];
    float mix;                    /* processed share, ramped near centre */
    /* Delay and reverb memory, owned by the caller. */
    struct fx_frame *delay;
    unsigned int delay_frames, write;
    float delay_time, delay_target, feedback, send;
    /* Reverb: four lines in the delay memory, and their damping. */
    unsigned int line_start[4], line_length[4], line_cursor[4];
    float damp_state[4], damp;
    /* Crush. */
    float hold_left, hold_right, hold_phase, hold_step, quantum;
    /* Noise and gate. */
    unsigned int seed;
    float lfo_phase, lfo_step, gate;
    float frames_per_beat;
    /* Smoothed processed share, and the decaying peak of a wet tail. */
    float mix_now, tail;
};

static inline unsigned int fx_color_amount_active(float color)
{ return fx_absf(color) > FX_COLOR_DEADZONE; }

/* |color| beyond the dead zone, rescaled to 0..1. */
static inline float fx_color_amount(float color)
{
    float a = (fx_absf(color) - FX_COLOR_DEADZONE) * (1.0f / (1.0f - FX_COLOR_DEADZONE));
    return fx_clampf(a, 0.0f, 1.0f);
}

static inline void fx_color_reset(struct fx_color *fx)
{
    struct fx_frame *delay = fx->delay;
    unsigned int frames = fx->delay_frames, kind = fx->kind;
    float rate = fx->rate, fpb = fx->frames_per_beat;
    unsigned int seed = fx->seed;
    for (unsigned int i = 0; i < sizeof(*fx); i++) ((unsigned char *)fx)[i] = 0;
    fx->delay = delay; fx->delay_frames = frames; fx->kind = kind; fx->rate = rate;
    fx->frames_per_beat = fpb; fx->seed = seed ? seed : 0x2545f491u;
    for (unsigned int i = 0; delay && i < frames; i++) delay[i].left = delay[i].right = 0.0f;
    fx->hold_step = 1.0f; fx->quantum = 0.0f; fx->delay_time = fx->delay_target = 1.0f;
    fx->gate = 1.0f;
    /* Mutually prime line lengths around 30..50 ms, carved from the delay
       memory after the space the echo would use. The reverb never runs at
       the same time as the echo in one engine, so they can share it. */
    static const float line_ms[4] = {29.7f, 37.1f, 41.1f, 43.7f};
    unsigned int at = 0u;
    for (unsigned int i = 0; i < 4u; i++) {
        unsigned int length = (unsigned int)(line_ms[i] * 0.001f * rate);
        if (at + length > frames) length = frames > at ? frames - at : 0u;
        fx->line_start[i] = at; fx->line_length[i] = length; fx->line_cursor[i] = 0u;
        at += length;
    }
}

static inline void fx_color_init(struct fx_color *fx, unsigned int kind, float rate,
                          struct fx_frame *delay, unsigned int delay_frames)
{
    fx->kind = kind < FX_COLOR_KINDS ? kind : FX_COLOR_FILTER;
    fx->rate = rate > 8000.0f ? rate : 44100.0f;
    fx->delay = delay;
    fx->delay_frames = delay ? delay_frames : 0u;
    fx->frames_per_beat = fx->rate * 0.5f;      /* 120 BPM until told */
    fx->seed = 0x2545f491u;
    fx_color_reset(fx);
}

/* Tempo for the synced engines; zero or nonsense keeps the last tempo. */
static inline void fx_color_tempo(struct fx_color *fx, float bpm)
{
    if (bpm >= 40.0f && bpm <= 300.0f) fx->frames_per_beat = fx->rate * 60.0f / bpm;
}

/* Recompute coefficients from the smoothed controls. */
static inline void fx_color_control(struct fx_color *fx)
{
    float c = fx->color, p = fx->param, rate = fx->rate;
    float a = fx_color_amount(c);
    int left = c < 0.0f;
    /* The processed share reaches one a little past the dead zone, so the
       centre is an exact bypass and there is no step leaving it. */
    float mix = fx_clampf(a * 12.0f, 0.0f, 1.0f);
    switch (fx->kind) {
    case FX_COLOR_FILTER:
        fx->mode[0] = left ? 0u : 2u;
        fx_svf_design(&fx->coef[0], left ? 20000.0f * fx_exp2f(-a * 8.4f)
                                         : 20.0f * fx_exp2f(a * 9.0f),
                      0.707f + p * 4.0f, rate);
        fx->mix = mix;
        break;
    case FX_COLOR_DUB_ECHO: {
        /* Half a beat, the classic dub figure; the time follows the tempo but
           is smoothed, so a tempo change bends the tail like tape. */
        fx->delay_target = fx_clampf(fx->frames_per_beat * 0.5f, 64.0f,
                                     (float)(fx->delay_frames > 2u ? fx->delay_frames - 2u : 2u));
        fx->feedback = 0.35f + p * 0.55f;
        fx->send = a;
        fx->mode[0] = left ? 0u : 2u;
        fx_svf_design(&fx->coef[0], left ? 16000.0f * fx_exp2f(-a * 6.0f)
                                         : 30.0f * fx_exp2f(a * 8.0f), 0.707f, rate);
        break;
    }
    case FX_COLOR_SPACE:
        fx->feedback = 0.70f + p * 0.25f;
        fx->damp = 0.25f;
        fx->send = a;
        fx->mode[0] = left ? 0u : 2u;
        fx_svf_design(&fx->coef[0], left ? 16000.0f * fx_exp2f(-a * 5.0f)
                                         : 40.0f * fx_exp2f(a * 6.5f), 0.707f, rate);
        break;
    case FX_COLOR_CRUSH: {
        /* 16 bits to 3, and a hold of 1 to 24 frames. */
        float bits = 16.0f - a * 13.0f;
        fx->quantum = fx_exp2f(1.0f - bits);
        fx->hold_step = 1.0f / (1.0f + a * a * 23.0f);
        fx->mode[0] = left ? 0u : 2u;
        float tone = a * (0.3f + 0.7f * p);
        fx_svf_design(&fx->coef[0], left ? 18000.0f * fx_exp2f(-tone * 5.0f)
                                         : 20.0f * fx_exp2f(tone * 7.0f), 0.707f, rate);
        fx->mix = mix;
        break;
    }
    case FX_COLOR_NOISE:
        fx->mode[0] = left ? 0u : 2u;
        fx_svf_design(&fx->coef[0], left ? 18000.0f * fx_exp2f(-a * 7.0f)
                                         : 30.0f * fx_exp2f(a * 8.5f), 1.2f, rate);
        fx->send = a * (0.05f + 0.25f * p);
        break;
    case FX_COLOR_SWEEP:
        if (left) {
            /* A quarter, an eighth or a sixteenth of a beat. */
            float division = p < 0.34f ? 0.25f : (p < 0.67f ? 0.125f : 0.0625f);
            fx->lfo_step = 1.0f / fx_maxf(fx->frames_per_beat * division, 16.0f);
            fx->send = a;                     /* gate depth */
        } else {
            fx->mode[0] = 1u;
            fx_svf_design(&fx->coef[0], 200.0f * fx_exp2f(a * 5.5f), 1.5f + p * 6.0f, rate);
            fx->send = 0.0f;
        }
        fx->mix = mix;
        break;
    }
}

static inline float fx_color_delay_read(const struct fx_color *fx, float behind, int right)
{
    float at = (float)fx->write - behind;
    while (at < 0.0f) at += (float)fx->delay_frames;
    unsigned int i0 = (unsigned int)at;
    float frac = at - (float)i0;
    if (i0 >= fx->delay_frames) i0 -= fx->delay_frames;
    unsigned int i1 = i0 + 1u >= fx->delay_frames ? 0u : i0 + 1u;
    float a = right ? fx->delay[i0].right : fx->delay[i0].left;
    float b = right ? fx->delay[i1].right : fx->delay[i1].left;
    return a + (b - a) * frac;
}

/* True while an echo or reverb tail is still sounding after the control came
 * back to centre: the caller keeps the engine running until it is not. */
static inline int fx_color_has_tail(const struct fx_color *fx)
{ return fx->tail > 1e-5f; }

/* One stereo block in place. `color` is the bipolar COLOR position in
 * [-1, 1], `param` the PARAMETER position in [0, 1]. */
static inline void fx_color_process(struct fx_color *fx, struct fx_frame *io, unsigned int frames,
                             float color, float param)
{
    color = fx_clampf(color, -1.0f, 1.0f);
    param = fx_clampf(param, 0.0f, 1.0f);
    const int delayed = fx->delay && fx->delay_frames > 4u;
    for (unsigned int i = 0; i < frames; i++) {
        if (!fx->control_left) {
            fx->color += (color - fx->color) * 0.15f;
            fx->param += (param - fx->param) * 0.15f;
            if (fx_absf(color - fx->color) < 1e-4f) fx->color = color;
            fx_color_control(fx);
            fx->control_left = FX_COLOR_CONTROL_FRAMES;
        }
        fx->control_left--;
        fx->mix_now += (fx->mix - fx->mix_now) * 0.01f;
        float m = fx->mix_now;
        float l = io[i].left, r = io[i].right;
        switch (fx->kind) {
        case FX_COLOR_FILTER: {
            if (m < 1e-4f) break;
            float fl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], l, fx->mode[0]);
            float fr = fx_svf_run(&fx->svf[0][1], &fx->coef[0], r, fx->mode[0]);
            l += (fl - l) * m; r += (fr - r) * m;
            break;
        }
        case FX_COLOR_DUB_ECHO: {
            if (!delayed) break;
            fx->delay_time += (fx->delay_target - fx->delay_time) * 0.0005f;
            float dl = fx_color_delay_read(fx, fx->delay_time, 0);
            float dr = fx_color_delay_read(fx, fx->delay_time, 1);
            float wl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], dl, fx->mode[0]);
            float wr = fx_svf_run(&fx->svf[0][1], &fx->coef[0], dr, fx->mode[0]);
            fx->delay[fx->write].left = fx_flush(fx_softclip(l * fx->send + wl * fx->feedback));
            fx->delay[fx->write].right = fx_flush(fx_softclip(r * fx->send + wr * fx->feedback));
            if (++fx->write >= fx->delay_frames) fx->write = 0u;
            float peak = fx_maxf(fx_absf(wl), fx_absf(wr));
            fx->tail = fx_maxf(peak, fx->tail * 0.9995f);
            l += wl * 0.8f; r += wr * 0.8f;
            break;
        }
        case FX_COLOR_SPACE: {
            if (!delayed) break;
            float in = (l + r) * 0.5f * fx->send;
            float y[4];
            for (unsigned int n = 0; n < 4u; n++) {
                unsigned int length = fx->line_length[n];
                y[n] = length ? fx->delay[fx->line_start[n] + fx->line_cursor[n]].left : 0.0f;
            }
            /* A Hadamard mix keeps the loop lossless; the damping and the
               decay take energy out, and nothing else does. */
            float h0 = 0.5f * (y[0] + y[1] + y[2] + y[3]);
            float h1 = 0.5f * (y[0] - y[1] + y[2] - y[3]);
            float h2 = 0.5f * (y[0] + y[1] - y[2] - y[3]);
            float h3 = 0.5f * (y[0] - y[1] - y[2] + y[3]);
            float h[4] = {h0, h1, h2, h3};
            static const float sign[4] = {1.0f, -1.0f, 1.0f, -1.0f};
            for (unsigned int n = 0; n < 4u; n++) {
                unsigned int length = fx->line_length[n];
                if (!length) continue;
                fx->damp_state[n] += (h[n] - fx->damp_state[n]) * (1.0f - fx->damp);
                float v = fx_flush(in * sign[n] + fx->damp_state[n] * fx->feedback);
                fx->delay[fx->line_start[n] + fx->line_cursor[n]].left = v;
                if (++fx->line_cursor[n] >= length) fx->line_cursor[n] = 0u;
            }
            float wl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], (y[0] + y[2]) * 0.6f, fx->mode[0]);
            float wr = fx_svf_run(&fx->svf[0][1], &fx->coef[0], (y[1] + y[3]) * 0.6f, fx->mode[0]);
            float peak = fx_maxf(fx_absf(wl), fx_absf(wr));
            fx->tail = fx_maxf(peak, fx->tail * 0.9995f);
            l += wl; r += wr;
            break;
        }
        case FX_COLOR_CRUSH: {
            if (m < 1e-4f) break;
            fx->hold_phase += fx->hold_step;
            if (fx->hold_phase >= 1.0f) {
                fx->hold_phase -= 1.0f;
                float q = fx->quantum, inverse = q > 0.0f ? 1.0f / q : 0.0f;
                float sl = l * inverse, sr = r * inverse;
                fx->hold_left = q > 0.0f ? (float)(int)(sl + (sl < 0.0f ? -0.5f : 0.5f)) * q : l;
                fx->hold_right = q > 0.0f ? (float)(int)(sr + (sr < 0.0f ? -0.5f : 0.5f)) * q : r;
            }
            float cl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], fx->hold_left, fx->mode[0]);
            float cr = fx_svf_run(&fx->svf[0][1], &fx->coef[0], fx->hold_right, fx->mode[0]);
            l += (cl - l) * m; r += (cr - r) * m;
            break;
        }
        case FX_COLOR_NOISE: {
            if (fx->send < 1e-5f) {
                fx->svf[0][0].ic1 *= 0.999f; fx->svf[0][0].ic2 *= 0.999f;
                fx->svf[0][1].ic1 *= 0.999f; fx->svf[0][1].ic2 *= 0.999f;
                break;
            }
            float nl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], fx_noise(&fx->seed), fx->mode[0]);
            float nr = fx_svf_run(&fx->svf[0][1], &fx->coef[0], fx_noise(&fx->seed), fx->mode[0]);
            l += nl * fx->send; r += nr * fx->send;
            break;
        }
        case FX_COLOR_SWEEP: {
            if (fx->color < 0.0f) {
                fx->lfo_phase += fx->lfo_step;
                if (fx->lfo_phase >= 1.0f) fx->lfo_phase -= 1.0f;
                float target = fx->lfo_phase < 0.5f ? 1.0f : 1.0f - fx->send;
                fx->gate += (target - fx->gate) * 0.02f;
                l *= fx->gate; r *= fx->gate;
            } else {
                fx->gate += (1.0f - fx->gate) * 0.02f;
                l *= fx->gate; r *= fx->gate;
                if (m < 1e-4f) break;
                /* The band output peaks at Q; k = 1/Q brings it back to unity
                   so a narrow band is not also a loud one. */
                float makeup = fx->coef[0].k * 1.6f;
                float bl = fx_svf_run(&fx->svf[0][0], &fx->coef[0], l, 1u) * makeup;
                float br = fx_svf_run(&fx->svf[0][1], &fx->coef[0], r, 1u) * makeup;
                l += (bl - l) * m; r += (br - r) * m;
            }
            break;
        }
        }
        io[i].left = l; io[i].right = r;
    }
}

#endif /* FX_COLOR_H */
