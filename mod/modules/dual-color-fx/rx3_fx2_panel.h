/* SPDX-License-Identifier: MPL-2.0
 * COLOR 2 on the touchscreen, one row per deck in the STATUS position:
 *
 *   [<  DUB ECHO  >] [COLOR +40 ------|---] [PARAM 50 -----]
 *
 * The name in the middle of the stepper switches COLOR 2 on and off; the
 * arrows choose the effect. COLOR is bipolar: hold and drag it, or tap it to
 * send it back to centre, which is the quickest way out of an effect mid-mix.
 * PARAM the same way, a tap returning it to the middle. The track shows only
 * while a finger moves it, so the captions stay legible. Each effect
 * remembers its own COLOR and PARAMETER.
 */
#ifndef RX3_FX2_PANEL_H
#define RX3_FX2_PANEL_H

enum { FX2_W_KIND, FX2_W_COLOR, FX2_W_PARAM, FX2_WIDGETS };

static const uint16_t fx2_text_less[] = {'<', 0};
static const uint16_t fx2_text_more[] = {'>', 0};

static unsigned int fx2_selected(unsigned int deck)
{
    return __atomic_load_n(&fx2_decks[deck & 1u].kind, __ATOMIC_SEQ_CST) % FX_COLOR_KINDS;
}

/* "COLOR +40", "COLOR -15", "COLOR 0", "PARAM 50": the glyph atlas has the
   digits and both signs. */
static uint16_t *fx2_number(uint16_t *at, const char *prefix, int value, int signed_value)
{
    while (*prefix) *at++ = (uint16_t)*prefix++;
    if (signed_value && value > 0) *at++ = '+';
    if (value < 0) { *at++ = '-'; value = -value; }
    if (value >= 100) *at++ = (uint16_t)('0' + value / 100);
    if (value >= 10) *at++ = (uint16_t)('0' + (value / 10) % 10);
    *at++ = (uint16_t)('0' + value % 10);
    *at = 0;
    return at;
}

static const uint16_t *fx2_caption(unsigned int deck, unsigned int widget, unsigned int part)
{
    static uint16_t labels[2][2][16];
    deck &= 1u;
    unsigned int kind = fx2_selected(deck);
    if (widget == FX2_W_KIND)
        return part == 0u ? fx2_text_less : part == 2u ? fx2_text_more : fx2_names[kind];
    if (widget == FX2_W_COLOR) {
        int value = (int)__atomic_load_n(&fx2_decks[deck].color[kind], __ATOMIC_SEQ_CST) -
                    (int)FX2_COLOR_CENTRE;
        fx2_number(labels[deck][0], "COLOR ", value, 1);
        return labels[deck][0];
    }
    fx2_number(labels[deck][1], "PARAM ",
               (int)__atomic_load_n(&fx2_decks[deck].param[kind], __ATOMIC_SEQ_CST), 0);
    return labels[deck][1];
}

static int fx2_is_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    deck &= 1u;
    unsigned int on = __atomic_load_n(&fx2_decks[deck].on, __ATOMIC_SEQ_CST);
    if (widget == FX2_W_KIND) return part == 1u && on;
    if (widget == FX2_W_COLOR)
        return on && __atomic_load_n(&fx2_decks[deck].color[fx2_selected(deck)], __ATOMIC_SEQ_CST) !=
                     FX2_COLOR_CENTRE;
    return 0;
}

