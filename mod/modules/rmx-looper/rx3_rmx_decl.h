/* SPDX-License-Identifier: MPL-2.0
 * RMX X-PAD: declarations and shared state, private to rx3_rmx_module.c.
 *
 * Threads that touch this state, and what each may do:
 *   audio   the deck streams and the master bus: engines only, no I/O
 *   input   pads, SHIFT, the touch row: atomics and engine requests only
 *   worker  this module's own thread: the BPM accessor and nothing else
 *   startup start() and stop(): allocation, hooks, logging
 */
#ifndef RX3_RMX_DECL_H
#define RX3_RMX_DECL_H

/* The deck's current BPM in hundredths, the accessor Now Playing reads. Its
   prologue is checked before the first call. Upstream records it as read
   from the 1.19 binary and not yet observed on a deck, so a value outside
   40..300 BPM is treated as unknown and the engines fall back to 120. */
#define RMX_PLAY_BPM ((unsigned long)0x000fd1fc)
static const uint8_t rmx_play_bpm_guard[8] = {
    0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1
};
typedef int (*rmx_deck_int_fn)(unsigned int);
/* Reached through these, which the offline tests point elsewhere. */
static rmx_deck_int_fn rmx_bpm_reader = (rmx_deck_int_fn)RMX_PLAY_BPM;
static const void *rmx_bpm_prologue = (const void *)RMX_PLAY_BPM;

/* Places on the audio stages. Deck: Stems (50) mixes first, the roll inserts
   after it (55), the second Color FX after the roll (60) so a filter can be
   swept over a roll, and the loop's tap last (65), hearing all of it. Master:
   Samples (50) first, then the master roll and the X-PAD (60), so a sample
   can be rolled or overdubbed like anything else on the bus. */
#define RMX_DECK_ROLL_PRIORITY 55u
#define RMX_DECK_TAP_PRIORITY 65u
#define RMX_MASTER_PRIORITY 60u

/* Buffers are sized at start for this rate; a faster device keeps the same
   frames and so a little less time, which the engines clamp to. */
#define RMX_SIZING_RATE 48000.0f
/* Free memory that must remain after the reservation, in KiB. */
#define RMX_MEMORY_FLOOR_KB 65536u

#define RMX_SOURCE_CH1 0u
#define RMX_SOURCE_CH2 1u
#define RMX_SOURCE_MASTER 2u
#define RMX_ROLLS 3u                  /* deck 1, deck 2, master */

/* The X-PAD strip's five sections, as on the RMX-1000: 1/8 to 2/1 of a beat.
   With SHIFT held as the finger lands, the strip plays an octave finer. */
#define RMX_STRIP_SECTIONS 5u
static const float rmx_strip_beats[RMX_STRIP_SECTIONS] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f};
static const uint16_t rmx_strip_text[2][RMX_STRIP_SECTIONS][5] = {
    {{'1','/','8',0}, {'1','/','4',0}, {'1','/','2',0}, {'1','/','1',0}, {'2','/','1',0}},
    {{'1','/','1','6',0}, {'1','/','8',0}, {'1','/','4',0}, {'1','/','2',0}, {'1','/','1',0}}
};

/* The X-PAD FX buttons, left to right as on the RMX: ROLL, then the sounds. */
#define RMX_PICK_ROLL 0u
#define RMX_PICKS (1u + FX_XPAD_SOUNDS)
static const char *const rmx_sound_files[FX_XPAD_SOUNDS] = {"kick.wav", "snare.wav", "clap.wav", "hat.wav"};
/* A sound from the drive is at most this long and this many bytes. */
#define RMX_WAV_MAX_SECONDS 4u
#define RMX_WAV_MAX_BYTES (48000u * 4u * RMX_WAV_MAX_SECONDS + 4096u)

/* A deck's rolled signal reaches the OVERDUB through one of these: written by
   the deck's audio thread, read by the master's. They may be one thread or
   two; this does not assume either. */
#define RMX_FIFO_FRAMES 16384u        /* a power of two */
#define RMX_FIFO_BACKLOG 8192u        /* past this the reader skips ahead */
#define RMX_FIFO_KEEP 2048u
struct rmx_fifo {
    struct fx_frame data[RMX_FIFO_FRAMES];
    unsigned int head, tail;          /* free-running; head written by the deck */
    unsigned int flush;               /* asked by the input thread, done by the reader */
    unsigned int underruns, overruns, skips;
};

/* Work in chunks no larger than this, so the scratch copy stays small. */
#define RMX_CHUNK 1024u

/* Overload guard: a master block that takes more than this share of its own
   duration, this many times running, bypasses the module until restart. */
#define RMX_OVERLOAD_SHARE 0.5f
#define RMX_OVERLOAD_RUN 16u

#endif /* RX3_RMX_DECL_H */
