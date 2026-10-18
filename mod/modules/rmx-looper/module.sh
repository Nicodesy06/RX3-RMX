#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# RMX X-PAD. The engines live in the performance core's shared object; this
# module switches them on, stages the RMX tab pictures, points the core at
# the optional sounds on the drive, and honours /tmp/rx3-rmx-looper.off.

module_begin rmx-looper rmx_looper

RMX_LOOPER_DIR="$USB/RX3_RUNTIME/rmx-looper"
RMX_LOOPER_READY=0

rmx_looper_disable()
{
    module_export RX3_RMX 0 "RMX X-PAD" || :
}

rmx_looper_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "RMX X-PAD disabled: the performance core is not selected"
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
    # kick.wav, snare.wav, clap.wav, hat.wav replace the built-in sounds.
    # The path is only handed over; the core reads and checks the files.
    rmx_looper_sounds=
    for rmx_looper_name in kick snare clap hat; do
        [ -f "$RMX_LOOPER_DIR/$rmx_looper_name.wav" ] && rmx_looper_sounds=$RMX_LOOPER_DIR
    done
    module_export RX3_RMX_SOUNDS "$rmx_looper_sounds" "RMX X-PAD" || :
    RMX_LOOPER_READY=1
    module_export RX3_RMX 1 "RMX X-PAD"
    say "RMX X-PAD prepared${rmx_looper_sounds:+, sounds from the drive}"
}

rmx_looper_after_launch()
{
    [ "$RMX_LOOPER_READY" = "1" ] || return 0
    say "RMX: tap RMX on the touchscreen; choose ROLL or a sound, then slide on 1/8 .. 2/1"
    say "RMX pads: 1 KICK 2 SNARE 3 CLAP 4 HI HAT 5 ROLL 6 HOLD 7 OVERDUB 8 SOURCE"
    say "RMX: hardware validation pending"
}

register_prepare_hook rmx_looper_prepare
register_after_launch_hook rmx_looper_after_launch
