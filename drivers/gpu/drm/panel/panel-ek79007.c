#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct ek79007 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct ek79007 *panel_to_ek79007(struct drm_panel *panel)
{
	return container_of(panel, struct ek79007, panel);
}

static int ek79007_prepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	/* Enable power */
	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to enable supply: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 20000);

	/* Reset sequence */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);

	/* MIPI DSI initialization commands */
	struct mipi_dsi_device *dsi = ctx->dsi;

	// Switch to command mode
	ret = mipi_dsi_dcs_set_tear_off(dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to set tear off: %d\n", ret);
		return ret;
	}

	// Send initialization commands
	u8 init_commands[] = {
		0x80, 0xAC,
		0x81, 0xB8,
		0x82, 0x09,
		0x83, 0x78,
		0x84, 0x7F,
		0x85, 0xBB,
		0x86, 0x70,
	};

	for (int i = 0; i < ARRAY_SIZE(init_commands); i += 2) {
		ret = mipi_dsi_dcs_write(dsi, init_commands[i], &init_commands[i+1], 1);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev, "Failed to send command %02x: %d\n",
				   init_commands[i], ret);
			return ret;
		}
	}

	// Exit sleep mode
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	ctx->prepared = true;
	return 0;
}

static int ek79007_enable(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	return 0;
}

static int ek79007_disable(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	return 0;
}

static int ek79007_unprepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "Failed to enter sleep mode: %d\n", ret);

	regulator_disable(ctx->supply);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);

	ctx->prepared = false;
	return 0;
}

// H back porch:160
// H front porch:160
// H pulse width:10
// V back porch:23
// V front porch:12
// V pulse width:1
static const struct drm_display_mode ek79007_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 10,
	.htotal = 1024 + 160 + 10 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 1,
	.vtotal = 600 + 12 + 1 + 23,
	.width_mm = 154,
	.height_mm = 86,
};

static int ek79007_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ek79007_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = ek79007_mode.width_mm;
	connector->display_info.height_mm = ek79007_mode.height_mm;

	return 1;
}

static const struct drm_panel_funcs ek79007_panel_funcs = {
	.prepare = ek79007_prepare,
	.enable = ek79007_enable,
	.disable = ek79007_disable,
	.unprepare = ek79007_unprepare,
	.get_modes = ek79007_get_modes,
};

static int ek79007_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ek79007 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return PTR_ERR(ctx->supply);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return PTR_ERR(ctx->reset_gpio);

	drm_panel_init(&ctx->panel, dev, &ek79007_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ek79007_remove(struct mipi_dsi_device *dsi)
{
	struct ek79007 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ek79007_of_match[] = {
	{ .compatible = "elitek,ek79007" },
	{ }
};
MODULE_DEVICE_TABLE(of, ek79007_of_match);

static struct mipi_dsi_driver ek79007_driver = {
	.probe = ek79007_probe,
	.remove = ek79007_remove,
	.driver = {
		.name = "panel-elitek-ek79007",
		.of_match_table = ek79007_of_match,
	},
};
module_mipi_dsi_driver(ek79007_driver);

MODULE_AUTHOR("skx");
MODULE_DESCRIPTION("ELITEK EK79007 MIPI-DSI Panel Driver");
MODULE_LICENSE("GPL");
