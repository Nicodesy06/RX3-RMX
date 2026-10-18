/* SPDX-License-Identifier: MPL-2.0 */
/* Offline checks of the effect engines: the same headers the ARM hook
 * compiles, run here against synthetic signals. Each check prints one line
 * and the process fails on the first wrong result. These establish the
 * arithmetic, not how anything sounds on a deck. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../../mod/modules/core/api/rx3_fx_rmx.h"

#define RATE 44100.0f
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void sine(struct fx_frame *out, unsigned int frames, float hz, float amp, unsigned int t0)
{
    for (unsigned int i = 0; i < frames; i++) {
        float v = amp * sinf(2.0f * 3.14159265f * hz * (float)(t0 + i) / RATE);
        out[i].left = v; out[i].right = v * 0.9f;
    }
}

/* Largest step between neighbours: a click is a step far above the signal's
 * own. A 220 Hz sine at 0.5 never steps more than about 0.016. */
static float max_step(const struct fx_frame *x, unsigned int frames)
{
    float worst = 0.0f;
    for (unsigned int i = 1; i < frames; i++) {
        float d = fabsf(x[i].left - x[i - 1].left);
        if (d > worst) worst = d;
    }
    return worst;
}

static float rms(const struct fx_frame *x, unsigned int frames)
{
    double s = 0;
    for (unsigned int i = 0; i < frames; i++) s += x[i].left * x[i].left;
    return (float)sqrt(s / frames);
}

/* Run a signal through a roll in blocks, the way the player calls it. */
static void roll_run(struct fx_roll *roll, struct fx_frame *io, unsigned int frames, unsigned int block)
{
    for (unsigned int at = 0; at < frames; at += block)
        fx_roll_process(roll, io + at, frames - at < block ? frames - at : block);
}

static void test_roll(void)
{
    unsigned int capacity = fx_rmx_capacity(RATE);
    struct fx_frame *buffer = calloc(capacity, sizeof(*buffer));
    static struct fx_roll roll;
    fx_roll_init(&roll, buffer, capacity);
    unsigned int total = (unsigned int)RATE * 3u;
    struct fx_frame *in = malloc(total * sizeof(*in)), *out = malloc(total * sizeof(*out));
    sine(in, total, 220.0f, 0.5f, 0);
    memcpy(out, in, total * sizeof(*in));
    unsigned int beat = (unsigned int)(RATE * 60.0f / 128.0f);   /* 128 BPM */
    unsigned int length = beat / 4u;                              /* 1/4 beat */
    unsigned int engage = 10000u, release = engage + length * 12u;

    roll_run(&roll, out, engage, 256u);
    fx_roll_set(&roll, length, 1u);
    roll_run(&roll, out + engage, release - engage, 256u);
    fx_roll_set(&roll, 0u, 1u);
    roll_run(&roll, out + release, total - release, 256u);

    int same_first_pass = !memcmp(out, in, (engage + length) * sizeof(*in));
    CHECK(same_first_pass, "roll: the first pass is not the live signal");
    printf("roll: first pass identical to live: %s\n", same_first_pass ? "yes" : "no");

    float worst = 0.0f;
    for (unsigned int t = engage + length + FX_RMX_SEAM + 8u; t + length < release - 512u; t++) {
        float d = fabsf(out[t].left - out[t + length].left);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-6f, "roll: not periodic at the slice length (%g)", worst);
    printf("roll: periodic at %u frames, worst difference %.2g\n", length, worst);

    float step = max_step(out, total), own = max_step(in, total);
    CHECK(step < own * 3.0f, "roll: click, step %g against the signal's own %g", step, own);
    printf("roll: largest step %.4f (signal's own %.4f)\n", step, own);

    unsigned int settled = release + FX_RMX_RAMP + 512u;
    int back = !memcmp(out + settled, in + settled, (total - settled) * sizeof(*in));
    CHECK(back, "roll: did not return to the live signal");
    printf("roll: live signal back exactly %u frames after release: %s\n",
           FX_RMX_RAMP + 512u, back ? "yes" : "no");

    /* Change division inside a running roll: shorter, then longer again. */
    fx_roll_init(&roll, buffer, capacity);
    memcpy(out, in, total * sizeof(*in));
    unsigned int half = beat / 2u, eighth = beat / 8u;
    fx_roll_set(&roll, half, 2u);
    roll_run(&roll, out, half * 3u, 128u);
    fx_roll_set(&roll, eighth, 2u);
    roll_run(&roll, out + half * 3u, eighth * 16u, 128u);
    unsigned int mark = half * 3u + eighth * 16u;
    worst = 0.0f;
    for (unsigned int t = mark - eighth * 8u; t + eighth < mark; t++) {
        float d = fabsf(out[t].left - out[t + eighth].left);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-6f, "roll: shorter division not periodic (%g)", worst);
    fx_roll_set(&roll, beat, 2u);
    roll_run(&roll, out + mark, total - mark, 128u);
    step = max_step(out, total);
    CHECK(step < own * 3.0f, "roll: division change clicks (%g)", step);
    printf("roll: 1/2 -> 1/8 -> 1 beat without a click, largest step %.4f\n", step);

    /* Block sizes the player is known to use, and an odd one. */
    unsigned int blocks[] = {64u, 512u, 333u};
    for (unsigned int b = 0; b < 3u; b++) {
        fx_roll_init(&roll, buffer, capacity);
        memcpy(out, in, total * sizeof(*in));
        roll_run(&roll, out, 5000u, blocks[b]);
        fx_roll_set(&roll, length, 3u);
        roll_run(&roll, out + 5000u, length * 6u, blocks[b]);
        fx_roll_set(&roll, 0u, 3u);
        roll_run(&roll, out + 5000u + length * 6u, total - 5000u - length * 6u, blocks[b]);
        step = max_step(out, total);
        CHECK(step < own * 3.0f, "roll: block %u clicks (%g)", blocks[b], step);
    }
    printf("roll: blocks of 64, 512 and 333 frames click-free\n");
    free(buffer); free(in); free(out);
}

