/* SPDX-License-Identifier: MPL-2.0
 * COLOR 2 behaviour, private to rx3_fx2_module.c: the engines on each deck's
 * stream, the optional SHIFT + Color FX button mapping, the input probe and
 * the tempo worker.
 */
#ifndef RX3_FX2_FEATURE_H
#define RX3_FX2_FEATURE_H

static int fx2_enabled;
static const struct rx3_pad_row fx2_row;
static volatile unsigned int fx2_live;

static struct fx2_deck fx2_decks[2];
/* One engine per kind and deck, so an echo can ring out under a filter
   chosen after it. Only the echo and the reverb keep memory. */
static struct fx_color fx2_engines[2][FX_COLOR_KINDS];
static void *fx2_delay[2][2];
static unsigned long fx2_delay_bytes;
static unsigned int fx2_delay_count;
static volatile unsigned int fx2_rate = 44100u, fx2_engine_rate[2] = {44100u, 44100u};

static volatile unsigned int fx2_bpm_x100[2];
static pthread_t fx2_worker;
static volatile unsigned int fx2_worker_running;
static int fx2_worker_started;

/* SHIFT + a native Color FX button adds that effect as COLOR 2, once the
   buttons' codes are known. They are not known yet: the probe below is how
   to find them on a deck, and buttons.txt on the drive is how to give them. */
static unsigned int fx2_buttons[FX_COLOR_KINDS];
static unsigned int fx2_buttons_known;
static volatile unsigned int fx2_buttons_down;      /* presses we consumed */

static unsigned int fx2_probe_enabled;
static struct fx2_probe_entry fx2_probe[FX2_PROBE_KEYS];
static volatile unsigned int fx2_probe_count, fx2_probe_reported;

struct fx2_timeval { long seconds, microseconds; };

static float fx2_bpm_of(unsigned int deck)
{
    unsigned int x100 = deck < 2u ? __atomic_load_n(&fx2_bpm_x100[deck], __ATOMIC_SEQ_CST) : 0u;
    return x100 >= 4000u && x100 <= 30000u ? (float)x100 * 0.01f : 0.0f;
}

static void *fx2_tempo_worker(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&fx2_worker_running, __ATOMIC_SEQ_CST)) {
        for (unsigned int deck = 0; deck < 2u; deck++) {
            int bpm = fx2_bpm_reader(deck);
            __atomic_store_n(&fx2_bpm_x100[deck],
                             bpm > 0 && bpm != 0xffff ? (unsigned int)bpm : 0u, __ATOMIC_SEQ_CST);
        }
        usleep(100000u);
    }
    return 0;
}

static void fx2_init_engines(unsigned int deck, unsigned int rate)
{
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        struct fx_frame *memory = 0;
        if (kind == FX_COLOR_DUB_ECHO) memory = fx2_delay[deck][0];
        if (kind == FX_COLOR_SPACE) memory = fx2_delay[deck][1];
        fx_color_init(&fx2_engines[deck][kind], kind, (float)rate, memory,
                      memory ? FX_COLOR_DELAY_FRAMES : 0u);
    }
    fx2_engine_rate[deck] = rate;
}

/* ------------------------------------------------------------- audio --- */

static void fx2_stream(unsigned int deck, const void *reader, int position,
                       struct rx3_stereo *output, unsigned int frames)
{
    (void)reader; (void)position;
    if (deck >= 2u || !__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return;
    /* A device rate other than the one the engines were built for is taken
       once, here, on the thread that owns them. */
    unsigned int rate = __atomic_load_n(&fx2_rate, __ATOMIC_SEQ_CST);
    if (rate != fx2_engine_rate[deck]) fx2_init_engines(deck, rate);
    struct fx2_deck *state = &fx2_decks[deck];
    unsigned int on = __atomic_load_n(&state->on, __ATOMIC_SEQ_CST);
    unsigned int selected = __atomic_load_n(&state->kind, __ATOMIC_SEQ_CST);
    float bpm = fx2_bpm_of(deck);
    struct fx_frame *io = (struct fx_frame *)(void *)output;
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        struct fx_color *engine = &fx2_engines[deck][kind];
        float target = 0.0f;
        if (on && kind == selected)
            target = ((float)__atomic_load_n(&state->color[kind], __ATOMIC_SEQ_CST) -
                      (float)FX2_COLOR_CENTRE) * (1.0f / (float)FX2_COLOR_CENTRE);
        /* An engine at centre with nothing left to ring is skipped outright:
           the bypass costs nothing and the stream stays bit-exact. */
        if (!fx_color_amount_active(target) && !fx_color_amount_active(engine->color) &&
            engine->mix_now < 1e-4f && !fx_color_has_tail(engine) &&
            !(kind == FX_COLOR_SWEEP && engine->gate < 0.9999f))
            continue;
        if (bpm > 0.0f) fx_color_tempo(engine, bpm);
        float param = (float)__atomic_load_n(&state->param[kind], __ATOMIC_SEQ_CST) *
                      (1.0f / (float)FX2_PARAM_MAX);
        fx_color_process(engine, io, frames, fx_color_amount_active(target) ? target : 0.0f, param);
    }
}

