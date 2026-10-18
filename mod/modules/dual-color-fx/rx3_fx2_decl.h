/* SPDX-License-Identifier: MPL-2.0
 * COLOR 2, a second Sound Color FX per channel: declarations and state,
 * private to rx3_fx2_module.c.
 *
 * The mixer's own Color FX stays exactly as it is, on its own buttons and
 * knobs. COLOR 2 runs in software on the deck's stream, before the mixer, so
 * on a channel the order is: deck -> roll (if any) -> COLOR 2 -> trim, EQ,
 * native Color FX, fader. Two effects in series, the second one native.
 */
#ifndef RX3_FX2_DECL_H
#define RX3_FX2_DECL_H

#define FX2_PRIORITY 60u               /* after Stems (50) and the roll (55) */

/* The deck's BPM in hundredths, for the echo time and the gate. The same
   accessor ROLL / LOOP and Now Playing read; its prologue is checked first,
   and a value outside 40..300 BPM leaves the engines at their last tempo. */
#define FX2_PLAY_BPM ((unsigned long)0x000fd1fc)
static const uint8_t fx2_play_bpm_guard[8] = {
    0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1
};
typedef int (*fx2_deck_int_fn)(unsigned int);
/* Reached through these, which the offline tests point elsewhere. */
static fx2_deck_int_fn fx2_bpm_reader = (fx2_deck_int_fn)FX2_PLAY_BPM;
static const void *fx2_bpm_prologue = (const void *)FX2_PLAY_BPM;
#define FX2_MEMORY_FLOOR_KB 32768u

/* Controls in the row's own units. COLOR is 0..200 with 100 at centre. */
#define FX2_COLOR_MAX 200u
#define FX2_COLOR_CENTRE 100u
#define FX2_PARAM_MAX 100u

/* Engines that keep a tail own delay memory; the others need none. */
static inline int fx2_kind_has_delay(unsigned int kind)
{ return kind == FX_COLOR_DUB_ECHO || kind == FX_COLOR_SPACE; }

/* The six buttons, in the order the mixer prints them left to right. */
static const unsigned int fx2_button_order[FX_COLOR_KINDS] = {
    FX_COLOR_SPACE, FX_COLOR_DUB_ECHO, FX_COLOR_SWEEP, FX_COLOR_NOISE, FX_COLOR_CRUSH, FX_COLOR_FILTER
};
static const uint16_t fx2_names[FX_COLOR_KINDS][9] = {
    {'F','I','L','T','E','R',0}, {'D','U','B',' ','E','C','H','O',0}, {'S','P','A','C','E',0},
    {'C','R','U','S','H',0}, {'N','O','I','S','E',0}, {'S','W','E','E','P',0}
};

/* What the DJ set, per deck. `kind` selects; each kind keeps the COLOR and
   PARAMETER it was last left at, so going back to an effect picks it up
   where it was rather than at the centre. */
struct fx2_deck {
    volatile unsigned int on, kind;
    volatile unsigned int color[FX_COLOR_KINDS], param[FX_COLOR_KINDS];
};

/* The input probe: the first distinct key codes the panel reports, kept for
   the diagnostic worker to write out. The input thread never writes a file. */
#define FX2_PROBE_KEYS 48u
struct fx2_probe_entry { unsigned int key, operation, channel; };

#endif /* RX3_FX2_DECL_H */
