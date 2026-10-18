/* SPDX-License-Identifier: MPL-2.0
 * ISOLATOR on the touchscreen, one row per deck in the STATUS position:
 *
 *   [LOW 0] [MID KILL] [HIGH +4]   [FX LOW] [FX MID] [FX HI]
 *
 * A band: tap to kill it and tap again to bring it back at its level; hold
 * and drag to set the level, kill to +6 dB. FX: which bands COLOR 2 and the
 * RMX X-PAD's rolls act on, lit when in; all lit is the whole signal.
 */
#ifndef RX3_ISO_PANEL_H
#define RX3_ISO_PANEL_H

#define ISO_W_FX 3u
#define ISO_WIDGETS 6u

static const char *const iso_band_names[3] = {"LOW ", "MID ", "HIGH "};
static const uint16_t iso_fx_text[3][7] = {
    {'F','X',' ','L','O','W',0}, {'F','X',' ','M','I','D',0}, {'F','X',' ','H','I',0}
};

static const uint16_t *iso_caption(unsigned int index, unsigned int widget, unsigned int part)
{
    (void)part;
    static uint16_t labels[2][3][12];
    index &= 1u;
    if (widget >= ISO_W_FX) return iso_fx_text[widget - ISO_W_FX];
    uint16_t *at = labels[index][widget];
    for (const char *c = iso_band_names[widget]; *c; c++) *at++ = (uint16_t)*c;
    const struct iso_deck *deck = &iso_decks[index];
    unsigned int level = __atomic_load_n(&deck->level[widget], __ATOMIC_SEQ_CST);
    if ((__atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST) & (1u << widget)) || !level) {
        static const char kill[] = "KILL";
        for (const char *c = kill; *c; c++) *at++ = (uint16_t)*c;
        *at = 0;
        return labels[index][widget];
    }
    /* Whole decibels, rounded toward zero; the steps are half a decibel. */
    int tenths = ((int)level - (int)ISO_UNITY) * 5;
    int db = tenths / 10;
    if (db > 0) *at++ = '+';
    if (db < 0) { *at++ = '-'; db = -db; }
    if (db >= 10) *at++ = (uint16_t)('0' + db / 10);
    *at++ = (uint16_t)('0' + db % 10);
    *at = 0;
    return labels[index][widget];
}

static int iso_is_on(unsigned int index, unsigned int widget, unsigned int part)
{
    (void)part;
    const struct iso_deck *deck = &iso_decks[index & 1u];
    if (widget >= ISO_W_FX)
        return (__atomic_load_n(&deck->fx_bands, __ATOMIC_SEQ_CST) >> (widget - ISO_W_FX)) & 1u;
    /* A band lit when it is off its unity: killed, cut or boosted. */
    return (__atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST) & (1u << widget)) ||
           __atomic_load_n(&deck->level[widget], __ATOMIC_SEQ_CST) != ISO_UNITY;
}

static void iso_fire(unsigned int index, unsigned int widget, unsigned int part)
{
    (void)part;
    index &= 1u;
    struct iso_deck *deck = &iso_decks[index];
    if (widget < ISO_W_FX) {
        __atomic_xor_fetch(&deck->killed, 1u << widget, __ATOMIC_SEQ_CST);
        return;
    }
    unsigned int bands = __atomic_xor_fetch(&deck->fx_bands, 1u << (widget - ISO_W_FX), __ATOMIC_SEQ_CST);
    /* No band at all would be no effect; it means every band. */
    if (!(bands & RX3_BAND_ALL)) __atomic_store_n(&deck->fx_bands, RX3_BAND_ALL, __ATOMIC_SEQ_CST);
    iso_publish_bands(index);
}

static unsigned int iso_slider_max(unsigned int index, unsigned int widget)
{ (void)index; (void)widget; return ISO_LEVEL_MAX; }

static unsigned int iso_slider_get(unsigned int index, unsigned int widget)
{ return widget < ISO_W_FX ? __atomic_load_n(&iso_decks[index & 1u].level[widget], __ATOMIC_SEQ_CST) : 0u; }

static void iso_slider_set(unsigned int index, unsigned int widget, unsigned int value,
                           unsigned int committed)
{
    (void)committed;
    if (widget >= ISO_W_FX) return;
    struct iso_deck *deck = &iso_decks[index & 1u];
    /* A detent at 0 dB, a step each side, like the knob's. */
    if (value + 1u >= ISO_UNITY && value <= ISO_UNITY + 1u) value = ISO_UNITY;
    __atomic_store_n(&deck->level[widget], value > ISO_LEVEL_MAX ? ISO_LEVEL_MAX : value, __ATOMIC_SEQ_CST);
    /* Moving a killed band brings it back at the new level. */
    __atomic_and_fetch(&deck->killed, ~(1u << widget), __ATOMIC_SEQ_CST);
}

/* Red while killed. */
static uint16_t iso_colour(unsigned int index, unsigned int widget)
{
    if (widget >= ISO_W_FX) return 0u;
    const struct iso_deck *deck = &iso_decks[index & 1u];
    return (__atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST) & (1u << widget)) ||
           !__atomic_load_n(&deck->level[widget], __ATOMIC_SEQ_CST) ? 0xf800u : 0u;
}

static int iso_needs_refresh(void)
{
    static unsigned int seen[2][5];
    unsigned int now[2][5];
    for (unsigned int index = 0; index < 2u; index++) {
        const struct iso_deck *deck = &iso_decks[index];
        for (unsigned int b = 0; b < 3u; b++) now[index][b] = __atomic_load_n(&deck->level[b], __ATOMIC_SEQ_CST);
        now[index][3] = __atomic_load_n(&deck->killed, __ATOMIC_SEQ_CST);
        now[index][4] = __atomic_load_n(&deck->fx_bands, __ATOMIC_SEQ_CST);
    }
    int changed = memcmp(seen, now, sizeof(now)) != 0;
    memcpy(seen, now, sizeof(now));
    return changed;
}

static const struct rx3_pad_widget iso_widgets[ISO_WIDGETS] = {
    {RX3_PAD_TOGGLE_SLIDER, 4}, {RX3_PAD_TOGGLE_SLIDER, 4}, {RX3_PAD_TOGGLE_SLIDER, 4},
    {RX3_PAD_TOGGLE, 3}, {RX3_PAD_TOGGLE, 3}, {RX3_PAD_TOGGLE, 3}
};

/* Its own STATUS-position pictures: ISOLATOR beside the native BEAT FX. */
static const uint8_t iso_tab_slots[RX3_TAB_STATES] = {0u, 0u, 0u, 0u, 0u, 16u, 17u, 18u};

/* Panel 3 is the status slot: it replaces the native STATUS tab. */
static const struct rx3_pad_row iso_row = {
    .panel_id = 3u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_DECK,
    .count = ISO_WIDGETS, .widgets = iso_widgets,
    .caption = iso_caption, .is_on = iso_is_on, .fire = iso_fire,
    .slider_max = iso_slider_max, .slider_get = iso_slider_get, .slider_set = iso_slider_set,
    .needs_refresh = iso_needs_refresh, .colour = iso_colour, .tab_slots = iso_tab_slots
};

#endif /* RX3_ISO_PANEL_H */
