#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# ISOLATOR. The isolator lives in the performance core's shared object; this
# module switches it on, stages its tab pictures and honours the kill switch
# /tmp/rx3-isolator.off.

module_begin isolator isolator

ISOLATOR_READY=0

isolator_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "ISOLATOR disabled: the performance core is not selected"
        return 1
    }
    if module_disabled_by_switch isolator; then
        module_export RX3_ISOLATOR 0 "ISOLATOR" || :
        return 0
    fi
    stage_panel_asset isolator 16 iso-selected || return 1
    stage_panel_asset isolator 17 iso-none-selected || return 1
    stage_panel_asset isolator 18 iso-beatfx-selected || return 1
    ISOLATOR_READY=1
    module_export RX3_ISOLATOR 1 "ISOLATOR"
    say "ISOLATOR prepared: LOW MID HIGH per deck, and the bands the effects act on"
}

isolator_after_launch()
{
    [ "$ISOLATOR_READY" = "1" ] || return 0
    say "ISOLATOR: tap ISOLATOR on the touchscreen; tap a band to kill it, hold and drag to set it"
    say "ISOLATOR: hardware validation pending"
}

register_prepare_hook isolator_prepare
register_after_launch_hook isolator_after_launch
