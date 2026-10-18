/* SPDX-License-Identifier: MPL-2.0 */
/* COLOR 2: a second Sound Color FX on each channel, in series with the
 * mixer's own. The module owns the engines, their memory, the optional
 * button mapping, the input probe and its panel; the core owns the native
 * deck stream and key hooks. */
#include "../core/api/rx3_module_api.h"
#include "../core/api/rx3_fx_color.h"
static const struct rx3_services *framework;
#define log_line framework->log_line
#define log_number framework->log_number

#include "rx3_fx2_decl.h"
#include "rx3_fx2_feature.h"
#include "rx3_fx2_panel.h"

static int fx2_configured(void)
{
    const char *on = getenv("RX3_FX2");
    return on && on[0] == '1';
}

static int fx2_start(const struct rx3_services *services)
{
    framework = services;
    if (!services->input || !services->audio || !services->panels || !services->memory) return 0;
    fx2_enabled = fx2_configured();
    return fx2_feature_install();
}

static void fx2_stop(void)
{
    if (!framework || !framework->input || !framework->audio || !framework->panels ||
        !framework->memory) return;
    fx2_feature_remove();
    fx2_enabled = 0;
}

const struct rx3_module rx3_fx2_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "dual-color-fx", .configured = fx2_configured,
    .start = fx2_start, .stop = fx2_stop,
    .audio_started = fx2_started_audio, .report = fx2_report
};
