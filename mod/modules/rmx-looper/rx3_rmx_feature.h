/* SPDX-License-Identifier: MPL-2.0
 * ROLL / LOOP FX behaviour, private to rx3_rmx_module.c: the engines, where
 * they sit on the audio stages, the pads while the panel owns them, their
 * lights, and the worker that reads the tempo.
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
static struct fx_loop rmx_loop;
static struct rmx_fifo rmx_fifos[2];
static struct fx_frame rmx_scratch[RMX_CHUNK];

/* What the DJ chose. Written by the input thread, read anywhere. */
static volatile unsigned int rmx_source = RMX_SOURCE_CH1;
static volatile unsigned int rmx_division = 4u;          /* one beat */
static volatile unsigned int rmx_hold;
static volatile unsigned int rmx_mode;                    /* the panel owns the pads */
static volatile unsigned int rmx_level = 80u;             /* percent */
static volatile unsigned int rmx_muted;
static volatile unsigned int rmx_loop_stopped;
static volatile unsigned int rmx_reference_deck;          /* whose tempo MASTER uses */
/* The roll a deck's pads engaged, as an index into rmx_rolls plus one, so a
   release reaches the roll that was pressed even if SOURCE moved since. */
static volatile unsigned int rmx_pad_roll[2];
static volatile unsigned int rmx_pads_held[2];
static volatile unsigned int rmx_touch_roll;              /* roll index plus one */
static unsigned int rmx_press;

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

static void *rmx_buffers[RMX_ROLLS + FX_LOOP_SLOTS + 1u];
static unsigned long rmx_buffer_bytes;
static unsigned int rmx_buffer_count;

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

/* Frames in `division` at the tempo of whoever a roll belongs to. */
static unsigned int rmx_frames_for(unsigned int roll, unsigned int division)
{
    unsigned int deck = roll < 2u ? roll : __atomic_load_n(&rmx_reference_deck, __ATOMIC_SEQ_CST);
    float frames_per_beat = (float)__atomic_load_n(&rmx_rate, __ATOMIC_SEQ_CST) * 60.0f /
                            rmx_bpm_of(deck);
    return fx_frames_of(rmx_division_beats[division < RMX_DIVISIONS ? division : 4u],
                        frames_per_beat);
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

static void rmx_deck_roll(unsigned int deck, const void *reader, int position,
                          struct rx3_stereo *output, unsigned int frames)
{
    (void)reader;
    if (deck >= 2u) return;
    rmx_deck_frames[deck] += frames;
    if (position == rmx_deck_last_position[deck]) rmx_deck_repeats[deck]++;
    rmx_deck_last_position[deck] = position;
    if (rmx_running())
        fx_roll_process(&rmx_rolls[deck], (struct fx_frame *)(void *)output, frames);
}

/* The loop hears a deck after everything this runtime does to it. */
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
        /* The source is copied before the loop is added, so the master
           source never records the loop back into itself. */
        if (source == RMX_SOURCE_MASTER) memcpy(rmx_scratch, bus + at, n * sizeof(*bus));
        else rmx_fifo_pop(&rmx_fifos[source & 1u], rmx_scratch, n);
        fx_loop_process(&rmx_loop, rmx_scratch, bus + at, n);
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

/* Which roll a deck's pads drive: the deck's own, or the master bus. */
static unsigned int rmx_roll_for_deck(unsigned int deck)
{
    return __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) == RMX_SOURCE_MASTER ? 2u : (deck & 1u);
}

static unsigned int rmx_roll_for_source(void)
{
    unsigned int source = __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST);
    return source == RMX_SOURCE_MASTER ? 2u : source;
}

static void rmx_engage(unsigned int roll, unsigned int division, int new_press)
{
    if (roll >= RMX_ROLLS) return;
    if (new_press) rmx_press++;
    fx_roll_set(&rmx_rolls[roll], rmx_frames_for(roll, division), rmx_press);
}

static void rmx_release(unsigned int roll)
{
    if (roll < RMX_ROLLS) fx_roll_set(&rmx_rolls[roll], 0u, rmx_press);
}

/* HOLD let go: every roll no finger is still on comes back. */
static void rmx_release_unheld(void)
{
    for (unsigned int deck = 0; deck < 2u; deck++) {
        unsigned int roll = __atomic_load_n(&rmx_pad_roll[deck], __ATOMIC_SEQ_CST);
        if (roll && !__atomic_load_n(&rmx_pads_held[deck], __ATOMIC_SEQ_CST)) {
            rmx_release(roll - 1u);
            __atomic_store_n(&rmx_pad_roll[deck], 0u, __ATOMIC_SEQ_CST);
        }
    }
}

