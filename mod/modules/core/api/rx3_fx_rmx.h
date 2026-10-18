/* SPDX-License-Identifier: MPL-2.0 */
/* The RMX-style ROLL engine.
 *
 * ROLL is an insert: it sits on a signal (one deck's stream, or the master bus)
 * and, while engaged, replaces it with a slice that repeats. The slice is
 * recorded from the moment of the press, so the first repetition *is* the
 * live signal and engaging never clicks. The live signal keeps running
 * underneath: when the roll lets go the track is where it would have been.
 * Recording continues while the slice repeats, so a longer division chosen
 * mid-roll can still be served from the same start point.
 *
 * The X-PAD's sounds and its four-beat OVERDUB live in rx3_fx_xpad.h.
 *
 * It runs on the audio thread: no allocation, no I/O, bounded work per block.
 * Requests (engage, length, pitch) come from the input thread through atomic
 * words, and only the audio thread changes state.
 */
#ifndef FX_RMX_H
#define FX_RMX_H
#include "rx3_fx_color.h"

/* The seam: this many frames past the slice's end are blended into its start,
 * so the loop point is a crossfade rather than a step. 256 frames is 5.8 ms
 * at 44.1 kHz: shorter than the shortest slice we play (1/16 at 200 BPM is
 * 827 frames) and long enough to hide a waveform's phase. */
#define FX_RMX_SEAM 256u
/* Release and level ramps, and the dip that hides a jump inside the slice. */
#define FX_RMX_RAMP 220u
#define FX_RMX_DIP 32u
/* Slowest tempo the buffers are sized for. */
#define FX_RMX_MIN_BPM 60.0f
#define FX_RMX_MAX_BEATS 8.0f

static inline unsigned int fx_rmx_capacity(float rate)
{ return (unsigned int)(rate * 60.0f / FX_RMX_MIN_BPM * FX_RMX_MAX_BEATS) + FX_RMX_SEAM + 64u; }

/* ---------------------------------------------------------------- ROLL --- */

enum { FX_ROLL_IDLE, FX_ROLL_CAPTURE, FX_ROLL_LOOP };

/* Requests, packed into one word so one store carries all of them:
 * bit 31 engaged, bits 0..23 slice length in frames, bits 24..30 a sequence
 * number so a repeated press of the same pad is still a new press. */
#define FX_ROLL_ENGAGED 0x80000000u
#define FX_ROLL_LENGTH_MASK 0x00ffffffu

struct fx_roll {
    struct fx_frame *buffer;
    unsigned int capacity;
    struct fx_frame head[FX_RMX_SEAM];   /* the slice start before blending */
    unsigned int state, length, recorded, position, seamed;
    unsigned int applied;                /* last request taken */
    unsigned int pending, pending_previous, dipping;  /* held for the dip */
    float wet, wet_step, dip;
    volatile unsigned int request;
    /* PITCH, in cents. The slice keeps its length in time and is read
       faster or slower inside it, the way the RMX pitches a roll. */
    volatile int pitch_cents;
};

static inline void fx_roll_init(struct fx_roll *roll, struct fx_frame *buffer, unsigned int capacity)
{
    for (unsigned int i = 0; i < sizeof(*roll); i++) ((unsigned char *)roll)[i] = 0;
    roll->buffer = buffer;
    roll->capacity = buffer ? capacity : 0u;
    roll->dip = 1.0f;
}

/* Input thread. length 0 releases. A same-length request while engaged is a
 * no-op; a different one changes the division without leaving the roll. */
static inline void fx_roll_set(struct fx_roll *roll, unsigned int length, unsigned int press)
{
    unsigned int word = length ? (FX_ROLL_ENGAGED | ((press & 0x7fu) << 24) |
                                  (length & FX_ROLL_LENGTH_MASK)) : 0u;
    __atomic_store_n(&roll->request, word, __ATOMIC_SEQ_CST);
}

/* Any thread; applied from the next block. Clamped to an octave each way. */
static inline void fx_roll_pitch(struct fx_roll *roll, int cents)
{
    if (cents < -1200) cents = -1200;
    if (cents > 1200) cents = 1200;
    __atomic_store_n(&roll->pitch_cents, cents, __ATOMIC_SEQ_CST);
}

static inline int fx_roll_active(const struct fx_roll *roll)
{ return roll->state != FX_ROLL_IDLE; }

/* Blend the frames recorded past `length` into the slice's start. */
static inline void fx_roll_seam(struct fx_roll *roll, unsigned int length)
{
    for (unsigned int k = 0; k < FX_RMX_SEAM; k++) {
        float f = ((float)k + 0.5f) * (1.0f / (float)FX_RMX_SEAM);
        const struct fx_frame *after = &roll->buffer[length + k];
        roll->buffer[k].left = roll->head[k].left * f + after->left * (1.0f - f);
        roll->buffer[k].right = roll->head[k].right * f + after->right * (1.0f - f);
    }
    roll->seamed = length;
}

static inline void fx_roll_start_capture(struct fx_roll *roll, unsigned int length)
{
    roll->state = FX_ROLL_CAPTURE;
    roll->length = length;
    roll->recorded = 0u;
    roll->position = 0u;
    roll->seamed = 0u;
    roll->wet = 1.0f;
    roll->wet_step = 0.0f;
}