static void loop_run(struct fx_loop *loop, const struct fx_frame *source, struct fx_frame *out,
                     unsigned int frames, unsigned int block)
{
    for (unsigned int at = 0; at < frames; at += block)
        fx_loop_process(loop, source + at, out + at, frames - at < block ? frames - at : block);
}

static void noise(struct fx_frame *out, unsigned int frames, unsigned int seed, float amp)
{
    for (unsigned int i = 0; i < frames; i++) {
        out[i].left = fx_noise(&seed) * amp;
        out[i].right = fx_noise(&seed) * amp;
    }
}

static void test_loop(void)
{
    unsigned int capacity = fx_rmx_capacity(RATE);
    struct fx_frame *base = calloc(capacity, sizeof(*base));
    struct fx_frame *slots[FX_LOOP_SLOTS];
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++) slots[i] = calloc(capacity, sizeof(*base));
    static struct fx_loop loop;
    fx_loop_init(&loop, base, slots, capacity);
    fx_loop_set_level(&loop, 1000u);
    loop.level = 1.0f;

    unsigned int length = (unsigned int)(RATE * 60.0f / 120.0f);   /* one beat at 120 */
    unsigned int total = length * 20u;
    struct fx_frame *a = malloc(total * sizeof(*a)), *b = calloc(total, sizeof(*b));
    struct fx_frame *out = calloc(total, sizeof(*out)), *silence = calloc(total, sizeof(*out));
    noise(a, total, 7u, 0.3f);

    fx_loop_command(&loop, FX_LOOP_CMD_RECORD, length);
    loop_run(&loop, a, out, length + FX_RMX_SEAM, 256u);
    CHECK(loop.state == FX_LOOP_PLAY, "loop: not playing after one pass");
    /* Two clean passes from silence: the loop alone. */
    memset(out, 0, total * sizeof(*out));
    loop_run(&loop, silence, out, length * 2u, 256u);
    float worst = 0.0f;
    unsigned int offset = length - FX_RMX_SEAM;   /* where the playback cursor was */
    for (unsigned int t = 0; t < length * 2u; t++) {
        unsigned int at = (t + FX_RMX_SEAM) % length;
        if (at < FX_RMX_SEAM) continue;           /* the blended head */
        float d = fabsf(out[t].left - a[at].left);
        if (d > worst) worst = d;
    }
    (void)offset;
    CHECK(worst < 1e-4f, "loop: playback is not the recorded audio (%g)", worst);
    printf("loop: playback reproduces the captured beat, worst %.2g\n", worst);

    /* Overdub one exact pass of a second signal. */
    noise(b, total, 99u, 0.2f);
    fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_ON, 0u);
    memset(out, 0, total * sizeof(*out));
    loop_run(&loop, b, out, length, 64u);
    fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_OFF, 0u);
    loop_run(&loop, silence, out, 512u, 64u);
    CHECK(loop.shown_layers == 1u, "loop: expected one layer, have %u", loop.shown_layers);
    memset(out, 0, total * sizeof(*out));
    unsigned int start = loop.position;
    loop_run(&loop, silence, out, length, 256u);
    float with_layer = rms(out, length);
    printf("loop: after one overdub, %u layer(s), level %.3f\n", loop.shown_layers, with_layer);

    fx_loop_command(&loop, FX_LOOP_CMD_UNDO, 0u);
    loop_run(&loop, silence, out, 2048u, 256u);
    CHECK(loop.shown_layers == 0u, "loop: undo left %u layer(s)", loop.shown_layers);
    memset(out, 0, total * sizeof(*out));
    start = loop.position;
    loop_run(&loop, silence, out, length, 256u);
    worst = 0.0f;
    for (unsigned int t = 0; t < length; t++) {
        unsigned int at = (start + t) % length;
        if (at < FX_RMX_SEAM) continue;
        float d = fabsf(out[t].left - a[at].left);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-4f, "loop: undo did not restore the base (%g)", worst);
    CHECK(with_layer > rms(out, length) * 1.1f, "loop: the overdub was not audible");
    printf("loop: undo restores the base exactly, worst %.2g\n", worst);

    /* Six sessions: more than the undo depth, so the oldest are merged. The
       total must be the base plus every layer, merge or no merge. */
    double expected_energy_gain = 0;
    for (unsigned int session = 0; session < 6u; session++) {
        fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_ON, 0u);
        loop_run(&loop, b, out, length, 512u);
        fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_OFF, 0u);
        loop_run(&loop, silence, out, length * 2u, 512u);
        expected_energy_gain += 1;
    }
    for (unsigned int i = 0; i < 64u && loop.merging; i++) loop_run(&loop, silence, out, 512u, 512u);
    CHECK(!loop.merging, "loop: merge never finished");
    CHECK(loop.shown_layers <= FX_LOOP_LAYERS, "loop: %u layers exceed the undo depth", loop.shown_layers);
    memset(out, 0, total * sizeof(*out));
    start = loop.position;
    loop_run(&loop, silence, out, length, 256u);
    /* Every session overdubbed the same `b` from the same phase relation? No:
       each started where the cursor was. Check the sum frame by frame using
       the engine's own buffers instead: base plus layers equals the output. */
    worst = 0.0f;
    for (unsigned int t = 0; t < length; t++) {
        unsigned int at = (start + t) % length;
        float sum = base[at].left;
        for (unsigned int n = 0; n < loop.depth; n++) {
            struct fx_loop_slot *slot = &loop.slot[loop.stack[n]];
            if (fx_loop_in_range(&loop, slot, at)) sum += slot->frames[at].left * slot->gain;
        }
        float d = fabsf(out[t].left - sum);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-4f, "loop: output differs from its layers (%g)", worst);
    printf("loop: six overdubs, %u undoable layers, merged remainder consistent (worst %.2g)\n",
           loop.shown_layers, worst);
    (void)expected_energy_gain;

    /* Overdub on a sine: the session edges must not click. */
    struct fx_frame *s = malloc(total * sizeof(*s));
    sine(s, total, 220.0f, 0.4f, 0);
    fx_loop_init(&loop, base, slots, capacity);
    fx_loop_set_level(&loop, 1000u);
    loop.level = 1.0f;
    fx_loop_command(&loop, FX_LOOP_CMD_RECORD, length);
    memset(out, 0, total * sizeof(*out));
    loop_run(&loop, s, out, length + FX_RMX_SEAM, 128u);
    fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_ON, 0u);
    loop_run(&loop, s + length, out, length / 3u, 128u);
    fx_loop_command(&loop, FX_LOOP_CMD_OVERDUB_OFF, 0u);
    /* The source keeps playing while the session fades out, as a deck does. */
    loop_run(&loop, s + length + length / 3u, out, 1024u, 128u);
    memset(out, 0, total * sizeof(*out));
    loop_run(&loop, silence, out, length * 3u, 128u);
    float step = max_step(out, length * 3u);
    CHECK(step < 0.05f, "loop: overdub edge or seam clicks (%g)", step);
    printf("loop: seam and overdub edges on a sine, largest step %.4f\n", step);

    fx_loop_command(&loop, FX_LOOP_CMD_CLEAR, 0u);
    loop_run(&loop, silence, out, 2048u, 128u);
    memset(out, 0, total * sizeof(*out));
    loop_run(&loop, silence, out, length, 128u);
    CHECK(loop.state == FX_LOOP_EMPTY && rms(out, length) == 0.0f, "loop: clear left sound");
    printf("loop: clear silences and empties\n");
    free(s); free(a); free(b); free(out); free(silence); free(base);
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++) free(slots[i]);
}

