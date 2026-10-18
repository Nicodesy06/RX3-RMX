# SPDX-License-Identifier: MPL-2.0
"""ROLL / LOOP FX and COLOR 2, run offline against mock services.

These drive the modules through the same callbacks the core gives them --
pads, keys, the touch row, the deck streams and the master bus -- and measure
the audio that comes out. They establish the logic and the arithmetic on this
computer. They say nothing about timing, CPU or sound on an RX3.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests import test_framework

ROOT = pathlib.Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <stdio.h>
#include "core/api/rx3_module_api.h"
#include "core/api/rx3_fx_math.h"
static unsigned claims, releases, rows, keys, pads, leds, modes, unregisters, refuse_memory;
static const struct rx3_pad_row *row;
static rx3_pad_handler pad;
static rx3_key_handler key;
static rx3_light_fn light;
static struct { const void *owner; unsigned priority; rx3_deck_stream_fn fn; } decks[4];
static unsigned deck_count;
static rx3_master_fn master;
static unsigned master_priority;
static int shift;
static char last_log[256];
static int register_row(const struct rx3_pad_row *r) { rows++; row = r; return 1; }
static void unregister_row(const struct rx3_pad_row *r) { if (r == row) row = 0; }
static int register_pad(const void *o, unsigned p, rx3_pad_handler h) { assert(o && h && p); pads++; pad = h; return 1; }
static int register_key(const void *o, unsigned p, rx3_key_handler h) { assert(o && h && p); keys++; key = h; return 1; }
static int register_lights(const void *o, unsigned r, rx3_light_fn f) { assert(o && f && r == RX3_LIGHTS_PADS); leds++; light = f; return 1; }
static int claim_modes(const void *o) { assert(o); modes++; return 1; }
static void unregister_owner(const void *o) { assert(o); unregisters++; pad = 0; key = 0; light = 0; }
static int shift_held(unsigned c) { (void)c; return shift; }
static int blink_on(void) { return 1; }
static void blink_restart(void) {}
static int claim_deck_at(const void *o, unsigned p, rx3_deck_stream_fn f) {
    assert(o && f && deck_count < 4); for (unsigned i = 0; i < deck_count; i++) assert(decks[i].owner != o);
    unsigned at = deck_count++;
    while (at && decks[at - 1].priority > p) { decks[at] = decks[at - 1]; at--; }
    decks[at].owner = o; decks[at].priority = p; decks[at].fn = f; claims++; return 1;
}
static void release_deck(const void *o) {
    for (unsigned i = 0; i < deck_count; i++) if (decks[i].owner == o) {
        for (unsigned j = i; j + 1 < deck_count; j++) decks[j] = decks[j + 1];
        deck_count--; releases++; return;
    }
}
static int claim_master_at(const void *o, unsigned p, rx3_master_fn f) { assert(o && f && !master); master = f; master_priority = p; claims++; return 1; }
static void release_master(const void *o) { assert(o); if (master) releases++; master = 0; }
static int reserve(const void *o, unsigned long b, unsigned long f) { (void)o; (void)b; (void)f; return !refuse_memory; }
static void settle(const void *o, unsigned long b) { (void)o; (void)b; }
static unsigned long available(void) { return 1u << 20; }
static unsigned long held(void) { return 0; }
static void log_text(const char *s) { assert(s); snprintf(last_log, sizeof(last_log), "%s", s); }
static void log_value(const char *s, unsigned long v) { (void)v; assert(s); }
static const struct rx3_panel_service panels = {.register_row = register_row, .unregister_row = unregister_row};
static void key_arguments(unsigned out[3]);
static int register_led_filter(const void *o, rx3_led_filter_fn f);
static const struct rx3_input_service input = {.register_pad = register_pad, .register_key = register_key,
    .register_lights = register_lights, .claim_mode_keys = claim_modes, .unregister_owner = unregister_owner,
    .shift_held = shift_held, .blink_on = blink_on, .blink_restart = blink_restart,
    .register_led_filter = register_led_filter, .key_arguments = key_arguments};
static const struct rx3_audio_service audio = {.release_deck_stream = release_deck,
    .release_master = release_master, .claim_deck_stream_at = claim_deck_at, .claim_master_at = claim_master_at};
static const struct rx3_memory_service memory = {reserve, settle, settle, settle, available, held};
static unsigned band_mask = 7u;
static unsigned mask_of(unsigned d) { (void)d; return band_mask; }
static const struct rx3_band_service bands = {.mask = mask_of};
static unsigned key_args[3];
static void key_arguments(unsigned out[3]) { out[0] = key_args[0]; out[1] = key_args[1]; out[2] = key_args[2]; }
static rx3_led_filter_fn led_filter;
static int register_led_filter(const void *o, rx3_led_filter_fn f) { assert(o && f); led_filter = f; return 1; }
static const struct rx3_services services = {.log_line = log_text, .log_number = log_value,
    .panels = &panels, .input = &input, .audio = &audio, .memory = &memory, .bands = &bands};
static const uint8_t prologue[8] = {0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1};
static int bpm(unsigned d) { return d == 0 ? 12800 : 9000; }

/* One deck block through every client on its stream, in priority order. */
static void run_deck(unsigned deck, struct rx3_stereo *out, unsigned n) {
    for (unsigned i = 0; i < deck_count; i++) decks[i].fn(deck, 0, 0, out, n);
}
/* The engines' own sine, so the harness needs no libm. */
static void tone(struct rx3_stereo *out, unsigned n, unsigned t0, float hz) {
    for (unsigned i = 0; i < n; i++) {
        double turns = (double)hz * (double)(t0 + i) / 44100.0;
        out[i].left = out[i].right = 0.4f * fx_sin_turns((float)(turns - (double)(long long)turns));
    }
}
static struct rx3_pad_event press(unsigned code, unsigned deck) {
    struct rx3_pad_event e = {code, 0, deck + 1, deck, 0}; return e;
}
static struct rx3_pad_event lift(unsigned code, unsigned deck) {
    struct rx3_pad_event e = {code, 2, deck + 1, deck, 0}; return e;
}
'''


class PerformanceFxTests(unittest.TestCase):
    run_units = test_framework.FrameworkTests.run_units

    def test_engines_offline(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler required')
        with tempfile.TemporaryDirectory() as temporary:
            binary = pathlib.Path(temporary) / 'fx'
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/fx/fx_engine_test.c'), '-lm', '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('all fx engine checks passed', result.stdout)

    def test_rmx_xpad_module_through_its_callbacks(self):
        self.run_units(MOCKS + r'''
#include "rmx-looper/rx3_rmx_module.c"
#include "core/ui/rx3_pad_layout.h"
static struct rx3_stereo in[512], out[512], bus[512];
static void master_block(void) { memset(bus, 0, sizeof(bus)); master(bus, 512); }
static float bus_peak(void) { float p = 0; for (unsigned i = 0; i < 512; i++) if (fx_absf(bus[i].left) > p) p = fx_absf(bus[i].left); return p; }
int main(void) {
    rmx_bpm_reader = bpm; rmx_bpm_prologue = prologue;
    setenv("RX3_RMX", "1", 1);
    assert(rx3_rmx_module.start(&services) == 1);
    assert(deck_count == 2 && decks[0].priority == 55 && decks[1].priority == 65);
    assert(master && master_priority == 60);
    /* Twelve controls: five X-PAD FX buttons, a five-section strip, PITCH, VOL. */
    assert(row && row->panel_id == 2 && row->count == 12 && row->strip);
    assert(row->widgets[RMX_W_PICK].kind == RX3_PAD_TOGGLE && row->widgets[RMX_W_STRIP].kind == RX3_PAD_STRIP);
    assert(row->tab_slots[RX3_TAB_PAIR_LEFT] == 11 && pad && light && modes == 1);
    /* Across the screen, no control may straddle the two deck windows: the
       core clips each half into its own, and a caption across x=640 is cut. */
    {
        unsigned char weights[RX3_PAD_CELL_MAX];
        struct rx3_pad_cell cells[RX3_PAD_CELL_MAX];
        for (unsigned i = 0; i < row->count; i++) weights[i] = row->widgets[i].weight;
        assert(rx3_pad_solve(weights, (int)row->count, RX3_PAD_SCREEN_ORIGIN, RX3_PAD_SCREEN_SPAN, cells) == 12);
        for (unsigned i = 0; i < 12; i++) assert(!(cells[i].x1 <= 640 && cells[i].x2 >= 639));
    }
    rx3_rmx_module.audio_started(44100);
    for (unsigned i = 0; i < 200 && rmx_bpm_x100[0] != 12800; i++) usleep(10000);
    assert(rmx_bpm_x100[0] == 12800);
    const uint16_t *c = row->caption(0, RMX_W_PICK, 0);
    assert(c[0] == 'R' && c[5] == '1');                  /* ROLL names its source */
    c = row->caption(0, RMX_W_STRIP, 0); assert(c[0] == '1' && c[2] == '8');

    /* ROLL is chosen; the strip at 1/4 rolls deck 1 at 1/4 beat of 128 BPM. */
    assert(rmx_pick == RMX_PICK_ROLL);
    row->strip(0, RMX_W_STRIP + 1, 1);
    assert(rmx_strip_roll == 1 && rmx_section == 1 && row->is_on(0, RMX_W_STRIP + 1, 0));
    const unsigned length = 5168;
    static float history[44100];
    unsigned t = 0;
    for (; t < 44100 - 512; t += 512) {
        tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512);
        for (unsigned i = 0; i < 512; i++) history[t + i] = out[i].left;
        master_block();
    }
    float worst = 0;
    for (unsigned i = length + 600; i + length < t; i++) {
        float d = fx_absf(history[i] - history[i + length]); if (d > worst) worst = d;
    }
    assert(worst < 1e-5f);
    /* Sliding to 1/2 keeps the roll and doubles the slice. */
    row->strip(0, RMX_W_STRIP + 2, 1);
    assert(rmx_strip_roll == 1 && rmx_section == 2 && (rmx_rolls[0].request & FX_ROLL_LENGTH_MASK) == rmx_frames_of_beats(0.5f));
    /* Lifting stops it: the deck is back exactly after the ramp. */
    row->strip(0, RMX_W_STRIP + 2, 0);
    assert(!rmx_strip_roll && rmx_section == -1);
    for (unsigned b = 0; b < 5; b++) { tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512); t += 512; }
    tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512);
    assert(!memcmp(in, out, sizeof(in)));

    /* A sound button plays at once and becomes the strip's sound. */
    row->fire(0, RMX_W_PICK + 1, 0);
    assert(rmx_pick == 1);
    master_block(); assert(bus_peak() > 0.1f);
    /* The strip now repeats the kick; HOLD (pad 6 tapped) keeps it after the lift. */
    row->activate(1);
    struct rx3_pad_event e = press(0x411c, 0); assert(pad(&e)); e = lift(0x411c, 0); assert(pad(&e));
    assert(rmx_hold);
    row->strip(0, RMX_W_STRIP + 0, 1); row->strip(0, RMX_W_STRIP + 0, 0);
    assert(rmx_strip_effect == 2 && rmx_xpad.repeat_word != 0);
    unsigned hits = 0; float newest = 1e9f;
    for (unsigned b = 0; b < 80; b++) {
        master_block();
        /* A retrigger shows as the youngest kick voice starting over. */
        float young = 1e9f;
        for (unsigned v = 0; v < FX_XPAD_VOICES; v++)
            if (rmx_xpad.voices[v].active && rmx_xpad.voices[v].sound == 0 && rmx_xpad.voices[v].fade == 0.0f &&
                rmx_xpad.voices[v].position < young) young = rmx_xpad.voices[v].position;
        if (young < newest) hits++;
        newest = young;
    }
    assert(rmx_xpad.repeat_interval == rmx_frames_of_beats(0.125f));
    assert(hits >= 12);                    /* 80 blocks of 512 hold 15 eighths at 128 BPM */
    /* HOLD off stops it. */
    e = press(0x411c, 0); assert(pad(&e)); e = lift(0x411c, 0); assert(pad(&e));
    assert(!rmx_hold && !rmx_strip_effect && rmx_xpad.repeat_word == 0);

    /* OVERDUB from the pads: standby, the first hit starts it, a track per sound. */
    e = press(0x411d, 0); assert(pad(&e)); e = lift(0x411d, 0); assert(pad(&e));
    master_block(); assert(rmx_xpad.shown_state == FX_PATTERN_STANDBY);
    struct rx3_light l = {RX3_LIGHT_NATIVE, 0};
    light(0, 6, &l); assert(l.state == RX3_LIGHT_BLINK);
    e = press(0x4117, 0); assert(pad(&e));            /* KICK */
    for (unsigned b = 0; b < 30; b++) master_block();
    assert(rmx_xpad.shown_state == FX_PATTERN_RUN && (rmx_xpad.shown_tracks & 1u));
    assert(rmx_xpad.length == rmx_frames_of_beats(4.0f));
    l.state = RX3_LIGHT_NATIVE; light(0, 6, &l); assert(l.state == RX3_LIGHT_ON);
    /* HOLD + KICK mutes the kick's track and does not toggle HOLD. */
    e = press(0x411c, 0); assert(pad(&e));
    e = press(0x4117, 0); assert(pad(&e));
    e = lift(0x411c, 0); assert(pad(&e));
    assert(!rmx_hold && (rmx_xpad.muted & 1u));
    l.state = RX3_LIGHT_NATIVE; light(0, 0, &l); assert(l.state == RX3_LIGHT_BLINK);
    /* OVERDUB + KICK deletes it and leaves the loop running. */
    e = press(0x411d, 0); assert(pad(&e));
    e = press(0x4117, 0); assert(pad(&e));
    e = lift(0x411d, 0); assert(pad(&e));
    for (unsigned b = 0; b < 60; b++) master_block();
    assert(rmx_xpad.shown_state == FX_PATTERN_RUN && !(rmx_xpad.shown_tracks & 1u));
    /* OVERDUB alone stops and deletes it all. */
    e = press(0x411d, 0); assert(pad(&e)); e = lift(0x411d, 0); assert(pad(&e));
    master_block(); assert(rmx_xpad.shown_state == FX_PATTERN_OFF);

    /* SOURCE (pad 8) to CH2 and MST; ROLL's caption follows. */
    e = press(0x411e, 1); assert(pad(&e)); assert(rmx_source == RMX_SOURCE_CH2);
    e = press(0x411e, 1); assert(pad(&e)); assert(rmx_source == RMX_SOURCE_MASTER);
    c = row->caption(0, RMX_W_PICK, 0); assert(c[5] == 'M');
    /* PITCH: SHIFT + 7 up, the slider, a tap back to 0; it reaches the rolls. */
    shift = 1; e = press(0x411d, 0); assert(pad(&e)); shift = 0;
    assert(rmx_semitones == 1 && rmx_rolls[2].pitch_cents == 100 && rmx_xpad.pitch_cents == 100);
    row->slider_set(0, RMX_W_PITCH, 5, 1); assert(rmx_semitones == -7);
    c = row->caption(0, RMX_W_PITCH, 0); assert(c[6] == '-' && c[7] == '7');
    row->fire(0, RMX_W_PITCH, 0); assert(rmx_semitones == 0);
    /* VOL: the slider sets it, a tap mutes. */
    row->slider_set(0, RMX_W_VOLUME, 50, 1); assert(rmx_xpad.volume_permille == 500);
    row->fire(0, RMX_W_VOLUME, 0); assert(rmx_xpad.volume_permille == 0);
    c = row->caption(0, RMX_W_VOLUME, 0); assert(c[0] == 'M');
    /* SHIFT on landing plays the strip an octave finer. */
    rmx_cycle_source();                                       /* back to CH1 */
    row->fire(0, RMX_W_PICK, 0);
    shift = 1; row->strip(0, RMX_W_STRIP, 1); shift = 0;
    assert(rmx_section_fine && (rmx_rolls[0].request & FX_ROLL_LENGTH_MASK) == rmx_frames_of_beats(0.0625f));
    c = row->caption(0, RMX_W_STRIP, 0); assert(c[2] == '1' && c[3] == '6');
    row->strip(0, RMX_W_STRIP, 0);

    /* Off the panel the pads belong to the player. */
    row->activate(0);
    e = press(0x4117, 0); assert(!pad(&e));
    rx3_rmx_module.stop();
    assert(deck_count == 0 && !master && !row);

    static const uint8_t other[8] = {0};
    rmx_bpm_prologue = other;
    assert(rx3_rmx_module.start(&services) == 0);
    rx3_rmx_module.stop();
    rmx_bpm_prologue = prologue; refuse_memory = 1; claims = 0; rows = 0;
    assert(rx3_rmx_module.start(&services) == 1 && claims == 0 && rows == 0);
    rx3_rmx_module.stop();
    return 0;
}
''', [], ['-pthread', '-Wno-unused-function'])

    def test_color2_chord_through_its_callbacks(self):
        self.run_units(MOCKS + r'''
#include "dual-color-fx/rx3_fx2_module.c"
static struct rx3_stereo in[4096], out[4096];
/* Mean square, compared against squared thresholds. */
static float power(const struct rx3_stereo *x, unsigned n) {
    double s = 0; for (unsigned i = 0; i < n; i++) s += x[i].left * x[i].left; return (float)(s / n);
}
static int send(unsigned code, unsigned operation) {
    struct rx3_key_event k = {code, operation, 1}; return key(&k);
}
/* The mixer's order: SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER. */
enum { SPACE = 0x4180, ECHO, SWEEP, NOISE, CRUSH, FILTER };
int main(void) {
    fx2_bpm_reader = bpm; fx2_bpm_prologue = prologue;
    setenv("RX3_FX2", "1", 1);
    setenv("RX3_FX2_BUTTONS", "0x4180,4181,4182,4183,4184,4185", 1);
    setenv("RX3_FX2_LEDS", "21,22,23,24,25,26", 1);
    setenv("RX3_FX2_DEPTH", "40", 1);
    setenv("RX3_FX2_PROBE", "1", 1);
    assert(rx3_fx2_module.start(&services) == 1);
    /* No screen: nothing registered on the touchscreen. */
    assert(!row && deck_count == 1 && decks[0].priority == 60 && key && led_filter);
    assert(fx2_buttons_known && fx2_leds_known && fx2_depth == 40);
    rx3_fx2_module.audio_started(44100);

    /* Off: bit-exact. */
    tone(in, 4096, 0, 5000.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));

    /* One button alone is the mixer's: passed through, nothing starts. */
    assert(!send(FILTER, 0)); assert(!fx2_decks[0].on);
    /* Holding it, a second button is a chord: consumed, COLOR 2 = DUB ECHO
       on both channels, the mixer keeps FILTER. */
    assert(send(ECHO, 0));
    assert(fx2_decks[0].on && fx2_decks[1].on && fx2_decks[0].kind == FX_COLOR_DUB_ECHO);
    assert(send(ECHO, 2));                       /* its release, consumed too */
    assert(!send(FILTER, 2));                    /* the mixer's own release */
    /* It plays at the depth from the drive: echoes ring after the input stops. */
    for (unsigned b = 0; b < 4; b++) { tone(out, 4096, b * 4096, 440.0f); run_deck(0, out, 4096); }
    memset(out, 0, sizeof(out)); run_deck(0, out, 4096);
    assert(power(out, 4096) > 1e-6f);
    /* Its light is lit beside the mixer's: DUB ECHO is the second LED. */
    struct rx3_light l = {RX3_LIGHT_NATIVE, 0};
    /* LED IDs are hexadecimal, as the probe prints them. */
    led_filter(0x22, 1, &l); assert(l.state == RX3_LIGHT_ON);
    l.state = RX3_LIGHT_NATIVE; led_filter(0x26, 1, &l); assert(l.state == RX3_LIGHT_NATIVE);
    /* Another button alone still goes to the mixer; COLOR 2 stays. */
    assert(!send(CRUSH, 0)); assert(!send(CRUSH, 2)); assert(fx2_is(FX_COLOR_DUB_ECHO));
    /* DUB ECHO tapped alone stops COLOR 2, and the mixer never hears it. */
    assert(send(ECHO, 0)); assert(send(ECHO, 2)); assert(!fx2_decks[0].on);
    /* A chord again starts it; the same chord stops it. */
    assert(!send(CRUSH, 0)); assert(send(SWEEP, 0)); assert(fx2_is(FX_COLOR_SWEEP));
    assert(send(SWEEP, 2)); assert(send(SWEEP, 0)); assert(!fx2_decks[0].on);
    assert(send(SWEEP, 2)); assert(!send(CRUSH, 2));

    /* With ISOLATOR's FX buttons on HIGH only, a low-pass COLOR 2 leaves the
       low band alone: a FILTER chord at depth 40 (to the left). */
    band_mask = RX3_BAND_HIGH;
    assert(!send(SPACE, 0)); assert(send(FILTER, 0)); assert(send(FILTER, 2)); assert(!send(SPACE, 2));
    assert(fx2_is(FX_COLOR_FILTER));
    for (unsigned b = 0; b < 12; b++) { tone(out, 4096, b * 4096, 80.0f); run_deck(1, out, 4096); }
    tone(in, 4096, 12 * 4096, 80.0f);
    assert(power(out, 4096) > power(in, 4096) * 0.8f);   /* 80 Hz kept */
    for (unsigned b = 0; b < 12; b++) { tone(out, 4096, b * 4096, 9000.0f); run_deck(1, out, 4096); }
    tone(in, 4096, 12 * 4096, 9000.0f);
    assert(power(out, 4096) < power(in, 4096) * 0.1f);   /* 9 kHz filtered */
    band_mask = RX3_BAND_ALL;

    /* The probe: every key, how often, its further arguments; every LED ID. */
    key_args[0] = 0x55; assert(!send(0x4f00, 0)); assert(!send(0x4f00, 0));
    rx3_fx2_module.report();
    int found = 0;
    for (unsigned i = 0; i < fx2_probe_count; i++)
        if (fx2_probe[i].key == 0x4f00) { found = 1; assert(fx2_probe[i].count == 2 && fx2_probe[i].arguments[0] == 0x55); }
    assert(found);
    assert(fx2_probe_led_count >= 2);
    rx3_fx2_module.report();                 /* the last line written is the probe's or the bpm's */
    assert(last_log[0] == 'f');

    /* A list that is not exactly six distinct codes stays off. */
    fx2_parse_buttons("4180,4181"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,4180"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,zz"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,41850"); assert(!fx2_buttons_known);
    /* Without codes, no key is ever consumed. */
    assert(!send(ECHO, 0) && !send(FILTER, 0));

    rx3_fx2_module.stop();
    assert(deck_count == 0);
    return 0;
}
''', [], ['-pthread', '-Wno-unused-function'])

    def test_isolator_through_its_callbacks(self):
        self.run_units(MOCKS + r'''
#include "isolator/rx3_iso_module.c"
static struct rx3_stereo in[4096], out[4096];
static float power(const struct rx3_stereo *x, unsigned n) {
    double s = 0; for (unsigned i = 0; i < n; i++) s += x[i].left * x[i].left; return (float)(s / n);
}
static float through(unsigned deck, float hz) {
    for (unsigned b = 0; b < 6; b++) { tone(out, 4096, b * 4096, hz); run_deck(deck, out, 4096); }
    tone(in, 4096, 6 * 4096, hz);
    return power(out, 4096) / power(in, 4096);
}
int main(void) {
    struct rx3_services with_bands = services;
    with_bands.bands = &rx3_bands;
    setenv("RX3_ISOLATOR", "1", 1);
    assert(rx3_iso_module.start(&with_bands) == 1);
    assert(deck_count == 1 && decks[0].priority == 70);
    assert(row && row->panel_id == 3 && row->scope == RX3_PAD_SCOPE_DECK && row->count == 6);
    assert(row->tab_slots[RX3_TAB_STATUS_SELECTED] == 16);
    /* It provides the effect bands; another module cannot. */
    static const char other = 0;
    assert(!rx3_bands.provide(&other));
    rx3_iso_module.audio_started(44100);

    /* Flat: bit-exact. */
    tone(in, 4096, 0, 80.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));
    /* LOW tapped: killed; the bass goes, the highs stay; the other deck too. */
    row->fire(0, 0, 0);
    assert(row->is_on(0, 0, 0) && row->caption(0, 0, 0)[4] == 'K');
    assert(through(0, 80.0f) < 0.01f);
    assert(through(0, 9000.0f) > 0.9f);
    tone(in, 4096, 0, 80.0f); memcpy(out, in, sizeof(in)); run_deck(1, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));
    /* MID dragged to +6 dB: four times the power of a mid tone. */
    row->fire(0, 0, 0);
    row->slider_set(0, 1, ISO_LEVEL_MAX, 1);
    const uint16_t *c = row->caption(0, 1, 0);
    assert(c[4] == '+' && c[5] == '6');
    float boost = through(0, 900.0f);
    assert(boost > 3.6f && boost < 4.4f);
    /* Back to 0 dB through the detent: bit-exact again once it has faded out. */
    row->slider_set(0, 1, ISO_UNITY + 1u, 1);
    for (unsigned b = 0; b < 8; b++) { tone(out, 4096, b * 4096, 900.0f); run_deck(0, out, 4096); }
    tone(in, 4096, 0, 900.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));
    /* FX bands: LOW and MID off leaves HIGH for the effects; none means all. */
    row->fire(1, ISO_W_FX + 0, 0); row->fire(1, ISO_W_FX + 1, 0);
    assert(rx3_bands.mask(1) == RX3_BAND_HIGH && rx3_bands.mask(0) == RX3_BAND_ALL);
    assert(!row->is_on(1, ISO_W_FX, 0) && row->is_on(1, ISO_W_FX + 2, 0));
    row->fire(1, ISO_W_FX + 2, 0);
    assert(rx3_bands.mask(1) == RX3_BAND_ALL);
    rx3_iso_module.stop();
    assert(deck_count == 0 && !row && rx3_bands.mask(1) == RX3_BAND_ALL);
    assert(rx3_bands.provide(&other));
    return 0;
}
''', ['core/services/rx3_bands.c'], ['-pthread', '-Wno-unused-function', '-Wno-unused-const-variable'])

    def run_prepare(self, module, function, files=None, switch_off=False):
        """One module.sh prepare hook, in a shell with the orchestrator's
        functions stubbed to print what they are asked."""
        script = (test_framework.MODULES / module / 'module.sh').read_text()
        with tempfile.TemporaryDirectory() as temporary:
            usb = pathlib.Path(temporary)
            core = usb / 'core'
            core.write_bytes(b'x')
            for name, text in (files or {}).items():
                path = usb / 'RX3_RUNTIME' / module / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            prefix = f'USB="{usb}"\nCORE_OBJECT="{core}"\n' + f"""module_begin(){{ :; }}
