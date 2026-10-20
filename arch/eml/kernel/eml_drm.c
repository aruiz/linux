// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include <asm/eml_efi.h>

struct eml_drm {
	struct drm_device drm;
	struct drm_connector connector;
	struct drm_simple_display_pipe pipe;
	void *fb_vaddr;
	unsigned long fb_size;
	u32 width;
	u32 height;
	u32 stride;
};

static inline struct eml_drm *to_eml(struct drm_device *dev)
{
	return container_of(dev, struct eml_drm, drm);
}

static int eml_connector_get_modes(struct drm_connector *connector)
{
	struct eml_drm *eml =
		container_of(connector, struct eml_drm, connector);
	struct drm_display_mode *mode;

	mode = drm_mode_create(&eml->drm);
	if (!mode)
		return 0;

	mode->hdisplay = eml->width;
	mode->hsync_start = eml->width;
	mode->hsync_end = eml->width;
	mode->htotal = eml->width;
	mode->vdisplay = eml->height;
	mode->vsync_start = eml->height;
	mode->vsync_end = eml->height;
	mode->vtotal = eml->height;
	mode->clock = mode->htotal * mode->vtotal * 60 / 1000;
	if (!mode->clock)
		mode->clock = 1;
	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs eml_connector_helper = {
	.get_modes = eml_connector_get_modes,
};

static const struct drm_connector_funcs eml_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void eml_pipe_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
}

static void eml_pipe_disable(struct drm_simple_display_pipe *pipe)
{
}

static void eml_pipe_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct eml_drm *eml = to_eml(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *dma_obj;
	unsigned int y;
	void *src;

	if (!fb)
		return;

	dma_obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!dma_obj || !dma_obj->vaddr)
		return;

	src = dma_obj->vaddr;
	for (y = 0; y < fb->height && y < eml->height; y++)
		memcpy(eml->fb_vaddr + y * eml->stride,
		       src + y * fb->pitches[0],
		       min(fb->pitches[0], eml->stride));
}

static const struct drm_simple_display_pipe_funcs eml_pipe_funcs = {
	.enable = eml_pipe_enable,
	.disable = eml_pipe_disable,
	.update = eml_pipe_update,
};

DEFINE_DRM_GEM_DMA_FOPS(eml_drm_fops);

static const struct drm_driver eml_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.name = "eml-drm",
	.desc = "EML EFI GOP DRM",
	.major = 1,
	.minor = 0,
	.fops = &eml_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS,
};

static const struct drm_mode_config_funcs eml_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 eml_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
};

static int eml_drm_probe(struct platform_device *pdev)
{
	efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	struct eml_gop_mode_info *info;
	struct eml_gop *gop;
	struct eml_drm *eml;
	struct drm_device *drm;
	efi_status_t status;
	int ret;

	status = eml_systab->boot_services->locate_protocol(&gop_guid, NULL,
							    (void **)&gop);
	if (status != EFI_SUCCESS) {
		dev_info(&pdev->dev, "EFI GOP not available\n");
		return -ENODEV;
	}

	if (!gop->mode || !gop->mode->info || !gop->mode->frame_buffer_base) {
		dev_err(&pdev->dev, "GOP has no usable mode\n");
		return -ENODEV;
	}

	info = gop->mode->info;

	eml = devm_drm_dev_alloc(&pdev->dev, &eml_drm_driver, struct eml_drm,
				 drm);
	if (IS_ERR(eml))
		return PTR_ERR(eml);

	drm = &eml->drm;
	platform_set_drvdata(pdev, eml);

	eml->width = info->horizontal_resolution;
	eml->height = info->vertical_resolution;
	eml->stride = info->pixels_per_scan_line * 4;
	eml->fb_size = gop->mode->frame_buffer_size;
	eml->fb_vaddr = (void *)(unsigned long)gop->mode->frame_buffer_base;

	dev_info(&pdev->dev, "GOP %ux%u stride=%u fb=0x%llx (%lu KB)\n",
		 eml->width, eml->height, eml->stride,
		 (unsigned long long)gop->mode->frame_buffer_base,
		 eml->fb_size / 1024);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = eml->width;
	drm->mode_config.max_width = eml->width;
	drm->mode_config.min_height = eml->height;
	drm->mode_config.max_height = eml->height;
	drm->mode_config.preferred_depth = 32;
	drm->mode_config.funcs = &eml_mode_config_funcs;

	ret = drm_connector_init(drm, &eml->connector, &eml_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	drm_connector_helper_add(&eml->connector, &eml_connector_helper);
	eml->connector.status = connector_status_connected;

	ret = drm_simple_display_pipe_init(drm, &eml->pipe, &eml_pipe_funcs,
					   eml_formats, ARRAY_SIZE(eml_formats),
					   NULL, &eml->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "DRM device registered\n");
	return 0;
}

static void eml_drm_remove(struct platform_device *pdev)
{
	struct eml_drm *eml = platform_get_drvdata(pdev);

	drm_dev_unregister(&eml->drm);
}

static struct platform_driver eml_drm_platdrv = {
	.probe = eml_drm_probe,
	.remove = eml_drm_remove,
	.driver = {
		.name = "eml-drm",
	},
};

static int __init eml_drm_init(void)
{
	struct platform_device *pdev;
	int ret;

	ret = platform_driver_register(&eml_drm_platdrv);
	if (ret)
		return ret;

	pdev = platform_device_register_simple("eml-drm", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&eml_drm_platdrv);
		return PTR_ERR(pdev);
	}

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	return 0;
}
late_initcall(eml_drm_init);
