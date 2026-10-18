/* SPDX-License-Identifier: MPL-2.0 */
/* The RMX-1000's X-PAD FX section, in software.
 *
 * Four sounds, KICK, SNARE, CLAP and HI HAT, synthesised here (none is taken
 * from Pioneer) or replaced by WAV files from the drive. A hit plays a sound
 * once; touching the X-PAD strip with a sound selected repeats it at the
 * strip's division of the beat, the way the RMX rolls its drums. PITCH moves
 * every sound, and VOLUME sets the section's level on the master.
 *
 * OVERDUB works as on the RMX: pressed once it stands by; the first X-PAD FX
 * after that starts a four-beat loop, and everything the section plays from
 * then on is recorded into it. Each sound has a track of its own, and so has
 * ROLL, so one sound can be muted (HOLD + sound) or deleted (OVERDUB + sound)
 * on its own. Pressing OVERDUB again stops the loop and deletes it.
 *
 * Requests arrive as atomic counters and words from any thread; only the
 * audio thread changes state. No allocation, no I/O, bounded work per block.
 */
#ifndef FX_XPAD_H
#define FX_XPAD_H
#include "rx3_fx_rmx.h"

enum { FX_XPAD_KICK, FX_XPAD_SNARE, FX_XPAD_CLAP, FX_XPAD_HAT, FX_XPAD_SOUNDS };
#define FX_XPAD_ROLL_TRACK FX_XPAD_SOUNDS
#define FX_XPAD_TRACKS (FX_XPAD_SOUNDS + 1u)
#define FX_XPAD_VOICES 8u
#define FX_XPAD_CHUNK 512u
#define FX_XPAD_CHOKE 64u              /* a retriggered sound fades its last hit */
#define FX_XPAD_PATTERN_BEATS 4.0f
#define FX_XPAD_CLEAR_CHUNK 8192u
/* A synthesised sound is at most this long. */
#define FX_XPAD_SYNTH_SECONDS 0.5f

static inline unsigned int fx_xpad_pattern_capacity(float rate)
{ return (unsigned int)(rate * 60.0f / FX_RMX_MIN_BPM * FX_XPAD_PATTERN_BEATS) + 64u; }

static inline unsigned int fx_xpad_synth_frames(float rate)
{ return (unsigned int)(rate * FX_XPAD_SYNTH_SECONDS); }

/* --------------------------------------------------------------- sounds --- */

/* One sound into `out`, normalised to a peak of 0.7. Returns its length. The
   recipes are the classic analogue ones: a falling sine and a click for the
   kick, two tones and noise for the snare, three bursts and a tail of
   band-passed noise for the clap, six detuned squares through a high-pass for
   the hat. */