/* ------------------------------------------------------------- input --- */

static void fx2_toggle_kind(unsigned int deck, unsigned int kind)
{
    struct fx2_deck *state = &fx2_decks[deck & 1u];
    if (__atomic_load_n(&state->on, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&state->kind, __ATOMIC_SEQ_CST) == kind) {
        __atomic_store_n(&state->on, 0u, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(&state->kind, kind, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->on, 1u, __ATOMIC_SEQ_CST);
}

/* Never consumes anything but a mapped button pressed under SHIFT, and that
   button's own release. The probe only watches. */
static int fx2_key(const struct rx3_key_event *event)
{
    if (!__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return 0;
    if (fx2_probe_enabled) {
        unsigned int count = __atomic_load_n(&fx2_probe_count, __ATOMIC_SEQ_CST);
        unsigned int seen = 0u;
        for (unsigned int i = 0; i < count; i++)
            if (fx2_probe[i].key == event->key && fx2_probe[i].channel == event->channel) seen = 1u;
        if (!seen && count < FX2_PROBE_KEYS) {
            fx2_probe[count] = (struct fx2_probe_entry){event->key, event->operation, event->channel};
            __atomic_store_n(&fx2_probe_count, count + 1u, __ATOMIC_SEQ_CST);
        }
    }
    if (!fx2_buttons_known) return 0;
    for (unsigned int i = 0; i < FX_COLOR_KINDS; i++) {
        if (event->key != fx2_buttons[i]) continue;
        unsigned int bit = 1u << i;
        if (event->operation == 0u && framework->input->shift_held(RX3_NO_DECK)) {
            /* The mixer selects one effect for both channels; so does this. */
            unsigned int kind = fx2_button_order[i];
            fx2_toggle_kind(0u, kind);
            fx2_toggle_kind(1u, kind);
            __atomic_or_fetch(&fx2_buttons_down, bit, __ATOMIC_SEQ_CST);
            return 1;
        }
        if ((event->operation & ~1u) == 2u &&
            (__atomic_fetch_and(&fx2_buttons_down, ~bit, __ATOMIC_SEQ_CST) & bit))
            return 1;
        return 0;
    }
    return 0;
}

/* RX3_FX2_BUTTONS: six hexadecimal codes, comma separated, in the mixer's
   order SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER. Anything malformed
   leaves the mapping off rather than half on. */
static void fx2_parse_buttons(const char *text)
{
    fx2_buttons_known = 0u;
    if (!text || !text[0]) return;
    unsigned int index = 0u, value = 0u, digits = 0u;
    for (const char *c = text;; c++) {
        unsigned int nibble;
        if (*c >= '0' && *c <= '9') nibble = (unsigned int)(*c - '0');
        else if (*c >= 'a' && *c <= 'f') nibble = (unsigned int)(*c - 'a') + 10u;
        else if (*c >= 'A' && *c <= 'F') nibble = (unsigned int)(*c - 'A') + 10u;
        else if ((*c == 'x' || *c == 'X') && digits == 1u && value == 0u) { digits = 0u; continue; }
        else if (*c == ',' || *c == '\0') {
            if (!digits || digits > 4u || index >= FX_COLOR_KINDS) return;
            fx2_buttons[index++] = value;
            value = digits = 0u;
            if (*c == '\0') break;
            continue;
        } else return;
        value = value * 16u + nibble;
        digits++;
    }
    for (unsigned int i = 0; i < index; i++)
        for (unsigned int j = i + 1u; j < index; j++)
            if (fx2_buttons[i] == fx2_buttons[j]) return;
    fx2_buttons_known = index == FX_COLOR_KINDS;
}

/* ----------------------------------------------------------- lifecycle --- */

static void fx2_free_memory(void)
{
    for (unsigned int deck = 0; deck < 2u; deck++)
        for (unsigned int i = 0; i < 2u; i++) {
            if (fx2_delay[deck][i]) munmap(fx2_delay[deck][i], fx2_delay_bytes);
            fx2_delay[deck][i] = 0;
        }
    if (fx2_delay_count) framework->memory->release(&fx2_enabled, fx2_delay_bytes * fx2_delay_count);
    fx2_delay_count = 0u;
}

static int fx2_allocate(void)
{
    unsigned long bytes = ((unsigned long)FX_COLOR_DELAY_FRAMES * sizeof(struct fx_frame) + 4095u) & ~4095ul;
    if (!framework->memory->reserve(&fx2_enabled, bytes * 4u, FX2_MEMORY_FLOOR_KB)) {
        log_line("fx2: not enough free memory for the echo and reverb; COLOR 2 stays off");
        return 0;
    }
    fx2_delay_bytes = bytes;
    for (unsigned int n = 0; n < 4u; n++) {
        void *block = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            framework->memory->abandon(&fx2_enabled, bytes * (4u - n));
            fx2_delay_count = n;
            fx2_free_memory();
            log_line("fx2: delay allocation failed; COLOR 2 stays off");
            return 0;
        }
        memset(block, 0, bytes);
        fx2_delay[n >> 1][n & 1u] = block;
        fx2_delay_count = n + 1u;
    }
    framework->memory->settle(&fx2_enabled, bytes * 4u);
    return 1;
}

static void fx2_feature_remove(void)
{
    __atomic_store_n(&fx2_live, 0u, __ATOMIC_SEQ_CST);
    framework->panels->unregister_row(&fx2_row);
    framework->audio->release_deck_stream(&fx2_enabled);
    framework->input->unregister_owner(&fx2_enabled);
    if (fx2_worker_started) {
        __atomic_store_n(&fx2_worker_running, 0u, __ATOMIC_SEQ_CST);
        pthread_join(fx2_worker, 0);
        fx2_worker_started = 0;
    }
    fx2_free_memory();
}

static int fx2_feature_install(void)
{
    if (!fx2_enabled) return 0;
    if (!framework->audio->claim_deck_stream_at) {
        log_line("fx2 refused: the core has no chained audio stages");
        return 0;
    }
    if (memcmp(fx2_bpm_prologue, fx2_play_bpm_guard, 8)) {
        log_line("fx2 refused: the BPM accessor prologue differs");
        return 0;
    }
    if (!fx2_allocate()) return 1;
    for (unsigned int deck = 0; deck < 2u; deck++) {
        fx2_decks[deck].on = 0u;
        fx2_decks[deck].kind = FX_COLOR_FILTER;
        for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
            fx2_decks[deck].color[kind] = FX2_COLOR_CENTRE;
            fx2_decks[deck].param[kind] = 50u;
        }
        fx2_init_engines(deck, __atomic_load_n(&fx2_rate, __ATOMIC_SEQ_CST));
    }
    fx2_parse_buttons(getenv("RX3_FX2_BUTTONS"));
    const char *probe = getenv("RX3_FX2_PROBE");
    fx2_probe_enabled = probe && probe[0] == '1';
    if ((fx2_buttons_known || fx2_probe_enabled) &&
        !framework->input->register_key(&fx2_enabled, 30u, fx2_key)) {
        log_line("fx2 refused: the key hook could not be shared");
        return 0;
    }
    if (!framework->audio->claim_deck_stream_at(&fx2_enabled, FX2_PRIORITY, fx2_stream)) {
        log_line("fx2 refused: the deck stream could not be claimed");
        return 0;
    }
    __atomic_store_n(&fx2_worker_running, 1u, __ATOMIC_SEQ_CST);
    if (pthread_create(&fx2_worker, 0, fx2_tempo_worker, 0)) {
        __atomic_store_n(&fx2_worker_running, 0u, __ATOMIC_SEQ_CST);
        log_line("fx2: tempo worker could not start; synced effects assume 120 BPM");
    } else {
        fx2_worker_started = 1;
    }
    if (!framework->panels->register_row(&fx2_row)) {
        log_line("fx2 refused: panel slot 3 is taken");
        return 0;
    }
    __atomic_store_n(&fx2_live, 1u, __ATOMIC_SEQ_CST);
    log_line(fx2_buttons_known ? "fx2: COLOR 2 installed, SHIFT + Color FX buttons mapped"
                               : "fx2: COLOR 2 installed, touchscreen only");
    if (fx2_probe_enabled) log_line("fx2: input probe on; key codes follow in this log");
    log_line("fx2: hardware validation pending");
    return 1;
}

static void fx2_started_audio(unsigned int rate)
{
    if (rate >= 8000u && rate <= 192000u) __atomic_store_n(&fx2_rate, rate, __ATOMIC_SEQ_CST);
}

/* The probe's findings go out here, on the diagnostic worker. One line per
   key: code, the operation it was first seen with, the channel. */
static void fx2_report(void)
{
    if (!__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return;
    unsigned int count = __atomic_load_n(&fx2_probe_count, __ATOMIC_SEQ_CST);
    for (unsigned int i = __atomic_load_n(&fx2_probe_reported, __ATOMIC_SEQ_CST); i < count; i++) {
        /* "fx2 probe key=0x4123 op=0 ch=1", in hexadecimal as buttons.txt wants it. */
        static const char digits[] = "0123456789abcdef";
        char line[40] = "fx2 probe key=0x0000 op=0 ch=0";
        unsigned int key = fx2_probe[i].key & 0xffffu;
        for (unsigned int n = 0; n < 4u; n++) line[19u - n] = digits[(key >> (4u * n)) & 15u];
        line[24] = digits[fx2_probe[i].operation & 15u];
        line[29] = digits[fx2_probe[i].channel & 15u];
        log_line(line);
        __atomic_store_n(&fx2_probe_reported, i + 1u, __ATOMIC_SEQ_CST);
    }
    log_number("fx2 deck 1 bpm x100 = ", fx2_bpm_x100[0]);
}

#endif /* RX3_FX2_FEATURE_H */
