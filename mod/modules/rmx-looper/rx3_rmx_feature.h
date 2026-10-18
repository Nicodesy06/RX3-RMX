/* SPDX-License-Identifier: MPL-2.0
 * RMX X-PAD behaviour, private to rx3_rmx_module.c: the roll and X-PAD
 * engines, where they sit on the audio stages, the touch strip, the pads
 * while the panel owns them, their lights, the sounds and the tempo worker.
 */
#ifndef RX3_RMX_FEATURE_H
#define RX3_RMX_FEATURE_H

static int rmx_enabled;
static const struct rx3_pad_row rmx_row;
/* Owner tokens: one place per owner and stage, and this module takes two
   places on the deck stream. */
static const char rmx_deck_roll_owner, rmx_deck_tap_owner, rmx_master_owner;

static volatile unsigned int rmx_live;      /* callbacks may run the engines */
static volatile unsigned int rmx_overload;  /* bypassed by the guard */
static volatile unsigned int rmx_rate = 44100u;

static struct fx_roll rmx_rolls[RMX_ROLLS];
static struct fx_xpad rmx_xpad;
static struct fx_band_insert rmx_deck_bands[2];
static struct rmx_fifo rmx_fifos[2];
static struct fx_frame rmx_scratch[RMX_CHUNK];

/* What the DJ chose. Written by the input threads, read anywhere. */
static volatile unsigned int rmx_source = RMX_SOURCE_CH1;
static volatile unsigned int rmx_pick = RMX_PICK_ROLL;    /* the lit X-PAD FX button */
static volatile unsigned int rmx_hold;
static volatile unsigned int rmx_mode;                    /* the panel owns the pads */
static volatile unsigned int rmx_volume = 80u;            /* percent */
static volatile unsigned int rmx_muted;
static volatile int rmx_semitones;
static volatile unsigned int rmx_reference_deck;          /* whose tempo MASTER uses */
/* The strip: the section under the finger (or held), its finer octave, and
   which effect it started, so a release stops what was started. */
static volatile int rmx_section = -1;
static volatile unsigned int rmx_section_fine;
static volatile unsigned int rmx_strip_effect;            /* 0 none, pick + 1 */
static volatile unsigned int rmx_strip_roll;              /* roll index + 1 */
static volatile unsigned int rmx_strip_touching;
static unsigned int rmx_press;
/* Pads: HOLD and OVERDUB act on release unless a sound was pressed with them,
   which makes that press a mute or a delete instead, as on the RMX. */
static volatile unsigned int rmx_hold_down, rmx_overdub_down, rmx_combined;

static volatile unsigned int rmx_bpm_x100[2];
static pthread_t rmx_worker;
static volatile unsigned int rmx_worker_running;
static int rmx_worker_started;

/* Diagnostics, written by the audio thread and read by report(). */
static volatile unsigned int rmx_blocks, rmx_slow_blocks, rmx_worst_us, rmx_largest_block;
/* The roll and the fifo assume each deck's stream is called once per block,
   in order. These say whether it is: frames per deck and on the master since
   the last report, and calls whose position did not move. */
static volatile unsigned int rmx_deck_frames[2], rmx_master_frames, rmx_deck_repeats[2];
static int rmx_deck_last_position[2] = {-1, -1};
static uint64_t rmx_report_us;
static unsigned int rmx_reports;

/* Resident memory: three roll buffers, five pattern tracks, four sounds. */
#define RMX_BUFFERS (RMX_ROLLS + FX_XPAD_TRACKS + FX_XPAD_SOUNDS)
static void *rmx_buffers[RMX_BUFFERS];
static unsigned long rmx_buffer_bytes[RMX_BUFFERS];
static unsigned int rmx_buffer_count;
/* Sounds loaded from the drive, which replace the synthesised ones. */
static void *rmx_wav[FX_XPAD_SOUNDS];
static unsigned long rmx_wav_bytes[FX_XPAD_SOUNDS];

struct rmx_timeval { long seconds, microseconds; };
static uint64_t rmx_now_us(void)
{
    struct rmx_timeval value;
    if (gettimeofday(&value, 0)) return 0;
    return (uint64_t)(unsigned long)value.seconds * 1000000u +
           (uint64_t)(unsigned long)value.microseconds;
}

/* ------------------------------------------------------------- tempo --- */

static float rmx_bpm_of(unsigned int deck)
{
    unsigned int x100 = deck < 2u ? __atomic_load_n(&rmx_bpm_x100[deck], __ATOMIC_SEQ_CST) : 0u;
    return x100 >= 4000u && x100 <= 30000u ? (float)x100 * 0.01f : 120.0f;
}

