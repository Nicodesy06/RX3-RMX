/* SPDX-License-Identifier: MPL-2.0
 * The RMX X-PAD on the touchscreen: one row across both halves, laid out as
 * the RMX-1000's X-PAD FX section.
 *
 *   [ROLL 1][KICK][SNARE][CLAP][HI HAT] [1/8][1/4][1/2][1/1][2/1] [PITCH 0][VOL 80]
 *    the X-PAD FX buttons                the X-PAD strip            knobs
 *
 * Tap a button to choose what the strip plays; a sound also plays at once.
 * Touch the strip and slide along it: ROLL rolls the source at the section's
 * length, a sound repeats at it. Lift to stop, unless HOLD is on. SHIFT held
 * as the finger lands plays the strip an octave finer (1/16 to 1/1). PITCH:
 * hold and drag, tap for 0. VOL: hold and drag, tap to mute.
 */
#ifndef RX3_RMX_PANEL_H
#define RX3_RMX_PANEL_H

#define RMX_W_PICK 0u                        /* five X-PAD FX buttons */
#define RMX_W_STRIP RMX_PICKS                /* five strip sections */
#define RMX_W_PITCH (RMX_W_STRIP + RMX_STRIP_SECTIONS)
#define RMX_W_VOLUME (RMX_W_PITCH + 1u)
#define RMX_WIDGETS (RMX_W_VOLUME + 1u)

static const uint16_t rmx_text_picks[RMX_PICKS][7] = {
    {'R','O','L','L',' ','1',0}, {'K','I','C','K',0}, {'S','N','A','R','E',0},
    {'C','L','A','P',0}, {'H','I',' ','H','A','T',0}
};
static const uint16_t rmx_text_mute[] = {'M','U','T','E',0};

static uint16_t *rmx_number(uint16_t *at, const char *prefix, int value, int sign)
{
    while (*prefix) *at++ = (uint16_t)*prefix++;
    if (sign && value > 0) *at++ = '+';
    if (value < 0) { *at++ = '-'; value = -value; }
    if (value >= 100) *at++ = (uint16_t)('0' + value / 100);
    if (value >= 10) *at++ = (uint16_t)('0' + (value / 10) % 10);
    *at++ = (uint16_t)('0' + value % 10);
    *at = 0;
    return at;
}

static const uint16_t *rmx_caption(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    static uint16_t roll[7], pitch[12], volume[8];
    if (widget == RMX_W_PICK) {
        /* ROLL names its source: 1, 2 or M. */
        static const uint16_t marks[3] = {'1', '2', 'M'};
        memcpy(roll, rmx_text_picks[0], sizeof(roll));
        roll[5] = marks[__atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST) % 3u];
        return roll;
    }
    if (widget < RMX_W_STRIP) return rmx_text_picks[widget];
    if (widget < RMX_W_PITCH) {
        unsigned int fine = __atomic_load_n(&rmx_section, __ATOMIC_SEQ_CST) >= 0 &&
                            __atomic_load_n(&rmx_section_fine, __ATOMIC_SEQ_CST);
        return rmx_strip_text[fine][widget - RMX_W_STRIP];
    }
    if (widget == RMX_W_PITCH) {
        rmx_number(pitch, "PITCH ", __atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST), 1);
        return pitch;
    }
    if (__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST)) return rmx_text_mute;
    rmx_number(volume, "VOL ", (int)__atomic_load_n(&rmx_volume, __ATOMIC_SEQ_CST), 0);
    return volume;
}

static int rmx_is_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    if (widget < RMX_W_STRIP) return __atomic_load_n(&rmx_pick, __ATOMIC_SEQ_CST) == widget;
    if (widget < RMX_W_PITCH)
        return __atomic_load_n(&rmx_section, __ATOMIC_SEQ_CST) == (int)(widget - RMX_W_STRIP);
    if (widget == RMX_W_PITCH) return __atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST) != 0;
    return !__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST);
}

static void rmx_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    if (widget < RMX_W_STRIP) rmx_pick_button(widget);
    else if (widget == RMX_W_PITCH) rmx_set_pitch(0);
    else if (widget == RMX_W_VOLUME) {
        __atomic_store_n(&rmx_muted, !__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
        rmx_publish_volume();
    }
}

/* The strip. A landing starts the effect, a slide to another section moves
   it there, the release stops it unless HOLD keeps it. */
