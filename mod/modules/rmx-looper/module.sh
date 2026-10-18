#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# ROLL / LOOP FX. The engines live in the performance core's shared object;
# this module switches them on, stages the ROLL / LOOP tab pictures, and
# honours the kill switch /tmp/rx3-rmx-looper.off.

module_begin rmx-looper rmx_looper

RMX_LOOPER_READY=0

rmx_looper_disable()
{
    module_export RX3_RMX 0 "ROLL / LOOP FX" || :
}

rmx_looper_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "ROLL / LOOP FX disabled: the performance core is not selected"
        return 1
    }
    if module_disabled_by_switch rmx-looper; then
        rmx_looper_disable
        return 0
    fi
    stage_panel_asset rmx-looper 11 rmx-key-selected || return 1
    stage_panel_asset rmx-looper 12 rmx-none-selected || return 1
    stage_panel_asset rmx-looper 13 rmx-selected || return 1
    stage_panel_asset rmx-looper 14 single-rmx-none || return 1
    stage_panel_asset rmx-looper 15 single-rmx-selected || return 1
    RMX_LOOPER_READY=1
    module_export RX3_RMX 1 "ROLL / LOOP FX"
    say "ROLL / LOOP FX prepared: pads roll 1/16..8 beats, SHIFT+pads drive the loop"
}

rmx_looper_after_launch()
{
    [ "$RMX_LOOPER_READY" = "1" ] || return 0
    say "RMX: tap RMX on the touchscreen; pads 1-8 roll 1/16 1/8 1/4 1/2 1 2 4 8 beats"
    say "RMX SHIFT+pad: 1 HOLD 2 LOOP 3 OVERDUB 4 UNDO 5 CLEAR 6 SOURCE 7 LEVEL- 8 LEVEL+"
    say "RMX: hardware validation pending"
}

register_prepare_hook rmx_looper_prepare
register_after_launch_hook rmx_looper_after_launch