/* The deck whose tempo the X-PAD follows: the source's, or for MASTER the
   deck that last pressed a pad. */
static unsigned int rmx_tempo_deck(void)
{
    unsigned int source = __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST);
    return source < 2u ? source : __atomic_load_n(&rmx_reference_deck, __ATOMIC_SEQ_CST) & 1u;
}

static unsigned int rmx_frames_of_beats(float beats)
{
    float frames_per_beat = (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST) * 60.0f /
                            rmx_bpm_of(rmx_tempo_deck());
    return fx_frames_of(beats, frames_per_beat);
}

static unsigned int rmx_section_frames(int section, unsigned int fine)
{
    if (section < 0 || section >= (int)RMX_STRIP_SECTIONS) section = 3;
    return rmx_frames_of_beats(rmx_strip_beats[section] * (fine ? 0.5f : 1.0f));
}

/* The tempo lives behind a native accessor; it is read here, off the audio
   and input threads, and published as a number. */
static void *rmx_tempo_worker(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&rmx_worker_running, __ATOMIC_SEQ_CST)) {
        for (unsigned int deck = 0; deck < 2u; deck++) {
            int bpm = rmx_bpm_reader(deck);
            __atomic_store_n(&rmx_bpm_x100[deck],
                             bpm > 0 && bpm != 0xffff ? (unsigned int)bpm : 0u, __ATOMIC_SEQ_CST);
        }
        usleep(50000u);
    }
    return 0;
}

/* -------------------------------------------------------------- fifo --- */

static void rmx_fifo_push(struct rmx_fifo *fifo, const struct fx_frame *in, unsigned int frames)
{
    unsigned int head = fifo->head;
    unsigned int tail = __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
    unsigned int room = RMX_FIFO_FRAMES - (head - tail);
    if (frames > room) { fifo->overruns++; frames = room; }
    for (unsigned int i = 0; i < frames; i++) fifo->data[(head + i) & (RMX_FIFO_FRAMES - 1u)] = in[i];
    __atomic_store_n(&fifo->head, head + frames, __ATOMIC_RELEASE);
}

static void rmx_fifo_pop(struct rmx_fifo *fifo, struct fx_frame *out, unsigned int frames)
{
    unsigned int head = __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE);
    unsigned int tail = fifo->tail;
    /* Only the reader moves the tail, so a flush is asked for, not done. */
    if (__atomic_exchange_n(&fifo->flush, 0u, __ATOMIC_SEQ_CST)) tail = head;
    if (head - tail > RMX_FIFO_BACKLOG) {
        /* The deck ran ahead, or the master stalled: keep the newest. */
        tail = head - RMX_FIFO_KEEP;
        fifo->skips++;
    }
    unsigned int available = head - tail, take = available < frames ? available : frames;
    for (unsigned int i = 0; i < take; i++) out[i] = fifo->data[(tail + i) & (RMX_FIFO_FRAMES - 1u)];
    for (unsigned int i = take; i < frames; i++) out[i].left = out[i].right = 0.0f;
    if (take < frames) fifo->underruns++;
    __atomic_store_n(&fifo->tail, tail + take, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------- audio --- */

static int rmx_running(void)
{
    return __atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&rmx_overload, __ATOMIC_SEQ_CST);
}

static void rmx_roll_effect(void *context, struct fx_frame *io, unsigned int frames)
{
    fx_roll_process((struct fx_roll *)context, io, frames);
}

/* A deck's roll, on the bands ISOLATOR's FX buttons select for that deck. */
static void rmx_deck_roll(unsigned int deck, const void *reader, int position,
                          struct rx3_stereo *output, unsigned int frames)
{
    (void)reader;
    if (deck >= 2u) return;
    rmx_deck_frames[deck] += frames;
    if (position == rmx_deck_last_position[deck]) rmx_deck_repeats[deck]++;
    rmx_deck_last_position[deck] = position;
    if (!rmx_running()) return;
    unsigned int mask = framework->bands ? framework->bands->mask(deck) : RX3_BAND_ALL;
    fx_band_insert_process(&rmx_deck_bands[deck], (struct fx_frame *)(void *)output, frames, mask,
                           !fx_roll_active(&rmx_rolls[deck]), rmx_roll_effect, &rmx_rolls[deck]);
}

/* The OVERDUB hears a deck's roll after everything this runtime does to it. */
static void rmx_deck_tap(unsigned int deck, const void *reader, int position,
                         struct rx3_stereo *output, unsigned int frames)
{
    (void)reader; (void)position;
    if (deck < 2u && rmx_running() &&
        __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) == deck)
        rmx_fifo_push(&rmx_fifos[deck], (const struct fx_frame *)(const void *)output, frames);
}

