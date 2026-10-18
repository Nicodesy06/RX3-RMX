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
#include "../../mod/modules/core/api/rx3_fx_xpad.h"
#include "../../mod/modules/core/api/rx3_fx_bands.h"

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

static void noise(struct fx_frame *out, unsigned int frames, unsigned int seed, float amp)
{
    for (unsigned int i = 0; i < frames; i++) {
        out[i].left = fx_noise(&seed) * amp;
        out[i].right = fx_noise(&seed) * amp;
    }
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

/* PITCH on a roll keeps its period in time and moves the pitch inside it. */
static void test_roll_pitch(void)
{
    unsigned int capacity = fx_rmx_capacity(RATE);
    struct fx_frame *buffer = calloc(capacity, sizeof(*buffer));
    static struct fx_roll roll;
    fx_roll_init(&roll, buffer, capacity);
    unsigned int total = (unsigned int)RATE * 2u, length = 11025u;   /* half a beat at 120 */
    struct fx_frame *x = malloc(total * sizeof(*x));
    sine(x, total, 440.0f, 0.5f, 0);
    fx_roll_pitch(&roll, -1200);
    fx_roll_set(&roll, length, 1u);
    roll_run(&roll, x, total, 256u);
    unsigned int from = length * 2u;
    float worst = 0.0f;
    for (unsigned int t = from; t + length < total; t++) {
        float d = fabsf(x[t].left - x[t + length].left);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-5f, "roll pitch: the period moved (%g)", worst);
    /* An octave down: zero crossings at half the rate, away from the edges. */
    unsigned int crossings = 0u;
    for (unsigned int t = from + 128u; t < from + length - 128u; t++)
        if ((x[t - 1].left < 0.0f) != (x[t].left < 0.0f)) crossings++;
    float hz = (float)crossings * 0.5f * RATE / (float)(length - 256u);
    CHECK(hz > 210.0f && hz < 230.0f, "roll pitch: -12 gives %.1f Hz, not 220", hz);
    printf("roll: pitch -12 keeps the half-beat period, plays at %.1f Hz\n", hz);
    free(buffer); free(x);
}

/* The crossover's bands sum to an allpass: flat in level, and each band
   holds the tones it should. */
static void test_bands(void)
{
    unsigned int frames = (unsigned int)RATE;
    struct fx_frame *x = malloc(frames * sizeof(*x));
    static const float tones[3] = {80.0f, 900.0f, 9000.0f};
    for (unsigned int tone = 0; tone < 3u; tone++) {
        struct fx_bands bands;
        fx_bands_init(&bands, RATE);
        sine(x, frames, tones[tone], 0.5f, 0);
        double e[3] = {0, 0, 0}, sum = 0, in = 0;
        for (unsigned int i = 0; i < frames; i++) {
            struct fx_band_frame b;
            fx_bands_split(&bands, x[i], &b);
            if (i < frames / 2u) continue;
            e[0] += b.low.left * b.low.left; e[1] += b.mid.left * b.mid.left; e[2] += b.high.left * b.high.left;
            float s = b.low.left + b.mid.left + b.high.left;
            sum += s * s; in += x[i].left * x[i].left;
        }
        float flat = 10.0f * log10f((float)(sum / in));
        float own = 10.0f * log10f((float)(e[tone] / in));
        CHECK(fabsf(flat) < 0.05f, "bands: %g Hz sums to %.2f dB", tones[tone], flat);
        CHECK(own > -1.5f, "bands: %g Hz only %.1f dB in its band", tones[tone], own);
        for (unsigned int other = 0; other < 3u; other++) {
            if (other == tone) continue;
            float leak = 10.0f * log10f((float)(e[other] / in) + 1e-12f);
            CHECK(leak < -12.0f, "bands: %g Hz leaks %.1f dB into band %u", tones[tone], leak, other);
        }
        printf("bands: %5.0f Hz sum %+.3f dB, in its own band %.2f dB\n", tones[tone], flat, own);
    }
    free(x);
}

static void identity(void *c, struct fx_frame *io, unsigned int n) { (void)c; (void)io; (void)n; }
static void silence_effect(void *c, struct fx_frame *io, unsigned int n)
{ (void)c; for (unsigned int i = 0; i < n; i++) io[i].left = io[i].right = 0.0f; }

/* An effect limited to some bands: bit-exact with all of them, a smooth
   change of path, and only the chosen bands touched. */
static void test_band_insert(void)
{
    unsigned int frames = (unsigned int)RATE;
    struct fx_frame *x = malloc(frames * sizeof(*x)), *y = malloc(frames * sizeof(*y));
    static struct fx_band_insert insert;
    fx_band_insert_init(&insert, RATE);
    for (unsigned int i = 0; i < frames; i++) {
        float t = (float)i / RATE;
        x[i].left = x[i].right = 0.4f * sinf(6.2831853f * 80.0f * t) + 0.2f * sinf(6.2831853f * 9000.0f * t);
    }
    memcpy(y, x, frames * sizeof(*x));
    unsigned int half = frames / 2u;
    for (unsigned int at = 0; at < half; at += 300u)
        fx_band_insert_process(&insert, y + at, half - at < 300u ? half - at : 300u, 7u, 1, identity, 0);
    CHECK(!memcmp(x, y, half * sizeof(*x)), "band insert: all bands is not bit-exact");
    /* Now only HIGH, still idle: a crossfade to the allpass, no step. */
    for (unsigned int at = half; at < frames; at += 300u)
        fx_band_insert_process(&insert, y + at, frames - at < 300u ? frames - at : 300u, 4u, 1, identity, 0);
    float own = max_step(x, frames), step = max_step(y, frames);
    CHECK(step < own * 1.2f, "band insert: path change clicks (%g against %g)", step, own);
    /* An effect that silences its input, on HIGH only: the 9 kHz goes, 80 Hz stays. */
    memcpy(y, x, frames * sizeof(*x));
    for (unsigned int at = 0; at < frames; at += 256u)
        fx_band_insert_process(&insert, y + at, frames - at < 256u ? frames - at : 256u, 4u, 0, silence_effect, 0);
    double hi = 0, lo = 0;
    struct fx_bands probe; fx_bands_init(&probe, RATE);
    for (unsigned int i = 0; i < frames; i++) {
        struct fx_band_frame b; fx_bands_split(&probe, y[i], &b);
        if (i < frames / 2u) continue;
        hi += b.high.left * b.high.left; lo += b.low.left * b.low.left;
    }
    float lo_db = 10.0f * log10f((float)(lo / (frames / 2u)) / (0.4f * 0.4f * 0.5f));
    float hi_db = 10.0f * log10f((float)(hi / (frames / 2u)) / (0.2f * 0.2f * 0.5f) + 1e-12f);
    CHECK(lo_db > -0.5f && hi_db < -40.0f, "band insert: low %.1f dB, high %.1f dB", lo_db, hi_db);
    printf("bands: effect on HIGH only: 80 Hz %.2f dB, 9 kHz %.1f dB; all bands bit-exact; path change step %.4f\n",
           lo_db, hi_db, step);
    free(x); free(y);
}

static void xpad_run(struct fx_xpad *pad, const struct fx_frame *roll, struct fx_frame *bus,
                     unsigned int frames, unsigned int block)
{
    for (unsigned int at = 0; at < frames; at += block)
        fx_xpad_process(pad, roll ? roll + at : 0, bus + at, frames - at < block ? frames - at : block);
}

static void test_xpad(void)
{
    unsigned int capacity = fx_xpad_pattern_capacity(RATE), synth = fx_xpad_synth_frames(RATE);
    struct fx_frame *tracks[FX_XPAD_TRACKS], *sounds[FX_XPAD_SOUNDS];
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) tracks[t] = calloc(capacity, sizeof(struct fx_frame));
    static struct fx_xpad pad;
    fx_xpad_init(&pad, RATE, tracks, capacity);
    fx_xpad_volume(&pad, 1000u);
    pad.level = 1.0f;
    static const char *names[] = {"KICK", "SNARE", "CLAP", "HI HAT"};
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) {
        sounds[s] = calloc(synth, sizeof(struct fx_frame));
        unsigned int length = fx_xpad_synthesize(s, sounds[s], synth, RATE);
        float peak = 0.0f;
        for (unsigned int i = 0; i < length; i++) if (fabsf(sounds[s][i].left) > peak) peak = fabsf(sounds[s][i].left);
        CHECK(length > 4000u && fabsf(peak - 0.7f) < 0.01f, "%s: length %u peak %g", names[s], length, peak);
        CHECK(fabsf(sounds[s][length - 1u].left) < 0.02f, "%s: ends on a step", names[s]);
        fx_xpad_set_sound(&pad, s, sounds[s], length, RATE);
    }
    /* Where each sound's energy is: the kick low, the hat high. */
    {
        struct fx_bands bands;
        double kick_low = 0, kick_all = 0, hat_high = 0, hat_all = 0;
        fx_bands_init(&bands, RATE);
        for (unsigned int i = 0; i < synth; i++) {
            struct fx_band_frame b; fx_bands_split(&bands, sounds[FX_XPAD_KICK][i], &b);
            kick_low += b.low.left * b.low.left; kick_all += sounds[0][i].left * sounds[0][i].left;
        }
        fx_bands_init(&bands, RATE);
        for (unsigned int i = 0; i < synth; i++) {
            struct fx_band_frame b; fx_bands_split(&bands, sounds[FX_XPAD_HAT][i], &b);
            hat_high += b.high.left * b.high.left; hat_all += sounds[3][i].left * sounds[3][i].left;
        }
        CHECK(kick_low / kick_all > 0.8, "KICK: only %.0f%% below 250 Hz", 100.0 * kick_low / kick_all);
        CHECK(hat_high / hat_all > 0.8, "HI HAT: only %.0f%% above 2.5 kHz", 100.0 * hat_high / hat_all);
        printf("xpad: KICK %.0f%% of its energy below 250 Hz, HI HAT %.0f%% above 2.5 kHz\n",
               100.0 * kick_low / kick_all, 100.0 * hat_high / hat_all);
    }

    unsigned int total = (unsigned int)RATE * 6u;
    struct fx_frame *bus = calloc(total, sizeof(*bus));
    /* A hit sounds at once, then is silent. */
    fx_xpad_hit(&pad, FX_XPAD_SNARE);
    xpad_run(&pad, 0, bus, 22050u, 256u);
    CHECK(fabsf(bus[10].left) > 0.0f && rms(bus + 18000u, 2000u) == 0.0f, "xpad: a hit does not ring once");

    /* The strip repeats a sound at its division, struck on the frame. */
    memset(bus, 0, total * sizeof(*bus));
    unsigned int interval = 5512u;                       /* 1/4 beat at 120 */
    fx_xpad_repeat(&pad, FX_XPAD_KICK, interval);
    xpad_run(&pad, 0, bus, interval * 6u, 333u);
    fx_xpad_repeat(&pad, FX_XPAD_KICK, 0u);
    unsigned int onsets = 0u, last = 0u, misplaced = 0u;
    for (unsigned int i = 1; i < interval * 6u; i++)
        if (bus[i - 1].left == 0.0f && bus[i].left != 0.0f) {
            if (onsets && (i - last) % interval) misplaced++;
            onsets++; last = i;
        }
    float step = max_step(bus, interval * 6u);
    CHECK(onsets == 1u || misplaced == 0u, "xpad: repeats off their frame");
    /* The kick's own attack is a click on purpose; a choke may not add one
       larger than it. */
    float own = max_step(sounds[FX_XPAD_KICK], synth);
    CHECK(step <= own * 1.05f, "xpad: repeat choke clicks (%g against the kick's own %g)", step, own);
    printf("xpad: KICK repeated every %u frames across odd blocks, largest step %.3f (its own attack %.3f)\n",
           interval, step, own);

    /* PITCH: an octave up plays the hat twice as fast. */
    memset(bus, 0, total * sizeof(*bus));
    fx_xpad_pitch(&pad, 1200);
    fx_xpad_hit(&pad, FX_XPAD_CLAP);
    xpad_run(&pad, 0, bus, 22050u, 256u);
    unsigned int heard = 0u;
    for (unsigned int i = 0; i < 22050u; i++) if (bus[i].left != 0.0f) heard = i;
    fx_xpad_pitch(&pad, 0);
    CHECK(heard < synth / 2u + 64u, "xpad: pitch +12 lasted %u frames", heard);

    /* OVERDUB: standby, the first hit starts a four-beat loop, sounds are
       recorded per track, a muted track drops out, a deleted one goes. */
    unsigned int beats4 = 88200u;                         /* four beats at 120 */
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) memset(tracks[t], 0, capacity * sizeof(struct fx_frame));
    fx_xpad_init(&pad, RATE, tracks, capacity);
    fx_xpad_volume(&pad, 1000u); pad.level = 1.0f;
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) fx_xpad_set_sound(&pad, s, sounds[s], synth, RATE);
    fx_xpad_overdub(&pad, beats4);
    memset(bus, 0, total * sizeof(*bus));
    xpad_run(&pad, 0, bus, 4096u, 256u);
    CHECK(pad.shown_state == FX_PATTERN_STANDBY && rms(bus, 4096u) == 0.0f, "xpad: overdub did not stand by");
    fx_xpad_hit(&pad, FX_XPAD_KICK);
    xpad_run(&pad, 0, bus, 20000u, 256u);
    CHECK(pad.shown_state == FX_PATTERN_RUN && pad.position == 20000u, "xpad: the first hit did not start the loop");
    fx_xpad_hit(&pad, FX_XPAD_HAT);
    xpad_run(&pad, 0, bus, beats4 - 20000u, 256u);
    CHECK(pad.shown_tracks == ((1u << FX_XPAD_KICK) | (1u << FX_XPAD_HAT)), "xpad: tracks %x", pad.shown_tracks);
    /* The next pass plays both back with nothing pressed. */
    memset(bus, 0, total * sizeof(*bus));
    xpad_run(&pad, 0, bus, beats4, 512u);
    float kick = rms(bus, 4000u), hat = rms(bus + 20000u, 4000u);
    CHECK(kick > 0.05f && hat > 0.02f, "xpad: loop did not play back (%g, %g)", kick, hat);
    /* Mute the kick: gone; unmute: back. Delete the hat: gone for good. */
    fx_xpad_toggle_mute(&pad, FX_XPAD_KICK);
    memset(bus, 0, total * sizeof(*bus));
    xpad_run(&pad, 0, bus, beats4, 512u);
    CHECK(rms(bus, 4000u) < 1e-6f && rms(bus + 20000u, 4000u) > 0.02f, "xpad: mute is not per track");
    fx_xpad_toggle_mute(&pad, FX_XPAD_KICK);
    fx_xpad_delete(&pad, FX_XPAD_HAT);
    xpad_run(&pad, 0, bus, 8192u * 30u, 512u);            /* let the clear finish */
    memset(bus, 0, total * sizeof(*bus));
    unsigned int phase = pad.position;
    xpad_run(&pad, 0, bus, beats4, 512u);
    unsigned int kick_at = (beats4 - phase) % beats4;
    CHECK(rms(bus + kick_at, 4000u) > 0.05f, "xpad: the kick did not survive the delete");
    CHECK(rms(bus + (kick_at + 20000u) % beats4, 4000u) < 1e-6f, "xpad: the hat was not deleted");
    CHECK(pad.shown_tracks == (1u << FX_XPAD_KICK), "xpad: tracks after delete %x", pad.shown_tracks);
    /* A roll overdubbed onto its own track. */
    struct fx_frame *roll = malloc(beats4 * sizeof(*roll));
    sine(roll, beats4, 330.0f, 0.3f, 0);
    xpad_run(&pad, roll, bus, 8000u, 512u);
    CHECK(pad.shown_tracks & (1u << FX_XPAD_ROLL_TRACK), "xpad: roll not recorded");
    /* OVERDUB again: everything stops and goes. */
    fx_xpad_overdub(&pad, beats4);
    xpad_run(&pad, 0, bus, 8192u * 30u, 512u);
    memset(bus, 0, total * sizeof(*bus));
    xpad_run(&pad, 0, bus, beats4, 512u);
    CHECK(pad.shown_state == FX_PATTERN_OFF && rms(bus, beats4) == 0.0f && !pad.shown_tracks,
          "xpad: overdub off left sound");
    printf("xpad: overdub stands by, starts on the first hit, keeps a track per sound, mutes, deletes, clears\n");

    /* Host cost with the loop running five tracks and two voices. */
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) for (unsigned int i = 0; i < capacity; i++) tracks[t][i].left = 0.1f;
    fx_xpad_overdub(&pad, beats4); fx_xpad_hit(&pad, FX_XPAD_KICK); fx_xpad_hit(&pad, FX_XPAD_HAT);
    xpad_run(&pad, 0, bus, 1024u, 256u);
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) pad.written[t] = 1u;
    fx_xpad_repeat(&pad, FX_XPAD_SNARE, 2756u);
    clock_t begin = clock();
    xpad_run(&pad, roll, bus, beats4, 256u);
    printf("cost: X-PAD    %.1f ns per frame on this host (5 tracks, repeat, roll)\n",
           (double)(clock() - begin) / CLOCKS_PER_SEC * 1e9 / beats4);
    free(roll); free(bus);
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) free(tracks[t]);
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) free(sounds[s]);
}

/* Host cost of a repeating roll, for comparison with the colour engines. */
static void cost_roll(void)
{
    unsigned int capacity = fx_rmx_capacity(RATE), frames = 689u * 256u;
    struct fx_frame *buffer = calloc(capacity, sizeof(*buffer)), *x = malloc(frames * sizeof(*x));
    noise(x, frames, 9u, 0.5f);
    static struct fx_roll roll;
    fx_roll_init(&roll, buffer, capacity);
    fx_roll_set(&roll, 5000u, 1u);
    clock_t begin = clock();
    for (unsigned int at = 0; at < frames; at += 256u) fx_roll_process(&roll, x + at, 256u);
    printf("cost: ROLL     %.1f ns per frame on this host\n",
           (double)(clock() - begin) / CLOCKS_PER_SEC * 1e9 / frames);
    free(buffer); free(x);
}

int main(void)
{
    test_roll();
    test_roll_pitch();
    test_bands();
    test_band_insert();
    test_xpad();
    test_color();
    cost_roll();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all fx engine checks passed\n");
    return 0;
}