static void rmx_set_hold(unsigned int on)
{
    __atomic_store_n(&rmx_hold, on ? 1u : 0u, __ATOMIC_SEQ_CST);
    if (!on) rmx_release_unheld();
}

static void rmx_toggle_touch_roll(void)
{
    unsigned int engaged = __atomic_load_n(&rmx_touch_roll, __ATOMIC_SEQ_CST);
    if (engaged) {
        rmx_release(engaged - 1u);
        __atomic_store_n(&rmx_touch_roll, 0u, __ATOMIC_SEQ_CST);
        return;
    }
    unsigned int roll = rmx_roll_for_source();
    rmx_engage(roll, __atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST), 1);
    __atomic_store_n(&rmx_touch_roll, roll + 1u, __ATOMIC_SEQ_CST);
}

/* A new division reaches every roll that is running, keeping its start. */
static void rmx_set_division(unsigned int division)
{
    if (division >= RMX_DIVISIONS) return;
    __atomic_store_n(&rmx_division, division, __ATOMIC_SEQ_CST);
    unsigned int touch = __atomic_load_n(&rmx_touch_roll, __ATOMIC_SEQ_CST);
    if (touch) rmx_engage(touch - 1u, division, 0);
    for (unsigned int deck = 0; deck < 2u; deck++) {
        unsigned int roll = __atomic_load_n(&rmx_pad_roll[deck], __ATOMIC_SEQ_CST);
        if (roll && roll != touch) rmx_engage(roll - 1u, division, 0);
    }
}

static void rmx_cycle_source(int step)
{
    unsigned int source = __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST);
    source = (source + (step < 0 ? 2u : 1u)) % 3u;
    __atomic_store_n(&rmx_source, source, __ATOMIC_SEQ_CST);
    /* A loop already playing keeps playing; only what it records changes.
       The fifos start over so the new source is not heard late. */
    for (unsigned int deck = 0; deck < 2u; deck++)
        __atomic_store_n(&rmx_fifos[deck].flush, 1u, __ATOMIC_SEQ_CST);
}

static void rmx_publish_level(void)
{
    unsigned int level = __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST) ? 0u :
                         __atomic_load_n(&rmx_level, __ATOMIC_SEQ_CST);
    /* Squared, like the channel faders feel. */
    fx_loop_set_level(&rmx_loop, level * level / 10u);
}

static void rmx_loop_button(void)
{
    unsigned int state = rmx_loop.shown_state;
    if (state == FX_LOOP_EMPTY) {
        unsigned int division = __atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST);
        if (division < RMX_LOOP_MIN_DIVISION) division = RMX_LOOP_MIN_DIVISION;
        unsigned int roll = rmx_roll_for_source();
        if (fx_loop_command(&rmx_loop, FX_LOOP_CMD_RECORD, rmx_frames_for(roll, division)))
            __atomic_store_n(&rmx_loop_stopped, 0u, __ATOMIC_SEQ_CST);
    } else if (state == FX_LOOP_PLAY) {
        unsigned int stopped = __atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST);
        if (fx_loop_command(&rmx_loop, stopped ? FX_LOOP_CMD_PLAY : FX_LOOP_CMD_STOP, 0u))
            __atomic_store_n(&rmx_loop_stopped, !stopped, __ATOMIC_SEQ_CST);
    }
}

static void rmx_overdub_button(void)
{
    if (rmx_loop.shown_state != FX_LOOP_PLAY) return;
    (void)fx_loop_command(&rmx_loop, rmx_loop.shown_overdub ? FX_LOOP_CMD_OVERDUB_OFF
                                                            : FX_LOOP_CMD_OVERDUB_ON, 0u);
}

/* One button walks back: the newest layer first, then the loop itself. */
static void rmx_back_button(void)
{
    if (rmx_loop.shown_layers) (void)fx_loop_command(&rmx_loop, FX_LOOP_CMD_UNDO, 0u);
    else (void)fx_loop_command(&rmx_loop, FX_LOOP_CMD_CLEAR, 0u);
}

static void rmx_clear(void)
{
    (void)fx_loop_command(&rmx_loop, FX_LOOP_CMD_CLEAR, 0u);
    __atomic_store_n(&rmx_loop_stopped, 0u, __ATOMIC_SEQ_CST);
}

static void rmx_step_level(int delta)
{
    int level = (int)__atomic_load_n(&rmx_level, __ATOMIC_SEQ_CST) + delta;
    __atomic_store_n(&rmx_level, (unsigned int)(level < 0 ? 0 : level > 100 ? 100 : level),
                     __ATOMIC_SEQ_CST);
    rmx_publish_level();
}