static void rmx_master(struct rx3_stereo *output, unsigned int frames)
{
    rmx_master_frames += frames;
    if (!rmx_running()) return;
    uint64_t began = rmx_now_us();
    struct fx_frame *bus = (struct fx_frame *)(void *)output;
    fx_roll_process(&rmx_rolls[2], bus, frames);
    unsigned int source = __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST);
    for (unsigned int at = 0; at < frames; at += RMX_CHUNK) {
        unsigned int n = frames - at < RMX_CHUNK ? frames - at : RMX_CHUNK;
        /* What a running roll sounds like, for the ROLL track: the master
           itself, or the deck's stream through its fifo. Copied before the
           X-PAD adds itself, so the OVERDUB never records its own output. */
        const struct fx_frame *roll = 0;
        if (source == RMX_SOURCE_MASTER) {
            if (fx_roll_active(&rmx_rolls[2])) {
                memcpy(rmx_scratch, bus + at, n * sizeof(*bus));
                roll = rmx_scratch;
            }
        } else {
            rmx_fifo_pop(&rmx_fifos[source & 1u], rmx_scratch, n);
            if (fx_roll_active(&rmx_rolls[source & 1u])) roll = rmx_scratch;
        }
        fx_xpad_process(&rmx_xpad, roll, bus + at, n);
    }
    unsigned int spent = (unsigned int)(rmx_now_us() - began);
    unsigned int rate = __atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST);
    unsigned int budget = rate ? (unsigned int)((uint64_t)frames * 1000000u / rate) : 0u;
    rmx_blocks++;
    if (spent > rmx_worst_us) rmx_worst_us = spent;
    if (frames > rmx_largest_block) rmx_largest_block = frames;
    if (budget && (float)spent > (float)budget * RMX_OVERLOAD_SHARE) {
        if (++rmx_slow_blocks >= RMX_OVERLOAD_RUN)
            __atomic_store_n(&rmx_overload, 1u, __ATOMIC_SEQ_CST);
    } else {
        rmx_slow_blocks = 0u;
    }
}

/* ------------------------------------------------------------ controls --- */

static unsigned int rmx_roll_for_source(void)
{
    unsigned int source = __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST);
    return source == RMX_SOURCE_MASTER ? 2u : source;
}

/* Stop whatever the strip started. */
static void rmx_strip_stop(void)
{
    unsigned int roll = __atomic_exchange_n(&rmx_strip_roll, 0u, __ATOMIC_SEQ_CST);
    if (roll) fx_roll_set(&rmx_rolls[roll - 1u], 0u, rmx_press);
    if (__atomic_exchange_n(&rmx_strip_effect, 0u, __ATOMIC_SEQ_CST) > 1u)
        fx_xpad_repeat(&rmx_xpad, 0u, 0u);
    __atomic_store_n(&rmx_section, -1, __ATOMIC_SEQ_CST);
}

/* The strip, under the selected X-PAD FX: ROLL rolls the source at the
   section's length; a sound repeats at it. A section reached by sliding
   keeps the same roll start, as the RMX does. */
static void rmx_strip_play(int section, int first)
{
    unsigned int pick = __atomic_load_n(&rmx_pick, __ATOMIC_SEQ_CST);
    unsigned int fine = __atomic_load_n(&rmx_section_fine, __ATOMIC_SEQ_CST);
    unsigned int frames = rmx_section_frames(section, fine);
    __atomic_store_n(&rmx_section, section, __ATOMIC_SEQ_CST);
    if (pick == RMX_PICK_ROLL) {
        unsigned int roll = __atomic_load_n(&rmx_strip_roll, __ATOMIC_SEQ_CST);
        if (first || !roll) {
            if (roll) fx_roll_set(&rmx_rolls[roll - 1u], 0u, rmx_press);
            roll = rmx_roll_for_source() + 1u;
            rmx_press++;
        }
        fx_roll_set(&rmx_rolls[roll - 1u], frames, rmx_press);
        __atomic_store_n(&rmx_strip_roll, roll, __ATOMIC_SEQ_CST);
    } else {
        fx_xpad_repeat(&rmx_xpad, pick - 1u, frames);
    }
    __atomic_store_n(&rmx_strip_effect, pick + 1u, __ATOMIC_SEQ_CST);
}

static void rmx_set_hold(unsigned int on)
{
    __atomic_store_n(&rmx_hold, on ? 1u : 0u, __ATOMIC_SEQ_CST);
    /* HOLD let go with no finger on the strip: what it kept stops. With a
       finger on it, the release will. */
    if (!on && !__atomic_load_n(&rmx_strip_touching, __ATOMIC_SEQ_CST)) rmx_strip_stop();
}

