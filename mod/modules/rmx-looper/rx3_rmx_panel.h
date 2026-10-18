/* SPDX-License-Identifier: MPL-2.0
 * ROLL / LOOP FX on the touchscreen: one row across both halves, because the
 * loop is one bus whichever deck feeds it.
 *
 *   [< CH1 >] [< 1/4 >] [ROLL] [HOLD] [REC] [OVERDUB] [UNDO] [LVL 80----]
 *
 * SOURCE picks what the loop records and what the touch ROLL rolls. The pads
 * roll their own deck unless SOURCE is MST, which rolls the master bus.
 */
#ifndef RX3_RMX_PANEL_H
#define RX3_RMX_PANEL_H

enum { RMX_W_SOURCE, RMX_W_DIVISION, RMX_W_ROLL, RMX_W_HOLD, RMX_W_LOOP,
       RMX_W_OVERDUB, RMX_W_BACK, RMX_W_LEVEL, RMX_WIDGETS };

static const uint16_t rmx_text_less[] = {'<', 0};
static const uint16_t rmx_text_more[] = {'>', 0};
static const uint16_t rmx_text_sources[3][4] = {{'C','H','1',0}, {'C','H','2',0}, {'M','S','T',0}};
static const uint16_t rmx_text_roll[] = {'R','O','L','L',0};
static const uint16_t rmx_text_hold[] = {'H','O','L','D',0};
static const uint16_t rmx_text_rec[] = {'R','E','C',0};
static const uint16_t rmx_text_recording[] = {'R','E','C',' ','*',0};
static const uint16_t rmx_text_stop[] = {'S','T','O','P',0};
static const uint16_t rmx_text_play[] = {'P','L','A','Y',0};
static const uint16_t rmx_text_overdub[] = {'O','V','E','R','D','U','B',0};
static const uint16_t rmx_text_undo[] = {'U','N','D','O',0};
static const uint16_t rmx_text_clear[] = {'C','L','E','A','R',0};
static const uint16_t rmx_text_mute[] = {'M','U','T','E',0};

static int rmx_rolling(void)
{
    return __atomic_load_n(&rmx_touch_roll, __ATOMIC_SEQ_CST) ||
           __atomic_load_n(&rmx_pad_roll[0], __ATOMIC_SEQ_CST) ||
           __atomic_load_n(&rmx_pad_roll[1], __ATOMIC_SEQ_CST);
}

static const uint16_t *rmx_caption(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck;
    static uint16_t level[8] = {'L','V','L',' ',0};
    switch (widget) {
    case RMX_W_SOURCE:
        return part == 0u ? rmx_text_less : part == 2u ? rmx_text_more :
               rmx_text_sources[__atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) % 3u];
    case RMX_W_DIVISION:
        return part == 0u ? rmx_text_less : part == 2u ? rmx_text_more :
               rmx_division_text[__atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST) % RMX_DIVISIONS];
    case RMX_W_ROLL: return rmx_text_roll;
    case RMX_W_HOLD: return rmx_text_hold;
    case RMX_W_LOOP:
        if (rmx_loop.shown_state == FX_LOOP_EMPTY) return rmx_text_rec;
        if (rmx_loop.shown_state == FX_LOOP_RECORD) return rmx_text_recording;
        return __atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST) ? rmx_text_play : rmx_text_stop;
    case RMX_W_OVERDUB: return rmx_text_overdub;
    case RMX_W_BACK:
        return !rmx_loop.shown_layers && rmx_loop.shown_state != FX_LOOP_EMPTY
            ? rmx_text_clear : rmx_text_undo;
    default: {
        if (__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST)) return rmx_text_mute;
        unsigned int value = __atomic_load_n(&rmx_level, __ATOMIC_SEQ_CST);
        if (value >= 100u) { level[4] = '1'; level[5] = '0'; level[6] = '0'; level[7] = 0; }
        else { level[4] = (uint16_t)('0' + value / 10u); level[5] = (uint16_t)('0' + value % 10u); level[6] = 0; }
        return level;
    }
    }
}

static int rmx_is_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    switch (widget) {
    case RMX_W_ROLL: return rmx_rolling();
    case RMX_W_HOLD: return (int)__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST);
    case RMX_W_LOOP:
        return rmx_loop.shown_state == FX_LOOP_RECORD ||
               (rmx_loop.shown_state == FX_LOOP_PLAY && !__atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST));
    case RMX_W_OVERDUB: return (int)rmx_loop.shown_overdub;
    case RMX_W_BACK: return rmx_loop.shown_layers != 0u;
    case RMX_W_LEVEL: return !__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST);
    default: return 0;
    }
}

