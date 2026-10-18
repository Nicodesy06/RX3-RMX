/* SPDX-License-Identifier: MPL-2.0 */
/* RMX X-PAD: the RMX-1000's X-PAD FX section on the touchscreen and pads,
 * ROLL, KICK, SNARE, CLAP, HI HAT, the touch strip, HOLD, OVERDUB, PITCH and
 * VOLUME. The module owns the engines, their buffers, the pad policy while
 * its panel is up, its lights and its panel; the core owns the native audio,
 * pad and LED hooks and calls the module through typed services. */
#include "../core/api/rx3_module_api.h"
#include "../core/api/rx3_fx_xpad.h"
#include "../core/api/rx3_fx_bands.h"
static const struct rx3_services *framework;
#define log_line framework->log_line
#define log_number framework->log_number

#include "rx3_rmx_decl.h"
#include "rx3_rmx_feature.h"
#include "rx3_rmx_panel.h"

static int rmx_configured(void)
{
    const char *on = getenv("RX3_RMX");
    return on && on[0] == '1';
}

static int rmx_start(const struct rx3_services *services)
{
    framework = services;
    if (!services->input || !services->audio || !services->panels || !services->memory) return 0;
    rmx_enabled = rmx_configured();
    return rmx_feature_install();
}

static void rmx_stop(void)
{
    if (!framework || !framework->input || !framework->audio || !framework->panels ||
        !framework->memory) return;
    rmx_feature_remove();
    rmx_enabled = 0;
}

const struct rx3_module rx3_rmx_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "rmx-looper", .configured = rmx_configured,
    .start = rmx_start, .stop = rmx_stop,
    .audio_started = rmx_started_audio, .report = rmx_report
};
