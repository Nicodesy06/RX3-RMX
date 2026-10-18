/* SPDX-License-Identifier: MPL-2.0
 * COLOR 2 behaviour, private to rx3_fx2_module.c: the engines on each deck's
 * stream, the two-button chord on the mixer's own Color FX buttons, their
 * lights, the input probe and the tempo worker. There is no screen: COLOR 2
 * is played on the six physical buttons.
 *
 * The chord. Press a Color FX button: the mixer takes it, as stock. While a
 * button is held, press a second one: the mixer never sees that press, its
 * first effect stays, and the second effect starts as COLOR 2 on both
 * channels. The same chord again, or the second effect's button tapped on its
 * own, stops COLOR 2. Every other press goes to the mixer untouched.
 */
#ifndef RX3_FX2_FEATURE_H
#define RX3_FX2_FEATURE_H

static int fx2_enabled;
static volatile unsigned int fx2_live;

static struct fx2_deck fx2_decks[2];
/* One engine per kind and deck, so an echo can ring out under a filter
   chosen after it. Only the echo and the reverb keep memory. */
static struct fx_color fx2_engines[2][FX_COLOR_KINDS];
static struct fx_band_insert fx2_bands[2];
static void *fx2_delay[2][2];
static unsigned long fx2_delay_bytes;
static unsigned int fx2_delay_count;
static volatile unsigned int fx2_rate = 44100u, fx2_engine_rate[2] = {44100u, 44100u};

static volatile unsigned int fx2_bpm_x100[2];
static pthread_t fx2_worker;
static volatile unsigned int fx2_worker_running;
static int fx2_worker_started;

/* The six buttons' key codes and LED IDs, in the mixer's order, from the
   drive. Neither is known yet: the probe below is how to find them. */
static unsigned int fx2_buttons[FX_COLOR_KINDS], fx2_leds[FX_COLOR_KINDS];
static unsigned int fx2_buttons_known, fx2_leds_known;
static unsigned int fx2_depth = FX2_DEFAULT_DEPTH;
static volatile unsigned int fx2_held;        /* buttons down, one bit each */
static volatile unsigned int fx2_consumed;    /* presses the mixer did not see */

static unsigned int fx2_probe_enabled;
static struct fx2_probe_entry fx2_probe[FX2_PROBE_KEYS];
static volatile unsigned int fx2_probe_count, fx2_probe_reported;
static struct fx2_probe_led fx2_probe_leds[FX2_PROBE_LEDS];
static volatile unsigned int fx2_probe_led_count, fx2_probe_leds_reported;

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
    fx_band_insert_init(&fx2_bands[deck], (float)rate);
    fx2_engine_rate[deck] = rate;
}

/* ------------------------------------------------------------- audio --- */

static float fx2_target(unsigned int deck, unsigned int kind)
{
    struct fx2_deck *state = &fx2_decks[deck];
    if (!__atomic_load_n(&state->on, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&state->kind, __ATOMIC_SEQ_CST) != kind) return 0.0f;
    float target = ((float)__atomic_load_n(&state->color, __ATOMIC_SEQ_CST) - (float)FX2_COLOR_CENTRE) *
                   (1.0f / (float)FX2_COLOR_CENTRE);
    return fx_color_amount_active(target) ? target : 0.0f;
}

/* An engine at centre with nothing left to ring is skipped outright: the
   bypass costs nothing and the stream stays bit-exact. */
static int fx2_engine_idle(unsigned int deck, unsigned int kind)
{
    const struct fx_color *engine = &fx2_engines[deck][kind];
    return !fx_color_amount_active(fx2_target(deck, kind)) && !fx_color_amount_active(engine->color) &&
           engine->mix_now < 1e-4f && !fx_color_has_tail(engine) &&
           !(kind == FX_COLOR_SWEEP && engine->gate < 0.9999f);
}

struct fx2_context { unsigned int deck; float bpm; };

static void fx2_effect(void *context, struct fx_frame *io, unsigned int frames)
{
    const struct fx2_context *c = context;
    float param = (float)__atomic_load_n(&fx2_decks[c->deck].param, __ATOMIC_SEQ_CST) *
                  (1.0f / (float)FX2_PARAM_MAX);
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++) {
        if (fx2_engine_idle(c->deck, kind)) continue;
        struct fx_color *engine = &fx2_engines[c->deck][kind];
        if (c->bpm > 0.0f) fx_color_tempo(engine, c->bpm);
        fx_color_process(engine, io, frames, fx2_target(c->deck, kind), param);
    }
}