static inline unsigned int fx_xpad_synthesize(unsigned int sound, struct fx_frame *out,
                                              unsigned int capacity, float rate)
{
    unsigned int frames = fx_xpad_synth_frames(rate);
    if (frames > capacity) frames = capacity;
    unsigned int seed = 0x1234567u + sound * 7919u;
    struct fx_svf filter[2] = {{0, 0}, {0, 0}};
    struct fx_svf_coef coef;
    float phase = 0.0f, phase2 = 0.0f, peak = 0.0f, dt = 1.0f / rate;
    static const float hat_hz[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    float hat_phase[6] = {0, 0, 0, 0, 0, 0};
    if (sound == FX_XPAD_SNARE) fx_svf_design(&coef, 1800.0f, 0.8f, rate);
    else if (sound == FX_XPAD_CLAP) fx_svf_design(&coef, 1200.0f, 1.6f, rate);
    else fx_svf_design(&coef, 7000.0f, 0.9f, rate);
    unsigned int length = frames;
    for (unsigned int i = 0; i < frames; i++) {
        float t = (float)i * dt, v = 0.0f;
        switch (sound) {
        case FX_XPAD_KICK: {
            float hz = 45.0f + 105.0f * fx_exp2f(-t * 41.2f);    /* e-fold 35 ms */
            phase += hz * dt; if (phase >= 1.0f) phase -= 1.0f;
            v = fx_sin_turns(phase) * fx_exp2f(-t * 5.8f);       /* e-fold 250 ms */
            if (i < (unsigned int)(rate * 0.002f)) v += fx_noise(&seed) * 0.3f;
            length = (unsigned int)(rate * 0.45f);
            break;
        }
        case FX_XPAD_SNARE: {
            phase += 185.0f * dt; if (phase >= 1.0f) phase -= 1.0f;
            phase2 += 330.0f * dt; if (phase2 >= 1.0f) phase2 -= 1.0f;
            float tone = (fx_sin_turns(phase) + 0.6f * fx_sin_turns(phase2)) * fx_exp2f(-t * 24.0f);
            float hiss = fx_svf_run(&filter[0], &coef, fx_noise(&seed), 2u) * fx_exp2f(-t * 9.6f);
            v = tone * 0.6f + hiss;
            length = (unsigned int)(rate * 0.35f);
            break;
        }
        case FX_XPAD_CLAP: {
            float envelope;
            if (t < 0.030f) {
                float burst = t - (float)(int)(t * 100.0f) * 0.010f;   /* 10 ms apart */
                envelope = fx_exp2f(-burst * 240.0f);
            } else {
                envelope = 0.8f * fx_exp2f(-(t - 0.030f) * 12.0f);
            }
            v = fx_svf_run(&filter[0], &coef, fx_noise(&seed), 1u) * envelope * 2.0f;
            length = (unsigned int)(rate * 0.40f);
            break;
        }
        default: {
            float metal = 0.0f;
            for (unsigned int n = 0; n < 6u; n++) {
                hat_phase[n] += hat_hz[n] * 2.0f * dt;
                if (hat_phase[n] >= 1.0f) hat_phase[n] -= 1.0f;
                metal += hat_phase[n] < 0.5f ? 1.0f : -1.0f;
            }
            v = fx_svf_run(&filter[0], &coef, metal * 0.2f + fx_noise(&seed) * 0.3f, 2u);
            v = fx_svf_run(&filter[1], &coef, v, 2u) * fx_exp2f(-t * 28.0f);
            length = (unsigned int)(rate * 0.20f);
            break;
        }
        }
        if (i >= length) v = 0.0f;
        out[i].left = out[i].right = v;
        if (fx_absf(v) > peak) peak = fx_absf(v);
    }
    if (length > frames) length = frames;
    float scale = peak > 0.0f ? 0.7f / peak : 0.0f;
    for (unsigned int i = 0; i < length; i++) { out[i].left *= scale; out[i].right *= scale; }
    /* The last 64 frames fade, so a cut tail does not click. */
    for (unsigned int i = length > 64u ? length - 64u : 0u, k = 64u; i < length; i++, k--) {
        float g = (float)k * (1.0f / 64.0f);
        out[i].left *= g; out[i].right *= g;
    }
    return length;
}

/* ---------------------------------------------------------------- state --- */

struct fx_xpad_sound {
    const struct fx_frame *frames;     /* swapped whole by the loader */
    unsigned int length;
    float rate;                        /* the sound's own sample rate */
};

struct fx_xpad_voice {
    unsigned int sound, active;
    float position, gain, fade;
};

enum { FX_PATTERN_OFF, FX_PATTERN_STANDBY, FX_PATTERN_RUN };

struct fx_xpad {
    float rate;
    struct fx_xpad_sound sounds[FX_XPAD_SOUNDS];
    struct fx_xpad_voice voices[FX_XPAD_VOICES];
    struct fx_frame scratch[FX_XPAD_TRACKS][FX_XPAD_CHUNK];
    /* Repeat from the strip: the sound and its interval. */
    unsigned int repeat_word_seen, repeat_sound, repeat_interval, repeat_left;
    /* The pattern. */
    struct fx_frame *track[FX_XPAD_TRACKS];
    unsigned int capacity, length, position, state;
    unsigned int clearing[FX_XPAD_TRACKS];     /* frames still to zero, 0 when clean */
    unsigned int written[FX_XPAD_TRACKS];      /* holds anything */
    float level;
    /* Requests. */
    volatile unsigned int hits[FX_XPAD_SOUNDS];
    unsigned int hits_seen[FX_XPAD_SOUNDS];
    volatile unsigned int repeat_word;         /* (sound + 1) << 24 | interval, 0 off */
    volatile unsigned int overdub_presses, deletes[FX_XPAD_TRACKS];
    unsigned int overdub_seen, deletes_seen[FX_XPAD_TRACKS];
    volatile unsigned int muted;               /* one bit per track */
    volatile unsigned int pattern_length;      /* frames, for the next STANDBY */
    volatile int pitch_cents;
    volatile unsigned int volume_permille;
    /* For the screen and the lights. */
    volatile unsigned int shown_state, shown_tracks, shown_playing;
};

static inline void fx_xpad_init(struct fx_xpad *x, float rate,
                                struct fx_frame *const tracks[FX_XPAD_TRACKS], unsigned int capacity)
{
    for (unsigned int i = 0; i < sizeof(*x); i++) ((unsigned char *)x)[i] = 0;
    x->rate = rate;
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) x->track[t] = tracks ? tracks[t] : 0;
    x->capacity = tracks ? capacity : 0u;
    x->volume_permille = 800u;
    x->level = 0.64f;
}