static float tone_gain(unsigned int kind, float color, float param, float hz)
{
    static struct fx_frame delay[FX_COLOR_DELAY_FRAMES];
    struct fx_color fx;
    fx_color_init(&fx, kind, RATE, delay, FX_COLOR_DELAY_FRAMES);
    unsigned int frames = (unsigned int)RATE;
    struct fx_frame *x = malloc(frames * sizeof(*x));
    sine(x, frames, hz, 0.5f, 0);
    float before = rms(x + frames / 2u, frames / 2u);
    fx_color_process(&fx, x, frames, color, param);
    float after = rms(x + frames / 2u, frames / 2u);
    free(x);
    return 20.0f * log10f(after / before);
}

static void test_color(void)
{
    static const char *names[] = {"FILTER", "DUB ECHO", "SPACE", "CRUSH", "NOISE", "SWEEP"};
    static struct fx_frame delay[FX_COLOR_DELAY_FRAMES];
    unsigned int frames = (unsigned int)RATE * 2u;
    struct fx_frame *x = malloc(frames * sizeof(*x)), *y = malloc(frames * sizeof(*y));

    /* Centre is bypass, sample for sample, for every engine. */
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        struct fx_color fx;
        fx_color_init(&fx, kind, RATE, delay, FX_COLOR_DELAY_FRAMES);
        noise(x, frames, 3u, 0.5f);
        memcpy(y, x, frames * sizeof(*x));
        fx_color_process(&fx, y, frames, 0.0f, 0.5f);
        int exact = !memcmp(x, y, frames * sizeof(*x));
        CHECK(exact, "%s: centre is not a bypass", names[kind]);
    }
    printf("color: centre is an exact bypass on all six engines\n");

    float lp_high = tone_gain(FX_COLOR_FILTER, -1.0f, 0.0f, 5000.0f);
    float lp_low = tone_gain(FX_COLOR_FILTER, -0.5f, 0.0f, 100.0f);
    float hp_low = tone_gain(FX_COLOR_FILTER, 1.0f, 0.0f, 100.0f);
    float hp_high = tone_gain(FX_COLOR_FILTER, 0.5f, 0.0f, 8000.0f);
    CHECK(lp_high < -30.0f, "FILTER left: 5 kHz only %.1f dB", lp_high);
    CHECK(lp_low > -3.0f, "FILTER half left: 100 Hz cut %.1f dB", lp_low);
    CHECK(hp_low < -30.0f, "FILTER right: 100 Hz only %.1f dB", hp_low);
    CHECK(hp_high > -3.0f, "FILTER half right: 8 kHz cut %.1f dB", hp_high);
    printf("color: FILTER full left 5 kHz %.1f dB, half left 100 Hz %.1f dB, "
           "full right 100 Hz %.1f dB, half right 8 kHz %.1f dB\n", lp_high, lp_low, hp_low, hp_high);

    /* DUB ECHO: an impulse comes back half a beat later. */
    {
        struct fx_color fx;
        fx_color_init(&fx, FX_COLOR_DUB_ECHO, RATE, delay, FX_COLOR_DELAY_FRAMES);
        fx_color_tempo(&fx, 120.0f);
        memset(x, 0, frames * sizeof(*x));
        struct fx_frame warm[8192];
        memset(warm, 0, sizeof(warm));
        /* Let the controls settle on silence before the impulse. */
        for (unsigned int i = 0; i < 20u; i++) fx_color_process(&fx, warm, 8192u, 0.6f, 0.3f);
        fx.delay_time = fx.delay_target;
        x[0].left = x[0].right = 1.0f;
        fx_color_process(&fx, x, frames, 0.6f, 0.3f);
        unsigned int peak = 1;
        for (unsigned int i = 1; i < frames / 2u; i++)
            if (fabsf(x[i].left) > fabsf(x[peak].left)) peak = i;
        unsigned int expected = (unsigned int)(RATE * 0.25f);
        CHECK(peak + 8u > expected && peak < expected + 8u, "DUB ECHO: first echo at %u, expected %u", peak, expected);
        printf("color: DUB ECHO first repeat at %u frames (half a beat at 120 BPM = %u)\n", peak, expected);
    }

    /* SPACE: the tail keeps sounding after the control returns, then dies. */
    {
        struct fx_color fx;
        fx_color_init(&fx, FX_COLOR_SPACE, RATE, delay, FX_COLOR_DELAY_FRAMES);
        noise(x, frames, 5u, 0.4f);
        fx_color_process(&fx, x, frames / 4u, 0.8f, 0.6f);
        memset(x, 0, frames * sizeof(*x));
        fx_color_process(&fx, x, frames, 0.0f, 0.6f);
        float early = rms(x + 2000u, 4000u), late = rms(x + frames - 4000u, 4000u);
        CHECK(early > 1e-3f, "SPACE: no tail after release (%g)", early);
        CHECK(late < early * 0.1f, "SPACE: tail does not decay (%g -> %g)", early, late);
        printf("color: SPACE tail after release %.4f, two seconds later %.6f\n", early, late);
    }

    /* Every engine, pushed hard both ways on loud noise for ten seconds:
       finite and bounded. */
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        for (int side = -1; side <= 1; side += 2) {
            struct fx_color fx;
            fx_color_init(&fx, kind, RATE, delay, FX_COLOR_DELAY_FRAMES);
            float peak = 0.0f;
            int finite = 1;
            for (unsigned int second = 0; second < 5u; second++) {
                noise(x, frames, 11u + second, 0.9f);
                fx_color_process(&fx, x, frames, (float)side, 1.0f);
                for (unsigned int i = 0; i < frames; i++) {
                    if (!isfinite(x[i].left) || !isfinite(x[i].right)) finite = 0;
                    if (fabsf(x[i].left) > peak) peak = fabsf(x[i].left);
                }
            }
            CHECK(finite && peak < 4.0f, "%s %s: unstable, peak %g", names[kind], side < 0 ? "left" : "right", peak);
        }
    }
    printf("color: all engines stable for ten seconds at full settings\n");

    /* Host cost, to compare engines with each other. Not a figure for the A9. */
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        struct fx_color fx;
        fx_color_init(&fx, kind, RATE, delay, FX_COLOR_DELAY_FRAMES);
        noise(x, frames, 1u, 0.5f);
        clock_t begin = clock();
        for (unsigned int pass = 0; pass < 10u; pass++)
            for (unsigned int at = 0; at < frames; at += 512u)
                fx_color_process(&fx, x + at, frames - at < 512u ? frames - at : 512u, 0.7f, 0.5f);
        double seconds = (double)(clock() - begin) / CLOCKS_PER_SEC;
        printf("cost: %-8s %.1f ns per frame on this host\n", names[kind], seconds * 1e9 / (frames * 10.0));
    }
    free(x); free(y);
}

int main(void)
{
    test_roll();
    test_loop();
    test_color();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all fx engine checks passed\n");
    return 0;
}