static void fx2_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    deck &= 1u;
    struct fx2_deck *state = &fx2_decks[deck];
    if (widget == FX2_W_KIND) {
        unsigned int kind = fx2_selected(deck);
        if (part == 1u) {
            __atomic_store_n(&state->on, !__atomic_load_n(&state->on, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
            return;
        }
        kind = (kind + (part == 0u ? FX_COLOR_KINDS - 1u : 1u)) % FX_COLOR_KINDS;
        /* The engine left behind rings out on its own; the new one starts at
           its remembered setting. */
        __atomic_store_n(&state->kind, kind, __ATOMIC_SEQ_CST);
        return;
    }
    if (widget == FX2_W_COLOR)
        __atomic_store_n(&state->color[fx2_selected(deck)], FX2_COLOR_CENTRE, __ATOMIC_SEQ_CST);
    if (widget == FX2_W_PARAM)
        __atomic_store_n(&state->param[fx2_selected(deck)], FX2_PARAM_MAX / 2u, __ATOMIC_SEQ_CST);
}

static unsigned int fx2_slider_max(unsigned int deck, unsigned int widget)
{
    (void)deck;
    return widget == FX2_W_COLOR ? FX2_COLOR_MAX : FX2_PARAM_MAX;
}

static unsigned int fx2_slider_get(unsigned int deck, unsigned int widget)
{
    deck &= 1u;
    unsigned int kind = fx2_selected(deck);
    return widget == FX2_W_COLOR ? __atomic_load_n(&fx2_decks[deck].color[kind], __ATOMIC_SEQ_CST)
                                 : __atomic_load_n(&fx2_decks[deck].param[kind], __ATOMIC_SEQ_CST);
}

static void fx2_slider_set(unsigned int deck, unsigned int widget, unsigned int value,
                           unsigned int committed)
{
    (void)committed;
    deck &= 1u;
    unsigned int kind = fx2_selected(deck);
    if (widget == FX2_W_COLOR) {
        /* A centre detent of two steps either side, like the knob's. */
        if (value + 2u >= FX2_COLOR_CENTRE && value <= FX2_COLOR_CENTRE + 2u) value = FX2_COLOR_CENTRE;
        if (value > FX2_COLOR_MAX) value = FX2_COLOR_MAX;
        __atomic_store_n(&fx2_decks[deck].color[kind], value, __ATOMIC_SEQ_CST);
        /* Turning COLOR is a request for the effect. */
        if (value != FX2_COLOR_CENTRE) __atomic_store_n(&fx2_decks[deck].on, 1u, __ATOMIC_SEQ_CST);
    } else {
        __atomic_store_n(&fx2_decks[deck].param[kind], value > FX2_PARAM_MAX ? FX2_PARAM_MAX : value,
                         __ATOMIC_SEQ_CST);
    }
}


static int fx2_needs_refresh(void)
{
    static unsigned int seen[2][4];
    unsigned int now[2][4];
    for (unsigned int deck = 0; deck < 2u; deck++) {
        unsigned int kind = fx2_selected(deck);
        now[deck][0] = __atomic_load_n(&fx2_decks[deck].on, __ATOMIC_SEQ_CST);
        now[deck][1] = kind;
        now[deck][2] = __atomic_load_n(&fx2_decks[deck].color[kind], __ATOMIC_SEQ_CST);
        now[deck][3] = __atomic_load_n(&fx2_decks[deck].param[kind], __ATOMIC_SEQ_CST);
    }
    int changed = memcmp(seen, now, sizeof(now)) != 0;
    memcpy(seen, now, sizeof(now));
    return changed;
}

static const struct rx3_pad_widget fx2_widgets[FX2_WIDGETS] = {
    {RX3_PAD_STEPPER, 6}, {RX3_PAD_TOGGLE_SLIDER, 4}, {RX3_PAD_TOGGLE_SLIDER, 3}
};

/* Its own STATUS-position pictures: COLOR 2 beside the native BEAT FX. */
static const uint8_t fx2_tab_slots[RX3_TAB_STATES] = {0u, 0u, 0u, 0u, 0u, 16u, 17u, 18u};

/* Panel 3 is the status slot: it replaces the native STATUS tab. */
static const struct rx3_pad_row fx2_row = {
    .panel_id = 3u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_DECK,
    .count = FX2_WIDGETS, .widgets = fx2_widgets,
    .caption = fx2_caption, .is_on = fx2_is_on, .fire = fx2_fire,
    .slider_max = fx2_slider_max, .slider_get = fx2_slider_get, .slider_set = fx2_slider_set,
    .needs_refresh = fx2_needs_refresh,
    .tab_slots = fx2_tab_slots
};

#endif /* RX3_FX2_PANEL_H */