/* Publish a sound; frames must stay valid until another replaces it. */
static inline void fx_xpad_set_sound(struct fx_xpad *x, unsigned int sound,
                                     const struct fx_frame *frames, unsigned int length, float rate)
{
    if (sound >= FX_XPAD_SOUNDS) return;
    __atomic_store_n(&x->sounds[sound].length, 0u, __ATOMIC_SEQ_CST);
    x->sounds[sound].rate = rate;
    __atomic_store_n(&x->sounds[sound].frames, frames, __ATOMIC_SEQ_CST);
    __atomic_store_n(&x->sounds[sound].length, length, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------- requests --- */

static inline void fx_xpad_hit(struct fx_xpad *x, unsigned int sound)
{ if (sound < FX_XPAD_SOUNDS) __atomic_add_fetch(&x->hits[sound], 1u, __ATOMIC_SEQ_CST); }

/* Repeat `sound` every `interval` frames, the first at once; interval 0 stops. */
static inline void fx_xpad_repeat(struct fx_xpad *x, unsigned int sound, unsigned int interval)
{
    unsigned int word = interval && sound < FX_XPAD_SOUNDS
        ? ((sound + 1u) << 24) | (interval & 0x00ffffffu) : 0u;
    __atomic_store_n(&x->repeat_word, word, __ATOMIC_SEQ_CST);
}

/* OVERDUB: off -> standby; standby or running -> stop and delete. */
static inline void fx_xpad_overdub(struct fx_xpad *x, unsigned int length)
{
    __atomic_store_n(&x->pattern_length, length, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&x->overdub_presses, 1u, __ATOMIC_SEQ_CST);
}

static inline void fx_xpad_delete(struct fx_xpad *x, unsigned int track)
{ if (track < FX_XPAD_TRACKS) __atomic_add_fetch(&x->deletes[track], 1u, __ATOMIC_SEQ_CST); }

static inline void fx_xpad_toggle_mute(struct fx_xpad *x, unsigned int track)
{ if (track < FX_XPAD_TRACKS) __atomic_xor_fetch(&x->muted, 1u << track, __ATOMIC_SEQ_CST); }

static inline void fx_xpad_pitch(struct fx_xpad *x, int cents)
{
    if (cents < -1200) cents = -1200;
    if (cents > 1200) cents = 1200;
    __atomic_store_n(&x->pitch_cents, cents, __ATOMIC_SEQ_CST);
}

static inline void fx_xpad_volume(struct fx_xpad *x, unsigned int permille)
{ __atomic_store_n(&x->volume_permille, permille > 1000u ? 1000u : permille, __ATOMIC_SEQ_CST); }

/* ---------------------------------------------------------------- audio --- */

static inline void fx_xpad_start_voice(struct fx_xpad *x, unsigned int sound)
{
    /* The sound's last hit is choked, as one drum struck again is. */
    int free = -1, oldest = 0;
    float furthest = -1.0f;
    for (unsigned int v = 0; v < FX_XPAD_VOICES; v++) {
        struct fx_xpad_voice *voice = &x->voices[v];
        if (voice->active && voice->sound == sound && voice->fade == 0.0f) voice->fade = 1.0f;
        if (!voice->active && free < 0) free = (int)v;
        if (voice->active && voice->position > furthest) { furthest = voice->position; oldest = (int)v; }
    }
    struct fx_xpad_voice *voice = &x->voices[free >= 0 ? free : oldest];
    voice->sound = sound;
    voice->position = 0.0f;
    voice->gain = 1.0f;
    voice->fade = 0.0f;
    voice->active = 1u;
}

/* A track starting to hold sound, which a STANDBY pattern takes as its cue. */
static inline void fx_xpad_cue(struct fx_xpad *x)
{
    if (x->state != FX_PATTERN_STANDBY) return;
    unsigned int length = __atomic_load_n(&x->pattern_length, __ATOMIC_SEQ_CST);
    if (!length || length > x->capacity) length = x->capacity;
    x->length = length;
    x->position = 0u;
    x->state = FX_PATTERN_RUN;
}

static inline void fx_xpad_take(struct fx_xpad *x, unsigned int *started)
{
    unsigned int presses = __atomic_load_n(&x->overdub_presses, __ATOMIC_SEQ_CST);
    while (x->overdub_seen != presses) {
        x->overdub_seen++;
        if (x->state == FX_PATTERN_OFF && x->capacity) {
            x->state = FX_PATTERN_STANDBY;
        } else {
            x->state = FX_PATTERN_OFF;
            for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++)
                if (x->written[t]) x->clearing[t] = x->capacity;
        }
    }
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) {
        unsigned int asked = __atomic_load_n(&x->deletes[t], __ATOMIC_SEQ_CST);
        if (asked != x->deletes_seen[t]) {
            x->deletes_seen[t] = asked;
            if (x->written[t]) x->clearing[t] = x->capacity;
        }
    }
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) {
        unsigned int asked = __atomic_load_n(&x->hits[s], __ATOMIC_SEQ_CST);
        if (asked != x->hits_seen[s]) {
            x->hits_seen[s] = asked;
            *started |= 1u << s;
        }
    }
    unsigned int word = __atomic_load_n(&x->repeat_word, __ATOMIC_SEQ_CST);
    if (word != x->repeat_word_seen) {
        x->repeat_word_seen = word;
        unsigned int sound = word >> 24, interval = word & 0x00ffffffu;
        if (sound && interval) {
            unsigned int was = x->repeat_sound;
            x->repeat_interval = interval;
            /* A new sound starts at once; the same one sliding to another
               division keeps its phase and takes the new interval. */
            if (was != sound || !x->repeat_left) x->repeat_left = 0u;
            if (x->repeat_left > interval) x->repeat_left = interval;
            x->repeat_sound = sound;
        } else {
            x->repeat_sound = 0u;
        }
    }
}

