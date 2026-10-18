/* SPDX-License-Identifier: MPL-2.0
 * ISOLATOR behaviour, private to rx3_iso_module.c.
 *
 * A three-band isolator on each deck's stream, after everything else this
 * runtime does to it and before the mixer: LOW, MID and HIGH each from kill
 * to +6 dB. Its panel also carries the effect bands: which of LOW / MID / HI
 * COLOR 2 and the RMX X-PAD's deck rolls act on, like a DJM's Beat FX
 * buttons. Every band at 0 dB is a bypass, bit for bit.
 */
#ifndef RX3_ISO_FEATURE_H
#define RX3_ISO_FEATURE_H

#define ISO_PRIORITY 70u                   /* the last client on a deck stream */
/* A band's level in the panel's units: 0 is kill, 100 is 0 dB, 112 is +6 dB,
   half a decibel a step in between. */
#define ISO_LEVEL_MAX 112u
#define ISO_UNITY 100u
#define ISO_FADE 220u                      /* 5 ms into and out of the split */

static int iso_enabled;
static const struct rx3_pad_row iso_row;
static volatile unsigned int iso_live;

struct iso_deck {
    volatile unsigned int level[3];        /* LOW, MID, HIGH */
    volatile unsigned int killed;          /* one bit per band, kept apart from the level */
    volatile unsigned int fx_bands;        /* RX3_BAND_* the effects act on */
    /* Audio thread only. */
    struct fx_bands bands;
    float gain[3];
    unsigned int split, fade;
};
static struct iso_deck iso_decks[2];
static volatile unsigned int iso_rate = 44100u, iso_engine_rate[2];

static float iso_gain_of(unsigned int level, unsigned int killed)
{
    if (killed || !level) return 0.0f;
    if (level == ISO_UNITY) return 1.0f;
    return fx_exp2f(((float)level - (float)ISO_UNITY) * 0.5f * 0.16609640f);   /* dB / 6.0206 */
}

static int iso_flat(const struct iso_deck *deck)
{
    if (__atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST)) return 0;
    for (unsigned int b = 0; b < 3u; b++)
        if (__atomic_load_n(&deck->level[b], __ATOMIC_SEQ_CST) != ISO_UNITY) return 0;
    return 1;
}

static void iso_stream(unsigned int index, const void *reader, int position,
                       struct rx3_stereo *output, unsigned int frames)
{
    (void)reader; (void)position;
    if (index >= 2u || !__atomic_load_n(&iso_live, __ATOMIC_SEQ_CST)) return;
    struct iso_deck *deck = &iso_decks[index];
    unsigned int rate = __atomic_load_n(&iso_rate, __ATOMIC_SEQ_CST);
    if (rate != iso_engine_rate[index]) {
        fx_bands_init(&deck->bands, (float)rate);
        iso_engine_rate[index] = rate;
    }
    int flat = iso_flat(deck);
    float target[3];
    unsigned int killed = __atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST);
    for (unsigned int b = 0; b < 3u; b++)
        target[b] = iso_gain_of(__atomic_load_n(&deck->level[b], __ATOMIC_SEQ_CST), killed & (1u << b));
    /* Every band back at unity, and arrived there: fade back to the plain
       signal, after which nothing is computed. */
    int settled = flat && fx_absf(deck->gain[0] - 1.0f) < 1e-4f &&
                  fx_absf(deck->gain[1] - 1.0f) < 1e-4f && fx_absf(deck->gain[2] - 1.0f) < 1e-4f;
    if (!deck->split && !deck->fade && flat) return;
    if (!flat && !deck->split) {
        fx_bands_reset(&deck->bands);
        for (unsigned int b = 0; b < 3u; b++) deck->gain[b] = 1.0f;
        deck->split = 1u;
        deck->fade = ISO_FADE;
    } else if (settled && deck->split) {
        deck->split = 0u;
        deck->fade = ISO_FADE;
    }
    struct fx_frame *io = (struct fx_frame *)(void *)output;
    for (unsigned int i = 0; i < frames; i++) {
        struct fx_band_frame b;
        fx_bands_split(&deck->bands, io[i], &b);
        for (unsigned int k = 0; k < 3u; k++) deck->gain[k] += (target[k] - deck->gain[k]) * 0.005f;
        struct fx_frame y = {
            b.low.left * deck->gain[0] + b.mid.left * deck->gain[1] + b.high.left * deck->gain[2],
            b.low.right * deck->gain[0] + b.mid.right * deck->gain[1] + b.high.right * deck->gain[2]
        };
        if (deck->fade) {
            float t = (float)deck->fade * (1.0f / (float)ISO_FADE);
            float w = deck->split ? 1.0f - t : t;
            y.left = io[i].left + (y.left - io[i].left) * w;
            y.right = io[i].right + (y.right - io[i].right) * w;
            deck->fade--;
        }
        io[i] = y;
    }
}

static void iso_publish_bands(unsigned int index)
{
    if (framework->bands)
        framework->bands->set(&iso_enabled, index, __atomic_load_n(&iso_decks[index].fx_bands, __ATOMIC_SEQ_CST));
}

static void iso_feature_remove(void)
{
    __atomic_store_n(&iso_live, 0u, __ATOMIC_SEQ_CST);
    framework->panels->unregister_row(&iso_row);
    framework->audio->release_deck_stream(&iso_enabled);
    if (framework->bands) framework->bands->withdraw(&iso_enabled);
}

static int iso_feature_install(void)
{
    if (!iso_enabled) return 0;
    if (!framework->audio->claim_deck_stream_at || !framework->bands) {
        log_line("isolator refused: the core has no chained audio stages or band service");
        return 0;
    }
    for (unsigned int index = 0; index < 2u; index++) {
        struct iso_deck *deck = &iso_decks[index];
        for (unsigned int b = 0; b < 3u; b++) { deck->level[b] = ISO_UNITY; deck->gain[b] = 1.0f; }
        deck->killed = 0u;
        deck->fx_bands = RX3_BAND_ALL;
        deck->split = deck->fade = 0u;
        fx_bands_init(&deck->bands, (float)__atomic_load_n(&iso_rate, __ATOMIC_SEQ_CST));
        iso_engine_rate[index] = __atomic_load_n(&iso_rate, __ATOMIC_SEQ_CST);
    }
    if (!framework->bands->provide(&iso_enabled)) {
        log_line("isolator refused: another module provides the effect bands");
        return 0;
    }
    if (!framework->audio->claim_deck_stream_at(&iso_enabled, ISO_PRIORITY, iso_stream)) {
        log_line("isolator refused: the deck stream could not be claimed");
        return 0;
    }
    if (!framework->panels->register_row(&iso_row)) {
        log_line("isolator refused: panel slot 3 is taken");
        return 0;
    }
    __atomic_store_n(&iso_live, 1u, __ATOMIC_SEQ_CST);
    log_line("isolator: installed; hardware validation pending");
    return 1;
}

static void iso_started_audio(unsigned int rate)
{
    if (rate >= 8000u && rate <= 192000u) __atomic_store_n(&iso_rate, rate, __ATOMIC_SEQ_CST);
}

#endif /* RX3_ISO_FEATURE_H */