/* An X-PAD FX button: select it, and a sound also plays at once. Choosing
   another while the strip holds an effect moves the strip to it. */
static void rmx_pick_button(unsigned int pick)
{
    if (pick >= RMX_PICKS) return;
    unsigned int was = __atomic_exchange_n(&rmx_pick, pick, __ATOMIC_SEQ_CST);
    if (pick != RMX_PICK_ROLL) fx_xpad_hit(&rmx_xpad, pick - 1u);
    int section = __atomic_load_n(&rmx_section, __ATOMIC_SEQ_CST);
    if (was != pick && section >= 0) {
        rmx_strip_stop();
        rmx_strip_play(section, 1);
    }
}

static void rmx_cycle_source(void)
{
    rmx_strip_stop();
    unsigned int source = (__atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) + 1u) % 3u;
    __atomic_store_n(&rmx_source, source, __ATOMIC_SEQ_CST);
    /* The fifos start over so the new source is not heard late. */
    for (unsigned int deck = 0; deck < 2u; deck++)
        __atomic_store_n(&rmx_fifos[deck].flush, 1u, __ATOMIC_SEQ_CST);
}

static void rmx_publish_volume(void)
{
    fx_xpad_volume(&rmx_xpad, __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST) ? 0u :
                   __atomic_load_n(&rmx_volume, __ATOMIC_SEQ_CST) * 10u);
}

/* PITCH moves the sounds and every roll together, as the RMX's knob does. */
static void rmx_set_pitch(int semitones)
{
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    __atomic_store_n(&rmx_semitones, semitones, __ATOMIC_SEQ_CST);
    fx_xpad_pitch(&rmx_xpad, semitones * 100);
    for (unsigned int i = 0; i < RMX_ROLLS; i++) fx_roll_pitch(&rmx_rolls[i], semitones * 100);
}

static void rmx_overdub(void)
{
    fx_xpad_overdub(&rmx_xpad, rmx_frames_of_beats(FX_XPAD_PATTERN_BEATS));
}

/* ---------------------------------------------------------------- pads --- */

/* While the panel is up, both decks' pads are the X-PAD FX section:
 *   1 KICK  2 SNARE  3 CLAP  4 HI HAT  5 ROLL  6 HOLD  7 OVERDUB  8 SOURCE
 * HOLD + a sound mutes that sound's track, OVERDUB + a sound deletes it,
 * as on the RMX. SHIFT + 1..5 deletes too; SHIFT + 6 and 7 move PITCH a
 * semitone, SHIFT + 8 puts it back. A release is still taken after the panel
 * closes, so nothing sticks because the screen changed under a finger. */
static int rmx_pad(const struct rx3_pad_event *event)
{
    unsigned int code = event->code, operation = event->operation;
    if (!__atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) || code < 0x4117u || code > 0x411eu)
        return 0;
    unsigned int pad = code - 0x4117u;
    unsigned int deck = event->deck < 2u ? event->deck : 0u;
    int release = (operation & 0x0eu) == 2u;
    unsigned int held = __atomic_load_n(&rmx_hold_down, __ATOMIC_SEQ_CST) |
                        __atomic_load_n(&rmx_overdub_down, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&rmx_mode, __ATOMIC_SEQ_CST) && !(release && pad >= 5u && pad <= 6u && held))
        return 0;
    if (!operation) {
        __atomic_store_n(&rmx_reference_deck, deck, __ATOMIC_SEQ_CST);
        if (framework->input->shift_held(RX3_NO_DECK)) {
            if (pad <= 4u) fx_xpad_delete(&rmx_xpad, pad < 4u ? pad : FX_XPAD_ROLL_TRACK);
            else if (pad == 5u) rmx_set_pitch(__atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST) - 1);
            else if (pad == 6u) rmx_set_pitch(__atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST) + 1);
            else rmx_set_pitch(0);
            return 1;
        }
        if (pad <= 4u) {
            unsigned int track = pad < 4u ? pad : FX_XPAD_ROLL_TRACK;
            if (__atomic_load_n(&rmx_overdub_down, __ATOMIC_SEQ_CST)) {
                fx_xpad_delete(&rmx_xpad, track);
                __atomic_store_n(&rmx_combined, 1u, __ATOMIC_SEQ_CST);
            } else if (__atomic_load_n(&rmx_hold_down, __ATOMIC_SEQ_CST)) {
                fx_xpad_toggle_mute(&rmx_xpad, track);
                __atomic_store_n(&rmx_combined, 1u, __ATOMIC_SEQ_CST);
            } else {
                rmx_pick_button(pad < 4u ? pad + 1u : RMX_PICK_ROLL);
            }
        } else if (pad == 5u) {
            __atomic_store_n(&rmx_hold_down, 1u, __ATOMIC_SEQ_CST);
            __atomic_store_n(&rmx_combined, 0u, __ATOMIC_SEQ_CST);
        } else if (pad == 6u) {
            __atomic_store_n(&rmx_overdub_down, 1u, __ATOMIC_SEQ_CST);
            __atomic_store_n(&rmx_combined, 0u, __ATOMIC_SEQ_CST);
        } else {
            rmx_cycle_source();
        }
    } else if (release) {
        if (pad == 5u && __atomic_exchange_n(&rmx_hold_down, 0u, __ATOMIC_SEQ_CST) &&
            !__atomic_load_n(&rmx_combined, __ATOMIC_SEQ_CST))
            rmx_set_hold(!__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST));
        if (pad == 6u && __atomic_exchange_n(&rmx_overdub_down, 0u, __ATOMIC_SEQ_CST) &&
            !__atomic_load_n(&rmx_combined, __ATOMIC_SEQ_CST))
            rmx_overdub();
    }
    return 1;
}

