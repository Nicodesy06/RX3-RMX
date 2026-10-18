/* SPDX-License-Identifier: MPL-2.0 */
/* RMX-style ROLL and live LOOP engines.
 *
 * ROLL is an insert: it sits on a signal (one deck's stream, or the master bus)
 * and, while engaged, replaces it with a slice that repeats. The slice is
 * recorded from the moment of the press, so the first repetition *is* the
 * live signal and engaging never clicks. The live signal keeps running
 * underneath: when the roll lets go the track is where it would have been.
 * Recording continues while the slice repeats, so a longer division chosen
 * mid-roll can still be served from the same start point.
 *
 * LOOP is a bus: a loop captured from a source plays on its own, at its own
 * level, until it is stopped or cleared, whatever the decks do. OVERDUB adds
 * the source into the loop pass after pass. Each overdub session is its own
 * layer, so the last one can be undone; past FX_LOOP_LAYERS the oldest is
 * folded into the base a chunk at a time, never in one block.
 *
 * Both run on the audio thread: no allocation, no I/O, bounded work per block.
 * Requests (engage, length, undo...) come from the input thread through the
 * atomics in the `request` words, and only the audio thread changes state.
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
        } else {
            slice = roll->buffer[roll->position];
            if (++roll->position >= roll->length) roll->position = 0u;
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

/* ---------------------------------------------------------------- LOOP --- */

#define FX_LOOP_LAYERS 4u
#define FX_LOOP_SLOTS (FX_LOOP_LAYERS + 1u)
#define FX_LOOP_MERGE_CHUNK 4096u
#define FX_LOOP_EDGE 64u          /* overdub fade in and out */

enum { FX_LOOP_EMPTY, FX_LOOP_RECORD, FX_LOOP_PLAY };

/* Commands from the input thread, one at a time through `command`. */
enum {
    FX_LOOP_CMD_NONE, FX_LOOP_CMD_RECORD, FX_LOOP_CMD_OVERDUB_ON, FX_LOOP_CMD_OVERDUB_OFF,
    FX_LOOP_CMD_UNDO, FX_LOOP_CMD_CLEAR, FX_LOOP_CMD_STOP, FX_LOOP_CMD_PLAY
};

struct fx_loop_slot {
    struct fx_frame *frames;
    unsigned int start, written;   /* the circular range this layer holds */
    unsigned int used;             /* in the undo stack */
    float gain, gain_step;         /* fades a layer out on undo */
};

struct fx_loop {
    struct fx_frame *base;
    struct fx_loop_slot slot[FX_LOOP_SLOTS];
    unsigned int capacity;
    unsigned int state, length, position, recorded;
    /* Undo order: stack[0] oldest .. stack[depth-1] newest. */
    unsigned int stack[FX_LOOP_SLOTS], depth;
    /* The open overdub session, if any. */
    unsigned int overdub, session_slot, session_elapsed;
    float record_gain, record_step;
    /* A layer being folded into the base, if any. */
    unsigned int merging, merge_slot;
    float level, level_target, playing_gain, playing_step;
    unsigned int clear_after_fade;
    volatile unsigned int command;
    volatile unsigned int pending_length;
    volatile unsigned int level_permille;
    /* Read-only state for the screen and the LEDs. */
    volatile unsigned int shown_state, shown_layers, shown_overdub;
};

static inline void fx_loop_init(struct fx_loop *loop, struct fx_frame *base,
                         struct fx_frame *const slots[FX_LOOP_SLOTS], unsigned int capacity)
{
    for (unsigned int i = 0; i < sizeof(*loop); i++) ((unsigned char *)loop)[i] = 0;
    loop->base = base;
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++) loop->slot[i].frames = slots ? slots[i] : 0;
    loop->capacity = base ? capacity : 0u;
    loop->level = loop->level_target = 0.8f;
    loop->level_permille = 800u;
}

/* Input thread. A command is dropped if the previous one has not been taken
 * yet, which at one audio block is a few milliseconds. */
static inline int fx_loop_command(struct fx_loop *loop, unsigned int command, unsigned int length)
{
    if (length) __atomic_store_n(&loop->pending_length, length, __ATOMIC_SEQ_CST);
    unsigned int expected = FX_LOOP_CMD_NONE;
    return __atomic_compare_exchange_n(&loop->command, &expected, command, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void fx_loop_set_level(struct fx_loop *loop, unsigned int permille)
{ __atomic_store_n(&loop->level_permille, permille > 1000u ? 1000u : permille, __ATOMIC_SEQ_CST); }

static inline unsigned int fx_loop_in_range(const struct fx_loop *loop,
                                            const struct fx_loop_slot *slot, unsigned int at)
{
    unsigned int d = at >= slot->start ? at - slot->start : at + loop->length - slot->start;
    return d < slot->written;
}

static inline void fx_loop_reset(struct fx_loop *loop)
{
    loop->state = FX_LOOP_EMPTY;
    loop->depth = 0u;
    loop->overdub = 0u;
    loop->merging = 0u;
    loop->position = loop->recorded = 0u;
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++) {
        loop->slot[i].used = 0u;
        loop->slot[i].written = 0u;
        loop->slot[i].gain = 1.0f;
        loop->slot[i].gain_step = 0.0f;
    }
}

static inline int fx_loop_free_slot(const struct fx_loop *loop)
{
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++)
        if (!loop->slot[i].used && loop->slot[i].frames &&
            !(loop->merging && loop->merge_slot == i))
            return (int)i;
    return -1;
}

