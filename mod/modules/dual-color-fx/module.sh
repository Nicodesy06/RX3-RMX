#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# COLOR 2, a second Sound Color FX per channel. The engines live in the
# performance core's shared object; this module switches them on, stages the
# COLOR 2 tab pictures, reads the optional button map and the probe switch
# from the drive, and honours the kill switch /tmp/rx3-dual-color-fx.off.

module_begin dual-color-fx dual_color_fx

DUAL_COLOR_FX_DIR="$USB/RX3_RUNTIME/dual-color-fx"
DUAL_COLOR_FX_READY=0

dual_color_fx_disable()
{
    module_export RX3_FX2 0 "COLOR 2" || :
    module_export RX3_FX2_BUTTONS "" "COLOR 2" || :
    module_export RX3_FX2_PROBE 0 "COLOR 2" || :
}

# buttons.txt: one line of six hexadecimal key codes, comma separated, in the
# mixer's order SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER. They come from
# the probe; nothing here guesses them. Lines starting with # are notes.
dual_color_fx_buttons()
{
    DUAL_COLOR_FX_CODES=
    [ -r "$DUAL_COLOR_FX_DIR/buttons.txt" ] || return 0
    while IFS= read -r dual_color_fx_line || [ -n "$dual_color_fx_line" ]; do
        case "$dual_color_fx_line" in
            ''|'#'*) continue ;;
        esac
        dual_color_fx_line=$(printf '%s' "$dual_color_fx_line" | tr -d ' \r\t')
        case "$dual_color_fx_line" in
            *[!0-9a-fA-FxX,]*) say "COLOR 2: buttons.txt ignored, unexpected characters"; return 0 ;;
        esac
        dual_color_fx_fields=$(printf '%s' "$dual_color_fx_line" | awk -F, '{print NF}')
        [ "$dual_color_fx_fields" = 6 ] || {
            say "COLOR 2: buttons.txt ignored, it needs six codes"
            return 0
        }
        DUAL_COLOR_FX_CODES=$dual_color_fx_line
        return 0
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
    stage_panel_asset dual-color-fx 16 fx2-selected || return 1
    stage_panel_asset dual-color-fx 17 fx2-none-selected || return 1
    stage_panel_asset dual-color-fx 18 fx2-beatfx-selected || return 1
    dual_color_fx_buttons
    module_export RX3_FX2_BUTTONS "$DUAL_COLOR_FX_CODES" "COLOR 2" || :
    dual_color_fx_probe=0
    [ -e "$DUAL_COLOR_FX_DIR/probe" ] && dual_color_fx_probe=1
    module_export RX3_FX2_PROBE "$dual_color_fx_probe" "COLOR 2" || :
    DUAL_COLOR_FX_READY=1
    module_export RX3_FX2 1 "COLOR 2"
    say "COLOR 2 prepared: a second Sound Color FX per channel, in series with the mixer's"
}

dual_color_fx_after_launch()
{
    [ "$DUAL_COLOR_FX_READY" = "1" ] || return 0
    say "COLOR 2: tap COLOR 2 on the touchscreen; < > choose, the name switches it on"
    if [ -n "$RX3_FX2_BUTTONS" ]; then
        say "COLOR 2: SHIFT + a Color FX button adds that effect as the second one"
    fi
    [ "$RX3_FX2_PROBE" = "1" ] && say "COLOR 2: input probe on, key codes go to the session log"
    say "COLOR 2: hardware validation pending"
}

register_prepare_hook dual_color_fx_prepare
register_after_launch_hook dual_color_fx_after_launch