/* Lights, while the panel owns the pads: each sound in its colour, full while
   it sounds or is the selected X-PAD FX, blinking while its track is muted;
   HOLD lit when on; OVERDUB blinking on standby and full while it records;
   SOURCE in its colour. Off the panel the player keeps its own lights. */
static const uint32_t rmx_pad_colours[8] = {
    0xff2020u, 0xff8000u, 0xffd000u, 0x00d0ffu, 0x9040ffu, 0xffd000u, 0xff0000u, 0x00ffffu
};
static void rmx_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    (void)deck;
    if (!__atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) || !__atomic_load_n(&rmx_mode, __ATOMIC_SEQ_CST))
        return;
    if (control == RX3_LIGHT_SHIFT) {
        light->state = RX3_LIGHT_ON;
        light->rgb = 0xff6000u;
        return;
    }
    if (control >= 8u) return;
    unsigned int pick = __atomic_load_n(&rmx_pick, __ATOMIC_SEQ_CST);
    unsigned int muted = __atomic_load_n(&rmx_xpad.muted, __ATOMIC_SEQ_CST);
    unsigned int tracks = rmx_xpad.shown_tracks, playing = rmx_xpad.shown_playing;
    light->rgb = rmx_pad_colours[control];
    light->state = RX3_LIGHT_DIM;
    if (control <= 4u) {
        unsigned int track = control < 4u ? control : FX_XPAD_ROLL_TRACK;
        unsigned int this_pick = control < 4u ? control + 1u : RMX_PICK_ROLL;
        if ((tracks & muted & (1u << track))) light->state = RX3_LIGHT_BLINK;
        else if (pick == this_pick || (control < 4u && (playing & (1u << control))))
            light->state = RX3_LIGHT_ON;
    } else if (control == 5u) {
        if (__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)) light->state = RX3_LIGHT_ON;
    } else if (control == 6u) {
        if (rmx_xpad.shown_state == FX_PATTERN_STANDBY) light->state = RX3_LIGHT_BLINK;
        if (rmx_xpad.shown_state == FX_PATTERN_RUN) light->state = RX3_LIGHT_ON;
    } else {
        static const uint32_t sources[3] = {0x00ffffu, 0xff00ffu, 0xffffffu};
        light->rgb = sources[__atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) % 3u];
        light->state = RX3_LIGHT_ON;
    }
}