/* ---------------------------------------------------------------- pads --- */

/* SHIFT + pad: the loop's controls, so a set needs no screen at all. */
static void rmx_shift_pad(unsigned int pad)
{
    switch (pad) {
    case 0u: rmx_set_hold(!__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)); break;
    case 1u: rmx_loop_button(); break;
    case 2u: rmx_overdub_button(); break;
    case 3u: rmx_back_button(); break;
    case 4u: rmx_clear(); break;
    case 5u: rmx_cycle_source(1); break;
    case 6u: rmx_step_level(-10); break;
    case 7u: rmx_step_level(10); break;
    }
}

/* While ROLL / LOOP is up the eight pads are the eight divisions, held to
   roll. A release is still taken after the panel closes, so a roll never
   sticks because the screen changed under a finger. */
static int rmx_pad(const struct rx3_pad_event *event)
{
    unsigned int code = event->code, operation = event->operation;
    if (!__atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) || code < 0x4117u || code > 0x411eu)
        return 0;
    unsigned int pad = code - 0x4117u;
    unsigned int deck = event->deck < 2u ? event->deck : 0u;
    unsigned int bit = 1u << pad;
    int release = (operation & 0x0eu) == 2u;
    unsigned int held = __atomic_load_n(&rmx_pads_held[deck], __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&rmx_mode, __ATOMIC_SEQ_CST) && !(release && (held & bit)))
        return 0;
    if (!operation) {
        if (framework->input->shift_held(RX3_NO_DECK)) {
            rmx_shift_pad(pad);
            return 1;
        }
        __atomic_store_n(&rmx_pads_held[deck], held | bit, __ATOMIC_SEQ_CST);
        __atomic_store_n(&rmx_reference_deck, deck, __ATOMIC_SEQ_CST);
        unsigned int roll = __atomic_load_n(&rmx_pad_roll[deck], __ATOMIC_SEQ_CST);
        __atomic_store_n(&rmx_division, pad, __ATOMIC_SEQ_CST);
        if (roll && (held || __atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST))) {
            /* Another pad while rolling: the same slice start, a new length. */
            rmx_engage(roll - 1u, pad, 0);
        } else {
            if (roll) rmx_release(roll - 1u);
            roll = rmx_roll_for_deck(deck) + 1u;
            __atomic_store_n(&rmx_pad_roll[deck], roll, __ATOMIC_SEQ_CST);
            rmx_engage(roll - 1u, pad, 1);
        }
    } else if (release) {
        held &= ~bit;
        __atomic_store_n(&rmx_pads_held[deck], held, __ATOMIC_SEQ_CST);
        unsigned int roll = __atomic_load_n(&rmx_pad_roll[deck], __ATOMIC_SEQ_CST);
        if (!roll) return 1;
        if (held) {
            /* Back to the highest division still held. */
            unsigned int remaining = 7u;
            while (!(held & (1u << remaining))) remaining--;
            __atomic_store_n(&rmx_division, remaining, __ATOMIC_SEQ_CST);
            rmx_engage(roll - 1u, remaining, 0);
        } else if (!__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)) {
            rmx_release(roll - 1u);
            __atomic_store_n(&rmx_pad_roll[deck], 0u, __ATOMIC_SEQ_CST);
        }
    }
    return 1;
}

/* Pads in two banks of colour, short divisions cool and long ones warm; the
   one rolling at full brightness, blinking while HOLD keeps it. With SHIFT,
   the loop's controls. Off the panel the player keeps its own lights. */
