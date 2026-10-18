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
   accessor the RMX X-PAD and Now Playing read; its prologue is checked first,
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

/* COLOR in 0..200 with 100 at centre, PARAMETER in 0..100. The physical
   knobs are not readable yet, so COLOR 2 runs at a depth set on the drive
   (buttons.txt, depth=), +50 by default. */
#define FX2_COLOR_MAX 200u
#define FX2_COLOR_CENTRE 100u
#define FX2_PARAM_MAX 100u
#define FX2_DEFAULT_DEPTH 150u

/* Engines that keep a tail own delay memory; the others need none. */
static inline int fx2_kind_has_delay(unsigned int kind)
{ return kind == FX_COLOR_DUB_ECHO || kind == FX_COLOR_SPACE; }

/* The six buttons, in the order the mixer prints them left to right. */
static const unsigned int fx2_button_order[FX_COLOR_KINDS] = {
    FX_COLOR_SPACE, FX_COLOR_DUB_ECHO, FX_COLOR_SWEEP, FX_COLOR_NOISE, FX_COLOR_CRUSH, FX_COLOR_FILTER
};

/* What is on, per deck: COLOR 2's effect, and the depth it runs at. */
struct fx2_deck {
    volatile unsigned int on, kind, color, param;
};

/* The input probe: the first distinct key codes the panel reports, how many
   times each came and the further arguments it last carried (a knob turned
   shows as one code arriving often, with a changing argument), and the LED
   IDs the unit-wide refresh carries. Kept for the diagnostic worker to write
   out: the input thread never writes a file. */
#define FX2_PROBE_KEYS 48u
#define FX2_PROBE_LEDS 64u
struct fx2_probe_entry {
    unsigned int key, operation, channel, count;
    unsigned int arguments[3];
};
struct fx2_probe_led { unsigned int id, channel; };

#endif /* RX3_FX2_DECL_H */