static void rmx_activate(unsigned int active)
{
    __atomic_store_n(&rmx_mode, active && __atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) ? 1u : 0u,
                     __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------- drive sounds --- */

static uint32_t rmx_le32(const uint8_t *b)
{ return b[0] | ((uint32_t)b[1] << 8u) | ((uint32_t)b[2] << 16u) | ((uint32_t)b[3] << 24u); }
static unsigned int rmx_le16(const uint8_t *b) { return b[0] | ((unsigned int)b[1] << 8u); }

/* A 16-bit PCM WAV, mono or stereo, 22.05 to 96 kHz, into stereo float.
   Anything else is refused and the synthesised sound stays. */
static void rmx_load_wav(unsigned int sound, const char *directory)
{
    char path[512];
    size_t n = 0, m = 0;
    while (directory[n] && n < sizeof(path) - 32u) { path[n] = directory[n]; n++; }
    path[n++] = '/';
    while (rmx_sound_files[sound][m]) path[n++] = rmx_sound_files[sound][m++];
    path[n] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 44 || (unsigned long)size > RMX_WAV_MAX_BYTES) { close(fd); log_line("rmx: a sound file is too large"); return; }
    void *file = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) return;
    const uint8_t *wave = file;
    const uint8_t *format = 0, *data = 0;
    uint32_t data_bytes = 0;
    if (!memcmp(wave, "RIFF", 4u) && !memcmp(wave + 8u, "WAVE", 4u)) {
        size_t at = 12u;
        while (at + 8u <= (size_t)size) {
            uint32_t count = rmx_le32(wave + at + 4u);
            if (count > (size_t)size - at - 8u) break;
            if (!memcmp(wave + at, "fmt ", 4u) && count >= 16u) format = wave + at + 8u;
            if (!memcmp(wave + at, "data", 4u)) { data = wave + at + 8u; data_bytes = count; }
            at += 8u + count + (count & 1u);
        }
    }
    unsigned int channels = format ? rmx_le16(format + 2u) : 0u;
    uint32_t rate = format ? rmx_le32(format + 4u) : 0u;
    if (!format || !data || rmx_le16(format) != 1u || rmx_le16(format + 14u) != 16u ||
        (channels != 1u && channels != 2u) || rate < 22050u || rate > 96000u) {
        munmap(file, (size_t)size);
        log_line("rmx: a sound file is not 16-bit PCM WAV; the built-in sound stays");
        return;
    }
    unsigned int frames = data_bytes / (2u * channels);
    if (frames > rate * RMX_WAV_MAX_SECONDS) frames = rate * RMX_WAV_MAX_SECONDS;
    unsigned long bytes = ((unsigned long)(frames + 2u) * sizeof(struct fx_frame) + 4095u) & ~4095ul;
    void *block = framework->memory->reserve(&rmx_enabled, bytes, RMX_MEMORY_FLOOR_KB)
        ? mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (block == MAP_FAILED) {
        framework->memory->abandon(&rmx_enabled, bytes);
        munmap(file, (size_t)size);
        return;
    }
    struct fx_frame *out = block;
    for (unsigned int i = 0; i < frames; i++) {
        const uint8_t *s = data + (size_t)i * 2u * channels;
        float l = (float)(int16_t)rmx_le16(s) * (1.0f / 32768.0f);
        float r = channels == 2u ? (float)(int16_t)rmx_le16(s + 2u) * (1.0f / 32768.0f) : l;
        out[i].left = l; out[i].right = r;
    }
    out[frames].left = out[frames].right = out[frames + 1u].left = out[frames + 1u].right = 0.0f;
    munmap(file, (size_t)size);
    framework->memory->settle(&rmx_enabled, bytes);
    /* The synthesised sound stays resident; the old file, if any, is freed
       only at stop, because a voice may still be reading it. */
    fx_xpad_set_sound(&rmx_xpad, sound, out, frames + 1u, (float)rate);
    rmx_wav[sound] = block;
    rmx_wav_bytes[sound] = bytes;
    log_line("rmx: a sound was loaded from the drive");
}

static void rmx_load_sounds(void *unused)
{
    (void)unused;
    const char *directory = getenv("RX3_RMX_SOUNDS");
    if (!directory || !directory[0]) return;
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) {
        if (framework->loader->stopping(&rmx_enabled)) return;
        rmx_load_wav(s, directory);
    }
}

/* ----------------------------------------------------------- lifecycle --- */

static void rmx_free_buffers(void)
{
    unsigned long total = 0;
    for (unsigned int i = 0; i < rmx_buffer_count; i++) {
        if (rmx_buffers[i]) munmap(rmx_buffers[i], rmx_buffer_bytes[i]);
        total += rmx_buffer_bytes[i];
        rmx_buffers[i] = 0;
    }
    if (total) framework->memory->release(&rmx_enabled, total);
    rmx_buffer_count = 0u;
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) {
        if (rmx_wav[s]) {
            munmap(rmx_wav[s], rmx_wav_bytes[s]);
            framework->memory->release(&rmx_enabled, rmx_wav_bytes[s]);
        }
        rmx_wav[s] = 0;
    }
}