static const uint32_t rmx_pad_colours[RMX_DIVISIONS] = {
    0x00c8ffu, 0x0096ffu, 0x0064ffu, 0x3c3cffu, 0x8c28ffu, 0xc800ffu, 0xff0096u, 0xff0040u
};
static void rmx_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    if (!__atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) || !__atomic_load_n(&rmx_mode, __ATOMIC_SEQ_CST))
        return;
    if (control == RX3_LIGHT_SHIFT) {
        light->state = RX3_LIGHT_ON;
        light->rgb = 0xff6000u;
        return;
    }
    if (control >= RMX_DIVISIONS) return;
    if (framework->input->shift_held(RX3_NO_DECK)) {
        unsigned int state = rmx_loop.shown_state;
        static const uint32_t colours[8] = {0xffd000u, 0x00ff40u, 0xff0000u, 0xffffffu,
                                            0xff6000u, 0x00ffffu, 0x0040ffu, 0x0040ffu};
        light->rgb = colours[control];
        light->state = RX3_LIGHT_DIM;
        if ((control == 0u && __atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)) ||
            (control == 1u && state == FX_LOOP_PLAY && !__atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST)) ||
            (control == 2u && rmx_loop.shown_overdub) ||
            (control == 3u && rmx_loop.shown_layers))
            light->state = RX3_LIGHT_ON;
        if (control == 1u && state == FX_LOOP_RECORD) {
            light->rgb = 0xff0000u;
            light->state = RX3_LIGHT_BLINK;
        }
        if (control == 5u) {
            static const uint32_t sources[3] = {0x00ffffu, 0xff00ffu, 0xffffffu};
            light->rgb = sources[__atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) % 3u];
            light->state = RX3_LIGHT_ON;
        }
        return;
    }
    unsigned int d = deck < 2u ? deck : 0u;
    unsigned int rolling = __atomic_load_n(&rmx_pad_roll[d], __ATOMIC_SEQ_CST) &&
                           __atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST) == control;
    light->rgb = rmx_pad_colours[control];
    light->state = !rolling ? RX3_LIGHT_DIM :
                   __atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST) ? RX3_LIGHT_BLINK : RX3_LIGHT_ON;
}

static void rmx_activate(unsigned int active)
{
    __atomic_store_n(&rmx_mode, active && __atomic_load_n(&rmx_live, __ATOMIC_SEQ_CST) ? 1u : 0u,
                     __ATOMIC_SEQ_CST);
}

/* ----------------------------------------------------------- lifecycle --- */

static void rmx_free_buffers(void)
{
    for (unsigned int i = 0; i < rmx_buffer_count; i++) {
        if (rmx_buffers[i]) munmap(rmx_buffers[i], rmx_buffer_bytes);
        rmx_buffers[i] = 0;
    }
    if (rmx_buffer_count) framework->memory->release(&rmx_enabled, rmx_buffer_bytes * rmx_buffer_count);
    rmx_buffer_count = 0u;
}

/* All buffers at once, or none: the ledger is asked for the total first. */
static int rmx_allocate(unsigned int capacity)
{
    const unsigned int count = RMX_ROLLS + FX_LOOP_SLOTS + 1u;
    unsigned long bytes = ((unsigned long)capacity * sizeof(struct fx_frame) + 4095u) & ~4095ul;
    if (!framework->memory->reserve(&rmx_enabled, bytes * count, RMX_MEMORY_FLOOR_KB)) {
        log_line("rmx: not enough free memory for the roll and loop buffers; ROLL / LOOP stays off");
        return 0;
    }
    rmx_buffer_bytes = bytes;
    for (unsigned int i = 0; i < count; i++) {
        void *block = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            framework->memory->abandon(&rmx_enabled, bytes * (count - i));
            rmx_buffer_count = i;
            rmx_free_buffers();
            log_line("rmx: buffer allocation failed; ROLL / LOOP stays off");
            return 0;
        }
        /* Touch every page now, on the startup thread, so the audio thread
           never takes the first fault on a buffer. */
        memset(block, 0, bytes);
        rmx_buffers[i] = block;
        rmx_buffer_count = i + 1u;
    }
    framework->memory->settle(&rmx_enabled, bytes * count);
    for (unsigned int i = 0; i < RMX_ROLLS; i++) fx_roll_init(&rmx_rolls[i], rmx_buffers[i], capacity);
    struct fx_frame *slots[FX_LOOP_SLOTS];
    for (unsigned int i = 0; i < FX_LOOP_SLOTS; i++) slots[i] = rmx_buffers[RMX_ROLLS + 1u + i];
    fx_loop_init(&rmx_loop, rmx_buffers[RMX_ROLLS], slots, capacity);
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
    if (!rmx_allocate(fx_rmx_capacity(RMX_SIZING_RATE))) return 1;
    rmx_publish_level();
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
        log_line("rmx: tempo worker could not start; rolls use 120 BPM");
    } else {
        rmx_worker_started = 1;
    }
    if (!framework->panels->register_row(&rmx_row)) {
        log_line("rmx refused: panel slot 2 is taken");
        return 0;
    }
    __atomic_store_n(&rmx_live, 1u, __ATOMIC_SEQ_CST);
    log_line("rmx: ROLL / LOOP installed; hardware validation pending");
    return 1;
}

static void rmx_started_audio(unsigned int rate)
{
    if (rate >= 8000u && rate <= 192000u) __atomic_store_n(&rmx_rate, rate, __ATOMIC_SEQ_CST);
    log_number("rmx sample rate = ", rate);
}

/* From the core's diagnostic worker, every five seconds: enough to tell
   on hardware whether the two stages share a thread, whether the tempo reads
   right and how much time the engines take. */
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
