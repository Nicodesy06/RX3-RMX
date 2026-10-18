#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# COLOR 2, a second Sound Color FX per channel, played by holding two of the
# mixer's Color FX buttons. The engines live in the performance core's shared
# object; this module switches them on, reads the button map and the probe
# switch from the drive, and honours the kill switch /tmp/rx3-dual-color-fx.off.

module_begin dual-color-fx dual_color_fx

DUAL_COLOR_FX_DIR="$USB/RX3_RUNTIME/dual-color-fx"
DUAL_COLOR_FX_READY=0

dual_color_fx_disable()
{
    module_export RX3_FX2 0 "COLOR 2" || :
    module_export RX3_FX2_BUTTONS "" "COLOR 2" || :
    module_export RX3_FX2_LEDS "" "COLOR 2" || :
    module_export RX3_FX2_PROBE 0 "COLOR 2" || :
}

# One list of six hexadecimal values, comma separated, in the mixer's order
# SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER, or empty if it is malformed.
dual_color_fx_six()
{
    case "$1" in
        *[!0-9a-fA-FxX,]*) return 1 ;;
    esac
    [ "$(printf '%s' "$1" | awk -F, '{print NF}')" = 6 ] || return 1
    printf '%s' "$1"
}

# buttons.txt, from the probe: a line of the six button codes, optionally a
# line "leds=" with their six LED IDs and a line "depth=" with COLOR 2's
# depth (0..200, 100 is centre). Lines starting with # are notes. Nothing
# here is executed or guessed.
dual_color_fx_read()
{
    DUAL_COLOR_FX_CODES=
    DUAL_COLOR_FX_LED_IDS=
    DUAL_COLOR_FX_DEPTH=
    [ -r "$DUAL_COLOR_FX_DIR/buttons.txt" ] || return 0
    while IFS= read -r dual_color_fx_line || [ -n "$dual_color_fx_line" ]; do
        dual_color_fx_line=$(printf '%s' "$dual_color_fx_line" | tr -d ' \r\t')
        case "$dual_color_fx_line" in
            ''|'#'*) ;;
            leds=*)
                DUAL_COLOR_FX_LED_IDS=$(dual_color_fx_six "${dual_color_fx_line#leds=}") ||
                    say "COLOR 2: the leds= line is ignored, it needs six values"
                ;;
            depth=*)
                case "${dual_color_fx_line#depth=}" in
                    ''|*[!0-9]*) say "COLOR 2: the depth= line is ignored" ;;
                    *) [ "${dual_color_fx_line#depth=}" -le 200 ] &&
                           DUAL_COLOR_FX_DEPTH=${dual_color_fx_line#depth=} ;;
                esac
                ;;
            *)
                DUAL_COLOR_FX_CODES=$(dual_color_fx_six "$dual_color_fx_line") ||
                    say "COLOR 2: buttons.txt ignored, it needs six button codes"
                ;;
        esac
    done < "$DUAL_COLOR_FX_DIR/buttons.txt"
}

dual_color_fx_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "COLOR 2 disabled: the performance core is not selected"
        return 1
    }
    if module_disabled_by_switch dual-color-fx; then
        dual_color_fx_disable
        return 0
    fi
    dual_color_fx_read
    module_export RX3_FX2_BUTTONS "$DUAL_COLOR_FX_CODES" "COLOR 2" || :
    module_export RX3_FX2_LEDS "$DUAL_COLOR_FX_LED_IDS" "COLOR 2" || :
    module_export RX3_FX2_DEPTH "$DUAL_COLOR_FX_DEPTH" "COLOR 2" || :
    dual_color_fx_probe=0
    [ -e "$DUAL_COLOR_FX_DIR/probe" ] && dual_color_fx_probe=1
    module_export RX3_FX2_PROBE "$dual_color_fx_probe" "COLOR 2" || :
    DUAL_COLOR_FX_READY=1
    module_export RX3_FX2 1 "COLOR 2"
    say "COLOR 2 prepared: hold a Color FX button and press a second to add it"
}

dual_color_fx_after_launch()
{
    [ "$DUAL_COLOR_FX_READY" = "1" ] || return 0
    if [ -z "$RX3_FX2_BUTTONS" ]; then
        say "COLOR 2: no button codes yet; put the probe's codes in RX3_RUNTIME/dual-color-fx/buttons.txt"
    fi
    [ "$RX3_FX2_PROBE" = "1" ] && say "COLOR 2: input probe on, key codes and LED IDs go to the session log"
    say "COLOR 2: hardware validation pending"
}

register_prepare_hook dual_color_fx_prepare
register_after_launch_hook dual_color_fx_after_launch