static void rmx_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck;
    switch (widget) {
    case RMX_W_SOURCE:
        if (part != 1u) rmx_cycle_source(part == 0u ? -1 : 1);
        break;
    case RMX_W_DIVISION: {
        unsigned int division = __atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST);
        if (part == 0u && division > 0u) rmx_set_division(division - 1u);
        if (part == 2u && division + 1u < RMX_DIVISIONS) rmx_set_division(division + 1u);
        break;
    }
    case RMX_W_ROLL: rmx_toggle_touch_roll(); break;
    case RMX_W_HOLD: rmx_set_hold(!__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)); break;
    case RMX_W_LOOP: rmx_loop_button(); break;
    case RMX_W_OVERDUB: rmx_overdub_button(); break;
    case RMX_W_BACK: rmx_back_button(); break;
    case RMX_W_LEVEL:
        __atomic_store_n(&rmx_muted, !__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
        rmx_publish_level();
        break;
    }
}

static unsigned int rmx_slider_max(unsigned int deck, unsigned int widget)
{ (void)deck; (void)widget; return 100u; }

static unsigned int rmx_slider_get(unsigned int deck, unsigned int widget)
{ (void)deck; (void)widget; return __atomic_load_n(&rmx_level, __ATOMIC_SEQ_CST); }

static void rmx_slider_set(unsigned int deck, unsigned int widget, unsigned int value,
                           unsigned int committed)
{
    (void)deck; (void)widget; (void)committed;
    __atomic_store_n(&rmx_level, value > 100u ? 100u : value, __ATOMIC_SEQ_CST);
    rmx_publish_level();
}

/* RGB565 accents: red while recording or overdubbing, green while playing. */
static uint16_t rmx_colour(unsigned int deck, unsigned int widget)
{
    (void)deck;
    if (widget == RMX_W_LOOP) {
        if (rmx_loop.shown_state == FX_LOOP_RECORD) return 0xf800u;
        if (rmx_loop.shown_state == FX_LOOP_PLAY && !__atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST))
            return 0x07e0u;
    }
    if (widget == RMX_W_OVERDUB && rmx_loop.shown_overdub) return 0xf800u;
    if (widget == RMX_W_ROLL && rmx_rolling()) return 0x041fu;
    return 0u;
}

/* The row repaints when anything it shows has moved: the loop finishing its
   first pass is the one change nobody touched the screen for. */
static int rmx_needs_refresh(void)
{
    static unsigned int seen[10];
    unsigned int now[10] = {
        rmx_loop.shown_state, rmx_loop.shown_layers, rmx_loop.shown_overdub,
        __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST), __atomic_load_n(&rmx_division, __ATOMIC_SEQ_CST),
        __atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST), (unsigned int)rmx_rolling(),
        __atomic_load_n(&rmx_level, __ATOMIC_SEQ_CST), __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST),
        __atomic_load_n(&rmx_loop_stopped, __ATOMIC_SEQ_CST)
    };
    int changed = memcmp(seen, now, sizeof(now)) != 0;
    memcpy(seen, now, sizeof(now));
    return changed;
}

static const struct rx3_pad_widget rmx_widgets[RMX_WIDGETS] = {
    {RX3_PAD_STEPPER, 3}, {RX3_PAD_STEPPER, 3}, {RX3_PAD_TOGGLE, 2}, {RX3_PAD_TOGGLE, 2},
    {RX3_PAD_BUTTON, 2}, {RX3_PAD_TOGGLE, 3}, {RX3_PAD_BUTTON, 2}, {RX3_PAD_TOGGLE_SLIDER, 3}
};

/* Its own tab pictures, in the slots module.sh stages: the pair with KEY,
   and the single tab when Key Shift is not selected. */
static const uint8_t rmx_tab_slots[RX3_TAB_STATES] = {11u, 12u, 13u, 14u, 15u, 0u, 0u, 0u};

/* Panel 2 is the right-hand performance tab, where STEMS would sit. */
static const struct rx3_pad_row rmx_row = {
    .panel_id = 2u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = RMX_WIDGETS, .widgets = rmx_widgets,
    .caption = rmx_caption, .is_on = rmx_is_on, .fire = rmx_fire,
    .slider_max = rmx_slider_max, .slider_get = rmx_slider_get, .slider_set = rmx_slider_set,
    .needs_refresh = rmx_needs_refresh, .colour = rmx_colour, .activate = rmx_activate,
    .tab_slots = rmx_tab_slots
};

#endif /* RX3_RMX_PANEL_H */