/* Clear a chunk of every track being deleted. */
static inline void fx_xpad_clear(struct fx_xpad *x)
{
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) {
        if (!x->clearing[t]) continue;
        unsigned int count = x->clearing[t] < FX_XPAD_CLEAR_CHUNK ? x->clearing[t] : FX_XPAD_CLEAR_CHUNK;
        unsigned int at = x->capacity - x->clearing[t];
        for (unsigned int i = 0; i < count; i++) x->track[t][at + i].left = x->track[t][at + i].right = 0.0f;
        x->clearing[t] -= count;
        if (!x->clearing[t]) x->written[t] = 0u;
    }
}

/* Every sounding voice, over frames [from, to) of the current chunk. */
static inline void fx_xpad_render(struct fx_xpad *x, unsigned int from, unsigned int to, float ratio)
{
    for (unsigned int v = 0; v < FX_XPAD_VOICES; v++) {
        struct fx_xpad_voice *voice = &x->voices[v];
        if (!voice->active) continue;
        const struct fx_xpad_sound *sound = &x->sounds[voice->sound];
        unsigned int length = __atomic_load_n(&sound->length, __ATOMIC_SEQ_CST);
        const struct fx_frame *data = __atomic_load_n(&sound->frames, __ATOMIC_SEQ_CST);
        if (!data || !length) { voice->active = 0u; continue; }
        float step = ratio * sound->rate / x->rate;
        struct fx_frame *out = x->scratch[voice->sound];
        for (unsigned int i = from; i < to; i++) {
            unsigned int at = (unsigned int)voice->position;
            if (at + 1u >= length) { voice->active = 0u; break; }
            float frac = voice->position - (float)at, g = voice->gain;
            if (voice->fade > 0.0f) {
                voice->fade -= 1.0f / (float)FX_XPAD_CHOKE;
                if (voice->fade <= 0.0f) { voice->active = 0u; break; }
                g *= voice->fade;
            }
            out[i].left += (data[at].left + (data[at + 1u].left - data[at].left) * frac) * g;
            out[i].right += (data[at].right + (data[at + 1u].right - data[at].right) * frac) * g;
            voice->position += step;
        }
    }
}