/* All buffers at once, or none: the ledger is asked for the total first. */
static int rmx_allocate(float rate)
{
    unsigned int roll_frames = fx_rmx_capacity(rate), track_frames = fx_xpad_pattern_capacity(rate);
    unsigned int sound_frames = fx_xpad_synth_frames(rate);
    unsigned long bytes[RMX_BUFFERS], total = 0;
    for (unsigned int i = 0; i < RMX_BUFFERS; i++) {
        unsigned int frames = i < RMX_ROLLS ? roll_frames :
                              i < RMX_ROLLS + FX_XPAD_TRACKS ? track_frames : sound_frames;
        bytes[i] = ((unsigned long)frames * sizeof(struct fx_frame) + 4095u) & ~4095ul;
        total += bytes[i];
    }
    if (!framework->memory->reserve(&rmx_enabled, total, RMX_MEMORY_FLOOR_KB)) {
        log_line("rmx: not enough free memory for the X-PAD; it stays off");
        return 0;
    }
    for (unsigned int i = 0; i < RMX_BUFFERS; i++) {
        void *block = mmap(0, bytes[i], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            /* Nothing was settled yet: the whole reservation is abandoned. */
            for (unsigned int j = 0; j < i; j++) { munmap(rmx_buffers[j], bytes[j]); rmx_buffers[j] = 0; }
            framework->memory->abandon(&rmx_enabled, total);
            log_line("rmx: buffer allocation failed; the X-PAD stays off");
            return 0;
        }
        /* Touch every page now, on the startup thread, so the audio thread
           never takes the first fault on a buffer. */
        memset(block, 0, bytes[i]);
        rmx_buffers[i] = block;
        rmx_buffer_bytes[i] = bytes[i];
    }
    rmx_buffer_count = RMX_BUFFERS;
    framework->memory->settle(&rmx_enabled, total);
    for (unsigned int i = 0; i < RMX_ROLLS; i++) fx_roll_init(&rmx_rolls[i], rmx_buffers[i], roll_frames);
    struct fx_frame *tracks[FX_XPAD_TRACKS];
    for (unsigned int t = 0; t < FX_XPAD_TRACKS; t++) tracks[t] = rmx_buffers[RMX_ROLLS + t];
    fx_xpad_init(&rmx_xpad, (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST), tracks, track_frames);
    for (unsigned int s = 0; s < FX_XPAD_SOUNDS; s++) {
        struct fx_frame *sound = rmx_buffers[RMX_ROLLS + FX_XPAD_TRACKS + s];
        unsigned int length = fx_xpad_synthesize(s, sound, sound_frames,
                                                 (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST));
        fx_xpad_set_sound(&rmx_xpad, s, sound, length, (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST));
    }
    for (unsigned int deck = 0; deck < 2u; deck++)
        fx_band_insert_init(&rmx_deck_bands[deck], (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST));
    return 1;
}

static void rmx_feature_remove(void)
{
    __atomic_store_n(&rmx_live, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_mode, 0u, __ATOMIC_SEQ_CST);
    framework->panels->unregister_row(&rmx_row);
    /* Each release drains its stage before returning, so no engine is still
       running when the buffers go. */
    framework->audio->release_master(&rmx_master_owner);
    framework->audio->release_deck_stream(&rmx_deck_tap_owner);
    framework->audio->release_deck_stream(&rmx_deck_roll_owner);
    framework->input->unregister_owner(&rmx_enabled);
    if (framework->loader) framework->loader->release(&rmx_enabled);
    if (rmx_worker_started) {
        __atomic_store_n(&rmx_worker_running, 0u, __ATOMIC_SEQ_CST);
        pthread_join(rmx_worker, 0);
        rmx_worker_started = 0;
    }
    rmx_free_buffers();
}

/* 0 only when the player is not the one this was written against: a guard
   that differs refuses the whole runtime, as every other module does. Too
   little memory is not that, and steps aside with a line in the log. */