register_prepare_hook(){{ :; }}
register_after_launch_hook(){{ :; }}
say(){{ printf 'SAY %s\\n' "$*"; }}
module_disabled_by_switch(){{ return {0 if switch_off else 1}; }}
stage_panel_asset(){{ printf 'STAGE %s %s %s\\n' "$1" "$2" "$3"; }}
module_export(){{ printf 'EXPORT %s=%s\\n' "$1" "$2"; return 1; }}
"""
            result = subprocess.run(['sh'], input=prefix + script + f'\n{function}\n', text=True,
                                    capture_output=True, cwd=temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((usb / 'injected').exists())
            return result.stdout

    def test_rmx_prepare_stages_its_slots_and_honours_its_switch(self):
        out = self.run_prepare('rmx-looper', 'rmx_looper_prepare')
        for slot in ('11', '12', '13', '14', '15'):
            self.assertIn(f'STAGE rmx-looper {slot} ', out)
        self.assertIn('EXPORT RX3_RMX=1', out)
        out = self.run_prepare('rmx-looper', 'rmx_looper_prepare', switch_off=True)
        self.assertIn('EXPORT RX3_RMX=0', out)
        self.assertNotIn('STAGE', out)

    def test_color2_prepare_reads_buttons_and_probe_without_executing_them(self):
        out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare')
        self.assertIn('EXPORT RX3_FX2=1', out)
        self.assertIn('EXPORT RX3_FX2_BUTTONS=\n', out)
        self.assertIn('EXPORT RX3_FX2_PROBE=0', out)
        self.assertNotIn('STAGE', out)                     # no screen of its own
        good = ('# from the probe\n0x4180, 0x4181,0x4182,0x4183,0x4184,0x4185\n'
                'leds=21,22,23,24,25,26\ndepth=40\n')
        out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare',
                               {'buttons.txt': good, 'probe': ''})
        self.assertIn('EXPORT RX3_FX2_BUTTONS=0x4180,0x4181,0x4182,0x4183,0x4184,0x4185\n', out)
        self.assertIn('EXPORT RX3_FX2_LEDS=21,22,23,24,25,26\n', out)
        self.assertIn('EXPORT RX3_FX2_DEPTH=40\n', out)
        self.assertIn('EXPORT RX3_FX2_PROBE=1', out)
        out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare',
                               {'buttons.txt': 'depth=900\nleds=1,2\ndepth=$(touch injected)\n'})
        self.assertIn('EXPORT RX3_FX2_DEPTH=\n', out)
        self.assertIn('EXPORT RX3_FX2_LEDS=\n', out)
        for bad in ('0x4180,0x4181\n', '$(touch injected),1,2,3,4,5\n', '4180;4181;4182;4183;4184;4185\n'):
            out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare', {'buttons.txt': bad})
            self.assertIn('EXPORT RX3_FX2_BUTTONS=\n', out)

    def test_isolator_prepare_stages_its_slots_and_honours_its_switch(self):
        out = self.run_prepare('isolator', 'isolator_prepare')
        for slot in ('16', '17', '18'):
            self.assertIn(f'STAGE isolator {slot} ', out)
        self.assertIn('EXPORT RX3_ISOLATOR=1', out)
        out = self.run_prepare('isolator', 'isolator_prepare', switch_off=True)
        self.assertIn('EXPORT RX3_ISOLATOR=0', out)
        self.assertNotIn('STAGE', out)

    def test_symbol_finder_reads_an_arm_symbol_table(self):
        """Run on the hook itself: the only ARM ELF this repository may hold."""
        import importlib.util
        spec = importlib.util.spec_from_file_location('finder', ROOT / 'scripts/find_fx_symbols.py')
        finder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(finder)
        hook = ROOT / 'build/librx3_core.so'
        if not hook.is_file():
            self.skipTest('build the hook first: make hook')
        names = {name for _, _, name in finder.symbols(hook.read_bytes())}
        self.assertIn('fx2_stream', names)
        self.assertIn('rmx_master', names)

    def test_probe_line_format(self):
        """The probe line is what buttons.txt is written from: hexadecimal."""
        source = (test_framework.MODULES / 'dual-color-fx/rx3_fx2_feature.h').read_text()
        self.assertIn('"fx2 probe key=0x0000 op=0 ch=0 n=0000 a=00000000 b=00000000 c=00000000"', source)
        self.assertIn('"fx2 probe led id=0x0000 ch=0"', source)


if __name__ == '__main__':
    unittest.main()