/* One block. `roll` is the signal a running ROLL is producing, or null when
   no roll is engaged; the ROLL track records it. The section's sounds and
   pattern are added to `bus` at VOLUME. */
static inline void fx_xpad_process(struct fx_xpad *x, const struct fx_frame *roll,
                                   struct fx_frame *bus, unsigned int frames)
{
    unsigned int started = 0u;
    fx_xpad_take(x, &started);
    fx_xpad_clear(x);
    int cents = __atomic_load_n(&x->pitch_cents, __ATOMIC_SEQ_CST);
    float ratio = cents ? fx_exp2f((float)cents * (1.0f / 1200.0f)) : 1.0f;
    float target = (float)__atomic_load_n(&x->volume_permille, __ATOMIC_SEQ_CST) * 0.001f;
    target *= target;                          /* a fader's law */
    unsigned int muted = __atomic_load_n(&x->muted, __ATOMIC_SEQ_CST);
    if (roll) fx_xpad_cue(x);
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++)
        if (started & (1u << s)) { fx_xpad_start_voice(x, s); fx_xpad_cue(x); }

    for (unsigned int done = 0; done < frames; ) {
        unsigned int n = frames - done < FX_XPAD_CHUNK ? frames - done : FX_XPAD_CHUNK;
        for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++)
            for (unsigned int i = 0; i < n; i++) x->scratch[t][i].left = x->scratch[t][i].right = 0.0f;
        /* Split the chunk where a repeat falls, so it is struck at its own
           frame and chokes the previous hit from there, not from the next
           block. */
        for (unsigned int at = 0; at < n; ) {
            unsigned int segment = n - at;
            if (x->repeat_sound) {
                if (!x->repeat_left) {
                    fx_xpad_start_voice(x, x->repeat_sound - 1u);
                    fx_xpad_cue(x);
                    x->repeat_left = x->repeat_interval;
                }
                if (x->repeat_left < segment) segment = x->repeat_left;
                x->repeat_left -= segment;
            }
            fx_xpad_render(x, at, at + segment, ratio);
            at += segment;
        }
        if (roll)
            for (unsigned int i = 0; i < n; i++) x->scratch[FX_XPAD_ROLL_TRACK][i] = roll[done + i];
        for (unsigned int i = 0; i < n; i++) {
            x->level += (target - x->level) * 0.002f;
            float l = 0.0f, r = 0.0f;
            for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) { l += x->scratch[s][i].left; r += x->scratch[s][i].right; }
            if (x->state == FX_PATTERN_RUN) {
                unsigned int at = x->position;
                for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) {
                    struct fx_frame *track = x->track[t];
                    if (x->clearing[t]) continue;
                    /* Played before it is recorded, so a sound overdubbed now
                       comes back on the next pass, not doubled on this one. */
                    if (x->written[t] && !(muted & (1u << t))) { l += track[at].left; r += track[at].right; }
                    float in_l = x->scratch[t][i].left, in_r = x->scratch[t][i].right;
                    if (in_l != 0.0f || in_r != 0.0f) {
                        track[at].left += in_l; track[at].right += in_r;
                        x->written[t] = 1u;
                    }
                }
                if (++x->position >= x->length) x->position = 0u;
            }
            bus[done + i].left += l * x->level;
            bus[done + i].right += r * x->level;
        }
        done += n;
    }
    unsigned int tracks = 0u, playing = 0u;
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) if (x->written[t] && !x->clearing[t]) tracks |= 1u << t;
    for (unsigned int v = 0; v < FX_XPAD_VOICES; v++)
        if (x->voices[v].active) playing |= 1u << x->voices[v].sound;
    x->shown_state = x->state;
    x->shown_tracks = tracks;
    x->shown_playing = playing;
}

#endif /* FX_XPAD_H */