static void fx2_stream(unsigned int deck, const void *reader, int position,
                       struct rx3_stereo *output, unsigned int frames)
{
    (void)reader; (void)position;
    if (deck >= 2u || !__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return;
    /* A device rate other than the one the engines were built for is taken
       once, here, on the thread that owns them. */
    unsigned int rate = __atomic_load_n(&fx2_rate, __ATOMIC_SEQ_CST);
    if (rate != fx2_engine_rate[deck]) fx2_init_engines(deck, rate);
    int idle = 1;
    for (unsigned int kind = 0; kind < FX_COLOR_KINDS; kind++)
        if (!fx2_engine_idle(deck, kind)) idle = 0;
    unsigned int mask = framework->bands ? framework->bands->mask(deck) : RX3_BAND_ALL;
    if (idle && mask == RX3_BAND_ALL && fx2_bands[deck].mask == RX3_BAND_ALL && !fx2_bands[deck].fade)
        return;
    struct fx2_context context = {deck, fx2_bpm_of(deck)};
    /* On the bands ISOLATOR's FX buttons select, like a DJM's Beat FX. */
    fx_band_insert_process(&fx2_bands[deck], (struct fx_frame *)(void *)output, frames, mask, idle,
                           fx2_effect, &context);
}

/* ------------------------------------------------------------- input --- */

static int fx2_button_of(unsigned int key)
{
    for (unsigned int i = 0; i < FX_COLOR_KINDS; i++) if (fx2_buttons[i] == key) return (int)i;
    return -1;
}

static void fx2_set(unsigned int on, unsigned int kind)
{
    for (unsigned int deck = 0; deck < 2u; deck++) {
        __atomic_store_n(&fx2_decks[deck].kind, kind, __ATOMIC_SEQ_CST);
        __atomic_store_n(&fx2_decks[deck].on, on, __ATOMIC_SEQ_CST);
    }
}

static int fx2_is(unsigned int kind)
{
    return __atomic_load_n(&fx2_decks[0].on, __ATOMIC_SEQ_CST) &&
           __atomic_load_n(&fx2_decks[0].kind, __ATOMIC_SEQ_CST) == kind;
}

static void fx2_probe_key(const struct rx3_key_event *event)
{
    unsigned int count = __atomic_load_n(&fx2_probe_count, __ATOMIC_SEQ_CST);
    unsigned int arguments[3] = {0, 0, 0};
    if (framework->input->key_arguments) framework->input->key_arguments(arguments);
    for (unsigned int i = 0; i < count; i++)
        if (fx2_probe[i].key == event->key && fx2_probe[i].channel == event->channel) {
            fx2_probe[i].count++;
            for (unsigned int a = 0; a < 3u; a++) fx2_probe[i].arguments[a] = arguments[a];
            return;
        }
    if (count >= FX2_PROBE_KEYS) return;
    fx2_probe[count] = (struct fx2_probe_entry){event->key, event->operation, event->channel, 1u,
                                                {arguments[0], arguments[1], arguments[2]}};
    __atomic_store_n(&fx2_probe_count, count + 1u, __ATOMIC_SEQ_CST);
}

/* Consumes only the second button of a chord, the lone tap that stops
   COLOR 2, and those presses' own releases. */
static int fx2_key(const struct rx3_key_event *event)
{
    if (!__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return 0;
    if (fx2_probe_enabled) fx2_probe_key(event);
    if (!fx2_buttons_known) return 0;
    int button = fx2_button_of(event->key);
    if (button < 0) return 0;
    unsigned int bit = 1u << (unsigned int)button;
    unsigned int kind = fx2_button_order[button];
    if (event->operation == 0u) {
        unsigned int others = __atomic_or_fetch(&fx2_held, bit, __ATOMIC_SEQ_CST) & ~bit;
        if (others || fx2_is(kind)) {
            /* A chord starts or stops COLOR 2; a lone tap on its button stops it. */
            fx2_set(others ? !fx2_is(kind) : 0u, kind);
            __atomic_or_fetch(&fx2_consumed, bit, __ATOMIC_SEQ_CST);
            return 1;
        }
        return 0;
    }
    if ((event->operation & ~1u) == 2u) {
        __atomic_and_fetch(&fx2_held, ~bit, __ATOMIC_SEQ_CST);
        return (__atomic_fetch_and(&fx2_consumed, ~bit, __ATOMIC_SEQ_CST) & bit) != 0u;
    }
    return 0;
}

/* The lights: COLOR 2's button lit beside the mixer's own, once the LED IDs
   are known; until then the probe only watches which IDs go past. */
static void fx2_led_filter(unsigned int id, unsigned int channel, struct rx3_light *light)
{
    if (!__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return;
    if (fx2_probe_enabled) {
        unsigned int count = __atomic_load_n(&fx2_probe_led_count, __ATOMIC_SEQ_CST), seen = 0u;
        for (unsigned int i = 0; i < count; i++)
            if (fx2_probe_leds[i].id == id && fx2_probe_leds[i].channel == channel) seen = 1u;
        if (!seen && count < FX2_PROBE_LEDS) {
            fx2_probe_leds[count] = (struct fx2_probe_led){id, channel};
            __atomic_store_n(&fx2_probe_led_count, count + 1u, __ATOMIC_SEQ_CST);
        }
    }
    if (!fx2_leds_known || !__atomic_load_n(&fx2_decks[0].on, __ATOMIC_SEQ_CST)) return;
    unsigned int kind = __atomic_load_n(&fx2_decks[0].kind, __ATOMIC_SEQ_CST);
    for (unsigned int i = 0; i < FX_COLOR_KINDS; i++)
        if (fx2_button_order[i] == kind && fx2_leds[i] == id) {
            light->state = RX3_LIGHT_ON;
            light->rgb = 0xffffffu;
        }
}

/* Six hexadecimal codes, comma separated, in the mixer's order SPACE, DUB
   ECHO, SWEEP, NOISE, CRUSH, FILTER. Anything malformed leaves the list
   unknown rather than half known. */
static unsigned int fx2_parse_six(const char *text, unsigned int out[FX_COLOR_KINDS])
{
    if (!text || !text[0]) return 0u;
    unsigned int index = 0u, value = 0u, digits = 0u;
    for (const char *c = text;; c++) {
        unsigned int nibble;
        if (*c >= '0' && *c <= '9') nibble = (unsigned int)(*c - '0');
        else if (*c >= 'a' && *c <= 'f') nibble = (unsigned int)(*c - 'a') + 10u;
        else if (*c >= 'A' && *c <= 'F') nibble = (unsigned int)(*c - 'A') + 10u;
        else if ((*c == 'x' || *c == 'X') && digits == 1u && value == 0u) { digits = 0u; continue; }
        else if (*c == ',' || *c == '\0') {
            if (!digits || digits > 4u || index >= FX_COLOR_KINDS) return 0u;
            out[index++] = value;
            value = digits = 0u;
            if (*c == '\0') break;
            continue;
        } else return 0u;
        value = value * 16u + nibble;
        digits++;
    }
    for (unsigned int i = 0; i < index; i++)
        for (unsigned int j = i + 1u; j < index; j++)
            if (out[i] == out[j]) return 0u;
    return index == FX_COLOR_KINDS;
}

static void fx2_parse_buttons(const char *text)
{
    fx2_buttons_known = fx2_parse_six(text, fx2_buttons);
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
            /* Nothing was settled yet: the whole reservation is abandoned. */
            for (unsigned int j = 0; j < n; j++) {
                munmap(fx2_delay[j >> 1][j & 1u], bytes);
                fx2_delay[j >> 1][j & 1u] = 0;
            }
            framework->memory->abandon(&fx2_enabled, bytes * 4u);
            fx2_delay_count = 0u;
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
    fx2_parse_buttons(getenv("RX3_FX2_BUTTONS"));
    fx2_leds_known = fx2_parse_six(getenv("RX3_FX2_LEDS"), fx2_leds);
    const char *depth = getenv("RX3_FX2_DEPTH");
    if (depth && depth[0]) {
        unsigned int value = 0u;
        for (const char *c = depth; *c >= '0' && *c <= '9' && value <= FX2_COLOR_MAX; c++)
            value = value * 10u + (unsigned int)(*c - '0');
        if (value <= FX2_COLOR_MAX) fx2_depth = value;
    }
    for (unsigned int deck = 0; deck < 2u; deck++) {
        fx2_decks[deck].on = 0u;
        fx2_decks[deck].kind = FX_COLOR_FILTER;
        fx2_decks[deck].color = fx2_depth;
        fx2_decks[deck].param = FX2_PARAM_MAX / 2u;
        fx2_init_engines(deck, __atomic_load_n(&fx2_rate, __ATOMIC_SEQ_CST));
    }
    const char *probe = getenv("RX3_FX2_PROBE");
    fx2_probe_enabled = probe && probe[0] == '1';
    if ((fx2_buttons_known || fx2_probe_enabled) &&
        !framework->input->register_key(&fx2_enabled, 30u, fx2_key)) {
        log_line("fx2 refused: the key hook could not be shared");
        return 0;
    }
    if ((fx2_leds_known || fx2_probe_enabled) && framework->input->register_led_filter &&
        !framework->input->register_led_filter(&fx2_enabled, fx2_led_filter))
        log_line("fx2: Color FX lights unavailable, guard rejected");
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
    __atomic_store_n(&fx2_live, 1u, __ATOMIC_SEQ_CST);
    log_line(fx2_buttons_known ? "fx2: COLOR 2 installed, two Color FX buttons together add the second"
                               : "fx2: COLOR 2 installed but idle: no buttons.txt with the button codes yet");
    if (fx2_probe_enabled) log_line("fx2: input probe on; key codes and LED IDs follow in this log");
    log_line("fx2: hardware validation pending");
    return 1;
}

static void fx2_started_audio(unsigned int rate)
{
    if (rate >= 8000u && rate <= 192000u) __atomic_store_n(&fx2_rate, rate, __ATOMIC_SEQ_CST);
}

static void fx2_hex(char *at, unsigned int value, unsigned int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned int n = 0; n < digits; n++) at[digits - 1u - n] = hex[(value >> (4u * n)) & 15u];
}

/* The probe's findings go out here, on the diagnostic worker:
   "fx2 probe key=0x4123 op=0 ch=1 n=0012 a=00000000 b=00000000 c=00000000"
   "fx2 probe led id=0x0023 ch=1"
   and a line again whenever a key has arrived more often since. */
static void fx2_report(void)
{
    if (!__atomic_load_n(&fx2_live, __ATOMIC_SEQ_CST)) return;
    static unsigned int counts[FX2_PROBE_KEYS];
    unsigned int count = __atomic_load_n(&fx2_probe_count, __ATOMIC_SEQ_CST);
    for (unsigned int i = 0; i < count; i++) {
        if (fx2_probe[i].count == counts[i]) continue;
        counts[i] = fx2_probe[i].count;
        char line[80] = "fx2 probe key=0x0000 op=0 ch=0 n=0000 a=00000000 b=00000000 c=00000000";
        fx2_hex(line + 16, fx2_probe[i].key & 0xffffu, 4u);
        fx2_hex(line + 24, fx2_probe[i].operation & 15u, 1u);
        fx2_hex(line + 29, fx2_probe[i].channel & 15u, 1u);
        fx2_hex(line + 33, fx2_probe[i].count & 0xffffu, 4u);
        fx2_hex(line + 40, fx2_probe[i].arguments[0], 8u);
        fx2_hex(line + 51, fx2_probe[i].arguments[1], 8u);
        fx2_hex(line + 62, fx2_probe[i].arguments[2], 8u);
        log_line(line);
    }
    unsigned int leds = __atomic_load_n(&fx2_probe_led_count, __ATOMIC_SEQ_CST);
    for (unsigned int i = __atomic_load_n(&fx2_probe_leds_reported, __ATOMIC_SEQ_CST); i < leds; i++) {
        char line[40] = "fx2 probe led id=0x0000 ch=0";
        fx2_hex(line + 19, fx2_probe_leds[i].id & 0xffffu, 4u);
        fx2_hex(line + 27, fx2_probe_leds[i].channel & 15u, 1u);
        log_line(line);
        __atomic_store_n(&fx2_probe_leds_reported, i + 1u, __ATOMIC_SEQ_CST);
    }
    static unsigned int reports;
    if (reports++ < 3u) {
        log_number("fx2 deck 1 bpm x100 = ", fx2_bpm_x100[0]);
        log_number("fx2 deck 2 bpm x100 = ", fx2_bpm_x100[1]);
    }
}

#endif /* RX3_FX2_FEATURE_H */