static inline void fx_loop_end_session(struct fx_loop *loop)
{
    if (loop->overdub) loop->record_step = -1.0f / (float)FX_LOOP_EDGE;
}

static inline void fx_loop_begin_session(struct fx_loop *loop)
{
    if (loop->state != FX_LOOP_PLAY || loop->overdub) return;
    if (loop->depth >= FX_LOOP_LAYERS && !loop->merging) {
        /* Fold the oldest layer into the base, chunk by chunk. */
        loop->merge_slot = loop->stack[0];
        loop->merging = 1u;
        for (unsigned int i = 1; i < loop->depth; i++) loop->stack[i - 1u] = loop->stack[i];
        loop->depth--;
    }
    int free = fx_loop_free_slot(loop);
    if (free < 0) {
        /* Sessions faster than a merge: keep adding to the newest layer. */
        if (!loop->depth) return;
        if (loop->slot[loop->stack[loop->depth - 1u]].written < loop->length) return;
        loop->session_slot = loop->stack[loop->depth - 1u];
        loop->session_elapsed = loop->length;   /* add, never overwrite */
    } else {
        struct fx_loop_slot *slot = &loop->slot[free];
        slot->used = 1u;
        slot->start = loop->position;
        slot->written = 0u;
        slot->gain = 1.0f;
        slot->gain_step = 0.0f;
        loop->stack[loop->depth++] = (unsigned int)free;
        loop->session_slot = (unsigned int)free;
        loop->session_elapsed = 0u;
    }
    loop->overdub = 1u;
    loop->record_gain = 0.0f;
    loop->record_step = 1.0f / (float)FX_LOOP_EDGE;
}

static inline void fx_loop_take(struct fx_loop *loop)
{
    unsigned int command = __atomic_exchange_n(&loop->command, FX_LOOP_CMD_NONE, __ATOMIC_SEQ_CST);
    switch (command) {
    case FX_LOOP_CMD_RECORD: {
        unsigned int length = __atomic_load_n(&loop->pending_length, __ATOMIC_SEQ_CST);
        if (!length || length + FX_RMX_SEAM + 1u > loop->capacity) break;
        fx_loop_reset(loop);
        loop->length = length;
        loop->state = FX_LOOP_RECORD;
        loop->playing_gain = 1.0f;
        loop->playing_step = 0.0f;
        loop->clear_after_fade = 0u;
        break;
    }
    case FX_LOOP_CMD_OVERDUB_ON: fx_loop_begin_session(loop); break;
    case FX_LOOP_CMD_OVERDUB_OFF: fx_loop_end_session(loop); break;
    case FX_LOOP_CMD_UNDO:
        if (loop->overdub) { loop->overdub = 0u; loop->record_gain = 0.0f; }
        if (loop->depth) {
            struct fx_loop_slot *slot = &loop->slot[loop->stack[loop->depth - 1u]];
            slot->gain_step = -1.0f / (float)FX_RMX_RAMP;
        }
        break;
    case FX_LOOP_CMD_CLEAR:
        if (loop->state == FX_LOOP_EMPTY) break;
        if (loop->state == FX_LOOP_RECORD) { fx_loop_reset(loop); break; }
        loop->clear_after_fade = 1u;
        loop->playing_step = -1.0f / (float)FX_RMX_RAMP;
        break;
    case FX_LOOP_CMD_STOP:
        if (loop->state == FX_LOOP_PLAY) loop->playing_step = -1.0f / (float)FX_RMX_RAMP;
        fx_loop_end_session(loop);
        break;
    case FX_LOOP_CMD_PLAY:
        if (loop->state == FX_LOOP_PLAY) loop->playing_step = 1.0f / (float)FX_RMX_RAMP;
        break;
    }
}

/* Fold up to one chunk of the merging layer into the base. Each frame moves
 * from the layer to the base between two reads, so playback never hears it
 * twice or not at all. */
static inline void fx_loop_merge(struct fx_loop *loop)
{
    if (!loop->merging) return;
    struct fx_loop_slot *slot = &loop->slot[loop->merge_slot];
    unsigned int count = slot->written < FX_LOOP_MERGE_CHUNK ? slot->written : FX_LOOP_MERGE_CHUNK;
    unsigned int at = slot->start;
    for (unsigned int i = 0; i < count; i++) {
        loop->base[at].left += slot->frames[at].left * slot->gain;
        loop->base[at].right += slot->frames[at].right * slot->gain;
        if (++at >= loop->length) at = 0u;
    }
    slot->start = at;
    slot->written -= count;
    if (!slot->written) {
        slot->used = 0u;
        loop->merging = 0u;
    }
}