static int rmx_feature_install(void)
{
    if (!rmx_enabled) return 0;
    if (!framework->audio->claim_deck_stream_at || !framework->audio->claim_master_at) {
        log_line("rmx refused: the core has no chained audio stages");
        return 0;
    }
    if (memcmp(rmx_bpm_prologue, rmx_play_bpm_guard, 8)) {
        log_line("rmx refused: the BPM accessor prologue differs");
        return 0;
    }
    /* Sized for the rate the deck streams run at (44.1 kHz on the RX3's own
       packages); a faster device keeps the frames, which the engines clamp. */
    if (!rmx_allocate(RMX_SIZING_RATE)) return 1;
    __atomic_store_n(&rmx_xpad.volume_permille, 800u, __ATOMIC_SEQ_CST);
    rmx_publish_volume();
    const struct rx3_input_service *input = framework->input;
    /* The pad-mode keys must be able to take the pads back on 1.19. */
    if (!input->claim_mode_keys(&rmx_enabled)) {
        log_line("rmx refused: physical pad-mode dispatcher guard rejected");
        return 0;
    }
    /* Ahead of Samples (10) and Stems (20): the panel that is up owns the
       pads, and only one is up at a time. */
    if (!input->register_pad(&rmx_enabled, 5u, rmx_pad)) return 0;
    if (!input->register_lights(&rmx_enabled, RX3_LIGHTS_PADS, rmx_light))
        log_line("rmx: pad lights unavailable, guard rejected");
    if (!framework->audio->claim_deck_stream_at(&rmx_deck_roll_owner, RMX_DECK_ROLL_PRIORITY, rmx_deck_roll) ||
        !framework->audio->claim_deck_stream_at(&rmx_deck_tap_owner, RMX_DECK_TAP_PRIORITY, rmx_deck_tap) ||
        !framework->audio->claim_master_at(&rmx_master_owner, RMX_MASTER_PRIORITY, rmx_master)) {
        log_line("rmx refused: an audio stage could not be claimed");
        return 0;
    }
    __atomic_store_n(&rmx_worker_running, 1u, __ATOMIC_SEQ_CST);
    if (pthread_create(&rmx_worker, 0, rmx_tempo_worker, 0)) {
        __atomic_store_n(&rmx_worker_running, 0u, __ATOMIC_SEQ_CST);
        log_line("rmx: tempo worker could not start; the X-PAD uses 120 BPM");
    } else {
        rmx_worker_started = 1;
    }
    if (framework->loader && framework->loader->claim(&rmx_enabled)) {
        const struct rx3_load_job job = {rmx_load_sounds, 0, 0};
        if (!framework->loader->submit(&rmx_enabled, &job))
            log_line("rmx: sounds from the drive could not be queued; built-in sounds play");
    }
    if (!framework->panels->register_row(&rmx_row)) {
        log_line("rmx refused: panel slot 2 is taken");
        return 0;
    }
    __atomic_store_n(&rmx_live, 1u, __ATOMIC_SEQ_CST);
    log_line("rmx: X-PAD installed; hardware validation pending");
    return 1;
}

static void rmx_started_audio(unsigned int rate)
{
    if (rate >= 8000u && rate <= 192000u) {
        __atomic_store_n(&rmx_rate, rate, __ATOMIC_SEQ_CST);
        /* The sounds keep the rate they were made at; the section plays
           them at the device's. */
        rmx_xpad.rate = (float)rate;
    }
    log_number("rmx sample rate = ", rate);
}

/* From the core's diagnostic worker, every five seconds: enough to tell on
   hardware whether the stages share a thread, whether the tempo reads right
   and how much time the engines take. */
static void rmx_report(void)
{
    if (!__atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST)) return;
    /* The log is shared and capped: every report for the first minute, then
       one every five, so a long set leaves room for everybody else's lines.
       The counters keep running between the reports that are written. */
    unsigned int report = rmx_reports++;
    if (report >= 12u && report % 60u) return;
    uint64_t now = rmx_now_us();
    unsigned int elapsed_ms = rmx_report_us ? (unsigned int)((now - rmx_report_us) / 1000u) : 0u;
    rmx_report_us = now;
    /* Each figure against the sample rate times the elapsed time: equal for a
       stream called once per block, a multiple of it for one that is not. */
    log_number("rmx report ms = ", elapsed_ms);
    log_number("rmx master frames = ", rmx_master_frames);
    log_number("rmx deck 1 frames = ", rmx_deck_frames[0]);
    log_number("rmx deck 2 frames = ", rmx_deck_frames[1]);
    log_number("rmx deck 1 repeated positions = ", rmx_deck_repeats[0]);
    log_number("rmx deck 2 repeated positions = ", rmx_deck_repeats[1]);
    rmx_master_frames = rmx_deck_frames[0] = rmx_deck_frames[1] = 0u;
    log_number("rmx blocks = ", rmx_blocks);
    log_number("rmx worst us = ", rmx_worst_us);
    log_number("rmx largest block = ", rmx_largest_block);
    log_number("rmx deck 1 bpm x100 = ", rmx_bpm_x100[0]);
    log_number("rmx deck 2 bpm x100 = ", rmx_bpm_x100[1]);
    for (unsigned int deck = 0; deck < 2u; deck++) {
        log_number(deck ? "rmx fifo 2 underruns = " : "rmx fifo 1 underruns = ", rmx_fifos[deck].underruns);
        log_number(deck ? "rmx fifo 2 skips = " : "rmx fifo 1 skips = ", rmx_fifos[deck].skips);
    }
    if (__atomic_load_n(&rmx_overload, __ATOMIC_SEQ_CST))
        log_line("rmx: bypassed by the overload guard until restart");
    rmx_worst_us = 0u;
}

#endif /* RX3_RMX_FEATURE_H */