static void rmx_strip(unsigned int deck, unsigned int widget, unsigned int touching)
{
    (void)deck;
    if (widget < RMX_W_STRIP || widget >= RMX_W_PITCH) return;
    int section = (int)(widget - RMX_W_STRIP);
    if (!touching) {
        __atomic_store_n(&rmx_strip_touching, 0u, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST)) rmx_strip_stop();
        return;
    }
    int first = !__atomic_load_n(&rmx_strip_touching, __ATOMIC_SEQ_CST);
    if (first) {
        __atomic_store_n(&rmx_strip_touching, 1u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&rmx_section_fine, framework->input->shift_held(RX3_NO_DECK) ? 1u : 0u,
                         __ATOMIC_SEQ_CST);
    }
    /* A new landing while HOLD keeps an effect takes it over from there. */
    rmx_strip_play(section, first && !__atomic_load_n(&rmx_strip_effect, __ATOMIC_SEQ_CST));
}

/* PITCH -12..+12 as 0..24; VOL 0..100. */
static unsigned int rmx_slider_max(unsigned int deck, unsigned int widget)
{ (void)deck; return widget == RMX_W_PITCH ? 24u : 100u; }

static unsigned int rmx_slider_get(unsigned int deck, unsigned int widget)
{
    (void)deck;
    if (widget == RMX_W_PITCH) return (unsigned int)(__atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST) + 12);
    return __atomic_load_n(&rmx_volume, __ATOMIC_SEQ_CST);
}

static void rmx_slider_set(unsigned int deck, unsigned int widget, unsigned int value,
                           unsigned int committed)
{
    (void)deck; (void)committed;
    if (widget == RMX_W_PITCH) { rmx_set_pitch((int)(value > 24u ? 24u : value) - 12); return; }
    __atomic_store_n(&rmx_volume, value > 100u ? 100u : value, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_muted, 0u, __ATOMIC_SEQ_CST);
    rmx_publish_volume();
}

/* Accents: a sound or ROLL whose OVERDUB track holds something is red, and
   blue while it sounds; OVERDUB on standby or recording shows on ROLL. */
static uint16_t rmx_colour(unsigned int deck, unsigned int widget)
{
    (void)deck;
    if (widget >= RMX_W_STRIP) return 0u;
    unsigned int track = widget == RMX_W_PICK ? FX_XPAD_ROLL_TRACK : widget - 1u;
    if (widget != RMX_W_PICK && (rmx_xpad.shown_playing & (1u << track))) return 0x041fu;
    if (rmx_xpad.shown_tracks & (1u << track)) return 0xf800u;
    if (widget == RMX_W_PICK && rmx_xpad.shown_state == FX_PATTERN_STANDBY) return 0xfc00u;
    return 0u;
}

static int rmx_needs_refresh(void)
{
    static unsigned int seen[10];
    unsigned int now[10] = {
        rmx_xpad.shown_state, rmx_xpad.shown_tracks, rmx_xpad.shown_playing,
        __atomic_load_n(&rmx_source, __ATOMIC_SEQ_CST), __atomic_load_n(&rmx_pick, __ATOMIC_SEQ_CST),
        (unsigned int)__atomic_load_n(&rmx_section, __ATOMIC_SEQ_CST),
        __atomic_load_n(&rmx_section_fine, __ATOMIC_SEQ_CST),
        (unsigned int)__atomic_load_n(&rmx_semitones, __ATOMIC_SEQ_CST),
        __atomic_load_n(&rmx_volume, __ATOMIC_SEQ_CST), __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST)
    };
    int changed = memcmp(seen, now, sizeof(now)) != 0;
    memcpy(seen, now, sizeof(now));
    return changed;
}

static const struct rx3_pad_widget rmx_widgets[RMX_WIDGETS] = {
    {RX3_PAD_TOGGLE, 2}, {RX3_PAD_TOGGLE, 2}, {RX3_PAD_TOGGLE, 2}, {RX3_PAD_TOGGLE, 2}, {RX3_PAD_TOGGLE, 2},
    {RX3_PAD_STRIP, 2}, {RX3_PAD_STRIP, 2}, {RX3_PAD_STRIP, 2}, {RX3_PAD_STRIP, 2}, {RX3_PAD_STRIP, 2},
    {RX3_PAD_TOGGLE_SLIDER, 3}, {RX3_PAD_TOGGLE_SLIDER, 3}
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
    .tab_slots = rmx_tab_slots, .strip = rmx_strip
};

#endif /* RX3_RMX_PANEL_H */
