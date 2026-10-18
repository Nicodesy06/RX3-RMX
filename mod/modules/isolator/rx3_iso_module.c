/* SPDX-License-Identifier: MPL-2.0 */
/* ISOLATOR: a three-band isolator per deck, and the LOW / MID / HI choice of
 * which bands the performance effects act on. The module owns the bands, the
 * levels and its panel; the core owns the native deck stream and calls the
 * module through typed services. */
#include "../core/api/rx3_module_api.h"
#include "../core/api/rx3_fx_bands.h"
static const struct rx3_services *framework;
#define log_line framework->log_line

#include "rx3_iso_feature.h"
#include "rx3_iso_panel.h"

static int iso_configured(void)
{
    const char *on = getenv("RX3_ISOLATOR");
    return on && on[0] == '1';
}

static int iso_start(const struct rx3_services *services)
{
    framework = services;
    if (!services->audio || !services->panels) return 0;
    iso_enabled = iso_configured();
    return iso_feature_install();
}

static void iso_stop(void)
{
    if (!framework || !framework->audio || !framework->panels) return;
    iso_feature_remove();
    iso_enabled = 0;
}

const struct rx3_module rx3_iso_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "isolator", .configured = iso_configured,
    .start = iso_start, .stop = iso_stop, .audio_started = iso_started_audio
};
