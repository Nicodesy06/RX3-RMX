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
static const struct rx3_input_service input = {.register_pad = register_pad, .register_key = register_key,
    .register_lights = register_lights, .claim_mode_keys = claim_modes, .unregister_owner = unregister_owner,
    .shift_held = shift_held, .blink_on = blink_on, .blink_restart = blink_restart};
static const struct rx3_audio_service audio = {.release_deck_stream = release_deck,
    .release_master = release_master, .claim_deck_stream_at = claim_deck_at, .claim_master_at = claim_master_at};
static const struct rx3_memory_service memory = {reserve, settle, settle, settle, available, held};
static const struct rx3_services services = {.log_line = log_text, .log_number = log_value,
    .panels = &panels, .input = &input, .audio = &audio, .memory = &memory};
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

    def test_roll_loop_module_through_its_callbacks(self):
        self.run_units(MOCKS + r'''
#include "rmx-looper/rx3_rmx_module.c"
static struct rx3_stereo in[512], out[512], bus[512];
int main(void) {
    rmx_bpm_reader = bpm; rmx_bpm_prologue = prologue;
    setenv("RX3_RMX", "1", 1);
    assert(rx3_rmx_module.start(&services) == 1);
    /* Two places on the deck stream around COLOR 2's, one on the master
       after Samples', the panel in the right-hand tab with its own art. */
    assert(deck_count == 2 && decks[0].priority == 55 && decks[1].priority == 65);
    assert(master && master_priority == 60);
    assert(row && row->panel_id == 2 && row->tab_slots && row->tab_slots[RX3_TAB_PAIR_LEFT] == 11);
    assert(pad && light && modes == 1);
    rx3_rmx_module.audio_started(44100);
    for (unsigned i = 0; i < 200 && rmx_bpm_x100[0] != 12800; i++) usleep(10000);
    assert(rmx_bpm_x100[0] == 12800 && rmx_bpm_x100[1] == 9000);

    /* Off the panel the pads belong to the player. */
    struct rx3_pad_event e = press(0x4119, 0);
    assert(!pad(&e));
    row->activate(1);

    /* Pad 3 is 1/4 beat; at 128 BPM that is 5168 frames. */
    const unsigned length = 5168;
    assert(rmx_frames_for(0, 2) == length);
    assert(pad(&e) && rmx_pad_roll[0] == 1);
    static float history[44100];
    unsigned t = 0;
    for (; t < 44100 - 512; t += 512) {
        tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in));
        run_deck(0, out, 512);
        for (unsigned i = 0; i < 512; i++) history[t + i] = out[i].left;
    }
    float worst = 0;
    for (unsigned i = length + 600; i + length < t; i++) {
        float d = fx_absf(history[i] - history[i + length]); if (d > worst) worst = d;
    }
    assert(worst < 1e-5f);
    /* The pad lights: the rolling division full, the others dim. */
    struct rx3_light l = {RX3_LIGHT_NATIVE, 0};
    light(0, 2, &l); assert(l.state == RX3_LIGHT_ON);
    l.state = RX3_LIGHT_NATIVE; light(0, 5, &l); assert(l.state == RX3_LIGHT_DIM);

    /* Letting go returns the live deck, exactly, after the release ramp. */
    e = lift(0x4119, 0); assert(pad(&e) && rmx_pad_roll[0] == 0);
    for (unsigned b = 0; b < 4; b++) { tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512); t += 512; }
    tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512);
    assert(!memcmp(in, out, sizeof(in)));

    /* SHIFT + pad 1 is HOLD: a released pad keeps rolling until HOLD goes. */
    shift = 1; e = press(0x4117, 0); assert(pad(&e)); shift = 0;
    assert(rmx_hold);
    e = press(0x411b, 0); assert(pad(&e)); e = lift(0x411b, 0); assert(pad(&e));
    assert(rmx_pad_roll[0] == 1);
    shift = 1; e = press(0x4117, 0); assert(pad(&e)); shift = 0;
    assert(!rmx_hold && rmx_pad_roll[0] == 0);

    /* SOURCE MST sends deck pads to the master roll. */
    rmx_cycle_source(1); rmx_cycle_source(1);
    assert(rmx_source == RMX_SOURCE_MASTER);
    e = press(0x411a, 1); assert(pad(&e) && rmx_pad_roll[1] == 3);
    e = lift(0x411a, 1); assert(pad(&e));
    rmx_cycle_source(1);
    assert(rmx_source == RMX_SOURCE_CH1);

    /* The loop: SHIFT + pad 2 records one beat of deck 1 (division 1 beat). */
    rmx_set_division(4);
    shift = 1; e = press(0x4118, 0); assert(pad(&e)); shift = 0;
    unsigned beat = rmx_frames_for(0, 4), played = 0;
    float loudest = 0;
    for (unsigned b = 0; b < 200; b++) {
        tone(in, 512, t, 330.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512); t += 512;
        memset(bus, 0, sizeof(bus)); master(bus, 512);
        if (rmx_loop.shown_state == FX_LOOP_PLAY) {
            played += 512;
            for (unsigned i = 0; i < 512; i++) if (fx_absf(bus[i].left) > loudest) loudest = fx_absf(bus[i].left);
        }
    }
    assert(beat == 20672 && played > beat);
    assert(loudest > 0.05f);       /* the bus carries the loop on its own */
    /* OVERDUB on for a while and off: one layer. UNDO takes it back. */
    shift = 1; e = press(0x4119, 0); assert(pad(&e)); shift = 0;
    for (unsigned b = 0; b < 30; b++) {
        tone(in, 512, t, 550.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 512); t += 512;
        memset(bus, 0, sizeof(bus)); master(bus, 512);
    }
    assert(rmx_loop.shown_overdub);
    shift = 1; e = press(0x4119, 0); assert(pad(&e)); shift = 0;
    for (unsigned b = 0; b < 4; b++) { memset(bus, 0, sizeof(bus)); master(bus, 512); }
    assert(rmx_loop.shown_layers == 1 && !rmx_loop.shown_overdub);
    shift = 1; e = press(0x411a, 0); assert(pad(&e)); shift = 0;
    for (unsigned b = 0; b < 4; b++) { memset(bus, 0, sizeof(bus)); master(bus, 512); }
    assert(rmx_loop.shown_layers == 0);
    /* The panel says CLEAR once only the base is left; CLEAR empties. */
    assert(row->caption(0, RMX_W_BACK, 0)[0] == 'C');
    shift = 1; e = press(0x411b, 0); assert(pad(&e)); shift = 0;
    for (unsigned b = 0; b < 4; b++) { memset(bus, 0, sizeof(bus)); master(bus, 512); }
    assert(rmx_loop.shown_state == FX_LOOP_EMPTY);
    memset(bus, 0, sizeof(bus)); master(bus, 512);
    for (unsigned i = 0; i < 512; i++) assert(bus[i].left == 0.0f);

    /* A release taken after the panel closed still lets go. */
    e = press(0x411c, 0); assert(pad(&e));
    row->activate(0);
    e = lift(0x411c, 0); assert(pad(&e) && rmx_pad_roll[0] == 0);
    e = press(0x411c, 0); assert(!pad(&e));

    rx3_rmx_module.stop();
    assert(deck_count == 0 && !master && !row && unregisters == 1);

    /* A player whose accessor differs is refused; too little memory steps
       aside without refusing the runtime. */
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

    def test_color2_module_through_its_callbacks(self):
        self.run_units(MOCKS + r'''
#include "dual-color-fx/rx3_fx2_module.c"
static struct rx3_stereo in[4096], out[4096];
/* Mean square, compared against squared thresholds. */
static float power(const struct rx3_stereo *x, unsigned n) {
    double s = 0; for (unsigned i = 0; i < n; i++) s += x[i].left * x[i].left; return (float)(s / n);
}
int main(void) {
    fx2_bpm_reader = bpm; fx2_bpm_prologue = prologue;
    setenv("RX3_FX2", "1", 1);
    setenv("RX3_FX2_BUTTONS", "0x4180,4181,4182,4183,4184,4185", 1);
    setenv("RX3_FX2_PROBE", "1", 1);
    assert(rx3_fx2_module.start(&services) == 1);
    assert(deck_count == 1 && decks[0].priority == 60 && key);
    assert(row && row->panel_id == 3 && row->scope == RX3_PAD_SCOPE_DECK);
    assert(row->tab_slots[RX3_TAB_STATUS_SELECTED] == 16 && row->tab_slots[RX3_TAB_STATUS_BEATFX] == 18);
    assert(fx2_buttons_known && fx2_buttons[0] == 0x4180 && fx2_buttons[5] == 0x4185);
    rx3_fx2_module.audio_started(44100);

    /* Off: bit-exact. */
    tone(in, 4096, 0, 5000.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));

    /* FILTER far left on deck 1 only: a 5 kHz tone mostly gone, deck 2 untouched. */
    assert(fx2_decks[0].kind == FX_COLOR_FILTER);
    row->slider_set(0, FX2_W_COLOR, 20, 1);
    assert(fx2_decks[0].on && row->is_on(0, FX2_W_KIND, 1));
    for (unsigned b = 0; b < 8; b++) { tone(out, 4096, b * 4096, 5000.0f); run_deck(0, out, 4096); }
    assert(power(out, 4096) < 0.02f * 0.02f);
    tone(in, 4096, 0, 5000.0f); memcpy(out, in, sizeof(in)); run_deck(1, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));
    /* The caption reads the bipolar position. */
    const uint16_t *c = row->caption(0, FX2_W_COLOR, 0);
    assert(c[6] == '-' && c[7] == '8' && c[8] == '0' && c[9] == 0);

    /* A tap on COLOR returns it to centre: bypass again, exactly, once the
       smoothing has arrived. */
    row->fire(0, FX2_W_COLOR, 0);
    for (unsigned b = 0; b < 4; b++) { tone(out, 4096, b * 4096, 5000.0f); run_deck(0, out, 4096); }
    tone(in, 4096, 0, 5000.0f); memcpy(out, in, sizeof(in)); run_deck(0, out, 4096);
    assert(!memcmp(in, out, sizeof(in)));

    /* DUB ECHO, then switch to FILTER: the echo keeps ringing under it. */
    row->fire(0, FX2_W_KIND, 2);
    assert(fx2_decks[0].kind == FX_COLOR_DUB_ECHO);
    row->slider_set(0, FX2_W_COLOR, 170, 1);
    for (unsigned b = 0; b < 4; b++) { tone(out, 4096, b * 4096, 440.0f); run_deck(0, out, 4096); }
    row->fire(0, FX2_W_KIND, 0);
    assert(fx2_decks[0].kind == FX_COLOR_FILTER && fx2_decks[0].color[FX_COLOR_FILTER] == 100);
    memset(out, 0, sizeof(out)); run_deck(0, out, 4096);
    assert(power(out, 4096) > 1e-6f);                       /* silence in, echoes out */
    assert(fx_color_has_tail(&fx2_engines[0][FX_COLOR_DUB_ECHO]));
    /* Each effect kept its own position. */
    row->fire(0, FX2_W_KIND, 2);
    assert(row->slider_get(0, FX2_W_COLOR) == 170);

    /* SHIFT + a mapped Color FX button adds that effect on both channels and
       is consumed with its release; without SHIFT the mixer has it. */
    struct rx3_key_event k = {0x4185, 0, 1};
    assert(!key(&k));
    shift = 1; assert(key(&k)); shift = 0;
    assert(fx2_decks[0].kind == FX_COLOR_FILTER && fx2_decks[1].kind == FX_COLOR_FILTER && fx2_decks[1].on);
    k.operation = 2; assert(key(&k));
    k.operation = 2; assert(!key(&k));
    /* The probe heard it, and writes it out on the diagnostic worker. */
    rx3_fx2_module.report();
    for (unsigned i = 0; i < fx2_probe_count; i++) assert(fx2_probe[i].key == 0x4185);
    assert(fx2_probe_count == 1);

    /* A button map that is not exactly six distinct codes stays off. */
    fx2_parse_buttons("4180,4181"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,4180"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,zz"); assert(!fx2_buttons_known);
    fx2_parse_buttons("4180,4181,4182,4183,4184,41850"); assert(!fx2_buttons_known);

    rx3_fx2_module.stop();
    assert(deck_count == 0 && !row);
    return 0;
}
''', [], ['-pthread', '-Wno-unused-function'])

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

    def test_roll_loop_prepare_stages_its_slots_and_honours_its_switch(self):
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
        for slot in ('16', '17', '18'):
            self.assertIn(f'STAGE dual-color-fx {slot} ', out)
        good = '# from the probe\n0x4180, 0x4181,0x4182,0x4183,0x4184,0x4185\n'
        out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare',
                               {'buttons.txt': good, 'probe': ''})
        self.assertIn('EXPORT RX3_FX2_BUTTONS=0x4180,0x4181,0x4182,0x4183,0x4184,0x4185\n', out)
        self.assertIn('EXPORT RX3_FX2_PROBE=1', out)
        for bad in ('0x4180,0x4181\n', '$(touch injected),1,2,3,4,5\n', '4180;4181;4182;4183;4184;4185\n'):
            out = self.run_prepare('dual-color-fx', 'dual_color_fx_prepare', {'buttons.txt': bad})
            self.assertIn('EXPORT RX3_FX2_BUTTONS=\n', out)

    def test_probe_line_format(self):
        """The probe line is what buttons.txt is written from: hexadecimal."""
        source = (test_framework.MODULES / 'dual-color-fx/rx3_fx2_feature.h').read_text()
        self.assertIn('"fx2 probe key=0x0000 op=0 ch=0"', source)


if __name__ == '__main__':
    unittest.main()