/* `source` is what the loop records and overdubs; `out` receives the loop,
 * added at its level. They may not alias. */
static inline void fx_loop_process(struct fx_loop *loop, const struct fx_frame *source,
                            struct fx_frame *out, unsigned int frames)
{
    if (!loop->capacity) return;
    fx_loop_take(loop);
    loop->level_target = (float)__atomic_load_n(&loop->level_permille, __ATOMIC_SEQ_CST) * 0.001f;
    if (loop->state == FX_LOOP_EMPTY) goto publish;
    fx_loop_merge(loop);
    for (unsigned int i = 0; i < frames; i++) {
        struct fx_frame in = source ? source[i] : (struct fx_frame){0.0f, 0.0f};
        loop->level += (loop->level_target - loop->level) * 0.002f;
        if (loop->state == FX_LOOP_RECORD) {
            unsigned int t = loop->recorded;
            if (t < loop->length) {
                loop->base[t] = in;
            } else {
                /* The first repetition fades in over the seam, while the
                   frames after the loop's end are blended into its head. */
                unsigned int k = t - loop->length;
                float f = ((float)k + 0.5f) * (1.0f / (float)FX_RMX_SEAM);
                struct fx_frame head = loop->base[k];
                loop->base[k].left = head.left * f + in.left * (1.0f - f);
                loop->base[k].right = head.right * f + in.right * (1.0f - f);
                out[i].left += head.left * f * loop->level * loop->playing_gain;
                out[i].right += head.right * f * loop->level * loop->playing_gain;
                if (k + 1u >= FX_RMX_SEAM) {
                    loop->state = FX_LOOP_PLAY;
                    loop->position = FX_RMX_SEAM >= loop->length ? 0u : FX_RMX_SEAM;
                }
            }
            loop->recorded++;
            continue;
        }
        unsigned int at = loop->position;
        float l = loop->base[at].left, r = loop->base[at].right;
        for (unsigned int n = 0; n < loop->depth; n++) {
            struct fx_loop_slot *slot = &loop->slot[loop->stack[n]];
            if (slot->gain_step != 0.0f) {
                slot->gain += slot->gain_step;
                if (slot->gain < 0.0f) slot->gain = 0.0f;
            }
            if (!fx_loop_in_range(loop, slot, at)) continue;
            l += slot->frames[at].left * slot->gain;
            r += slot->frames[at].right * slot->gain;
        }
        if (loop->merging) {
            struct fx_loop_slot *slot = &loop->slot[loop->merge_slot];
            if (fx_loop_in_range(loop, slot, at)) {
                l += slot->frames[at].left * slot->gain;
                r += slot->frames[at].right * slot->gain;
            }
        }
        if (loop->overdub) {
            /* Recorded after the read, so this pass plays the old content and
               the next pass hears the new. */
            struct fx_loop_slot *slot = &loop->slot[loop->session_slot];
            loop->record_gain += loop->record_step;
            if (loop->record_gain >= 1.0f) { loop->record_gain = 1.0f; loop->record_step = 0.0f; }
            float g = loop->record_gain;
            if (loop->session_elapsed < loop->length) {
                slot->frames[at].left = in.left * g;
                slot->frames[at].right = in.right * g;
                loop->session_elapsed++;
                if (slot->written < loop->length) slot->written++;
            } else {
                slot->frames[at].left += in.left * g;
                slot->frames[at].right += in.right * g;
            }
            if (g <= 0.0f && loop->record_step < 0.0f) {
                loop->overdub = 0u;
                loop->record_step = 0.0f;
            }
        }
        if (loop->playing_step != 0.0f) {
            loop->playing_gain += loop->playing_step;
            if (loop->playing_gain >= 1.0f) { loop->playing_gain = 1.0f; loop->playing_step = 0.0f; }
            if (loop->playing_gain <= 0.0f) {
                loop->playing_gain = 0.0f;
                loop->playing_step = 0.0f;
                if (loop->clear_after_fade) { fx_loop_reset(loop); goto publish; }
            }
        }
        float g = loop->level * loop->playing_gain;
        out[i].left += l * g;
        out[i].right += r * g;
        if (++loop->position >= loop->length) loop->position = 0u;
    }
    /* Layers whose undo fade has finished leave the stack. */
    while (loop->depth) {
        struct fx_loop_slot *slot = &loop->slot[loop->stack[loop->depth - 1u]];
        if (slot->gain_step == 0.0f || slot->gain > 0.0f) break;
        slot->gain = 1.0f;
        slot->gain_step = 0.0f;
        slot->used = 0u;
        slot->written = 0u;
        loop->depth--;
    }
publish:
    loop->shown_state = loop->state;
    loop->shown_layers = loop->depth + (loop->merging ? 1u : 0u);
    loop->shown_overdub = loop->overdub;
}

#endif /* FX_RMX_H */