/* Act on a request, possibly after the dip has brought the slice to silence. */
static inline void fx_roll_apply(struct fx_roll *roll, unsigned int word, unsigned int previous)
{
    unsigned int length = word & FX_ROLL_LENGTH_MASK;
    if (!(word & FX_ROLL_ENGAGED) || !length) {
        if (roll->state != FX_ROLL_IDLE)
            roll->wet_step = -1.0f / (float)FX_RMX_RAMP;
        return;
    }
    if (length + FX_RMX_SEAM + 1u > roll->capacity) length = roll->capacity - FX_RMX_SEAM - 1u;
    if (roll->state == FX_ROLL_IDLE || roll->wet_step < 0.0f ||
        (word & ~FX_ROLL_LENGTH_MASK) != (previous & ~FX_ROLL_LENGTH_MASK)) {
        /* A new press, or one while letting go: capture from now. */
        fx_roll_start_capture(roll, length);
        return;
    }
    if (length == roll->length) return;
    if (roll->state == FX_ROLL_LOOP && length + FX_RMX_SEAM <= roll->recorded) {
        /* The same start, a new length: re-blend the seam for it and fold
           the read position in. Only the audio thread reads the slice. */
        for (unsigned int k = 0; k < FX_RMX_SEAM; k++) roll->buffer[k] = roll->head[k];
        fx_roll_seam(roll, length);
        roll->length = length;
        while (roll->position >= length) roll->position -= length;
        return;
    }
    if (roll->state == FX_ROLL_CAPTURE && roll->recorded < length) {
        /* Still on the first pass: just let it run longer. */
        roll->length = length;
        return;
    }
    fx_roll_start_capture(roll, length);
}

/* In place on the signal the roll is inserted on. */
static inline void fx_roll_process(struct fx_roll *roll, struct fx_frame *io, unsigned int frames)
{
    if (!roll->capacity) return;
    unsigned int word = __atomic_load_n(&roll->request, __ATOMIC_SEQ_CST);
    if (word != roll->applied && !roll->dipping) {
        /* Engaging from silence, or capturing, needs no dip: the output is
           already the live signal. Anything that moves the read position
           inside a repeating slice does. */
        if (roll->state == FX_ROLL_LOOP && (word & FX_ROLL_ENGAGED)) {
            roll->pending = word;
            roll->pending_previous = roll->applied;
            roll->dipping = 1u;
        } else {
            fx_roll_apply(roll, word, roll->applied);
        }
        roll->applied = word;
    }
    if (roll->state == FX_ROLL_IDLE) return;
    int cents = __atomic_load_n(&roll->pitch_cents, __ATOMIC_SEQ_CST);
    float ratio = cents ? fx_exp2f((float)cents * (1.0f / 1200.0f)) : 1.0f;
    for (unsigned int i = 0; i < frames; i++) {
        struct fx_frame live = io[i];
        if (roll->recorded < roll->capacity) roll->buffer[roll->recorded++] = live;
        struct fx_frame slice = live;
        if (roll->state == FX_ROLL_CAPTURE) {
            unsigned int t = roll->recorded - 1u;
            if (t >= roll->length) {
                /* The second pass begins: blend as it plays, keeping the
                   originals so a later length can re-blend. */
                unsigned int k = t - roll->length;
                float f = ((float)k + 0.5f) * (1.0f / (float)FX_RMX_SEAM);
                roll->head[k] = roll->buffer[k];
                slice.left = roll->buffer[k].left * f + live.left * (1.0f - f);
                slice.right = roll->buffer[k].right * f + live.right * (1.0f - f);
                roll->buffer[k] = slice;
                if (k + 1u >= FX_RMX_SEAM) {
                    roll->seamed = roll->length;
                    roll->state = FX_ROLL_LOOP;
                    roll->position = FX_RMX_SEAM;
                    if (roll->position >= roll->length) roll->position = 0u;
                }
            }
        } else if (!cents) {
            slice = roll->buffer[roll->position];
            if (++roll->position >= roll->length) roll->position = 0u;
        } else {
            /* Each repetition starts at the slice's start and walks it at
               the pitch ratio, wrapping inside it when raised. Its two ends
               are faded over 64 frames, since they no longer meet. */
            unsigned int length = roll->length, at = roll->position;
            float read = (float)at * ratio;
            while (read >= (float)length) read -= (float)length;
            unsigned int i0 = (unsigned int)read;
            if (i0 >= length) i0 = length - 1u;
            unsigned int i1 = i0 + 1u >= length ? 0u : i0 + 1u;
            float frac = read - (float)i0;
            const struct fx_frame *a = &roll->buffer[i0], *b = &roll->buffer[i1];
            float edge = 1.0f;
            if (length > 256u) {
                if (at < 64u) edge = (float)(at + 1u) * (1.0f / 64.0f);
                else if (at >= length - 64u) edge = (float)(length - at) * (1.0f / 64.0f);
            }
            slice.left = (a->left + (b->left - a->left) * frac) * edge;
            slice.right = (a->right + (b->right - a->right) * frac) * edge;
            if (++roll->position >= length) roll->position = 0u;
        }
        if (roll->dipping) {
            roll->dip -= 1.0f / (float)FX_RMX_DIP;
            if (roll->dipping == 1u && roll->dip <= 0.0f) {
                roll->dip = 0.0f;
                fx_roll_apply(roll, roll->pending, roll->pending_previous);
                roll->dipping = 2u;
            } else if (roll->dipping == 2u) {
                roll->dip += 2.0f / (float)FX_RMX_DIP;
                if (roll->dip >= 1.0f) { roll->dip = 1.0f; roll->dipping = 0u; }
            }
            slice.left *= roll->dip;
            slice.right *= roll->dip;
        }
        if (roll->wet_step != 0.0f) {
            roll->wet += roll->wet_step;
            if (roll->wet <= 0.0f) {
                roll->wet = 0.0f;
                roll->wet_step = 0.0f;
                roll->state = FX_ROLL_IDLE;
                io[i] = live;
                return;
            }
        }
        io[i].left = live.left + (slice.left - live.left) * roll->wet;
        io[i].right = live.right + (slice.right - live.right) * roll->wet;
    }
}

#endif /* FX_RMX_H */
