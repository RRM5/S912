// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <drm/panfrost_drm.h>

#include "panfrost_device.h"

static int panfrost_ioctl_info(struct drm_device *ddev, void *data, struct drm_file *file)
{
	struct drm_panfrost_info *info = data;
	struct panfrost_device *pfdev = ddev->dev_private;

	switch (pfdev->id) {
	case panfrost_gpu_malit760:
		info->gpu_id = PANFROST_INFO_GPU_MALI_T760;
		break;
	case panfrost_gpu_malit860:
		info->gpu_id = PANFROST_INFO_GPU_MALI_T860;
		break;
	default:
		return -ENODEV;
	}
	return 0;
}

static const struct drm_ioctl_desc panfrost_drm_driver_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANFROST_INFO, panfrost_ioctl_info, DRM_AUTH|DRM_RENDER_ALLOW),
};

static const struct file_operations panfrost_drm_driver_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
};

static struct drm_driver panfrost_drm_driver = {
	.driver_features	= DRIVER_RENDER,
	.ioctls			= panfrost_drm_driver_ioctls,
	.num_ioctls		= ARRAY_SIZE(panfrost_drm_driver_ioctls),
	.fops			= &panfrost_drm_driver_fops,
	.name			= "panfrost",
	.desc			= "panfrost DRM",
	.date			= "20180908",
	.major			= 1,
	.minor			= 0,
};

static int panfrost_pdev_probe(struct platform_device *pdev)
{
	struct panfrost_device *pfdev;
	struct drm_device *ddev;
	int err;

	pfdev = devm_kzalloc(&pdev->dev, sizeof(*pfdev), GFP_KERNEL);
	if (!pfdev)
		return -ENOMEM;

	pfdev->pdev = pdev;
	pfdev->dev = &pdev->dev;

	platform_set_drvdata(pdev, pfdev);

	/* Allocate and initialze the DRM device. */
	ddev = drm_dev_alloc(&panfrost_drm_driver, &pdev->dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	ddev->dev_private = pfdev;
	pfdev->ddev = ddev;

	err = panfrost_device_init(pfdev);
	if (err) {
		dev_err(&pdev->dev, "Fatal error during GPU init\n");
		goto err_out0;
	}

	/*
	 * Register the DRM device with the core and the connectors with
	 * sysfs
	 */
	err = drm_dev_register(ddev, 0);
	if (err < 0)
		goto err_out1;

	return 0;

err_out1:
	panfrost_device_fini(pfdev);
err_out0:
	drm_dev_unref(ddev);
	return err;
}

static int panfrost_pdev_remove(struct platform_device *pdev)
{
	struct panfrost_device *pfdev = platform_get_drvdata(pdev);
	struct drm_device *ddev = pfdev->ddev;

	drm_dev_unregister(ddev);
	panfrost_device_fini(pfdev);
	drm_dev_unref(ddev);
	return 0;
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "arm,mali-t760" },
	{ .compatible = "arm,mali-t860" },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static struct platform_driver panfrost_platform_driver = {
	.probe		= panfrost_pdev_probe,
	.remove		= panfrost_pdev_remove,
	.driver		= {
		.name	= "panfrost",
		.of_match_table = dt_match,
	},
};

static int __init panfrost_init(void)
{
	int ret;

	ret = platform_driver_register(&panfrost_platform_driver);

	return ret;
}
module_init(panfrost_init);

static void __exit panfrost_exit(void)
{
	platform_driver_unregister(&panfrost_platform_driver);
}
module_exit(panfrost_exit);

MODULE_AUTHOR("Panfrost Project Developers");
MODULE_DESCRIPTION("Panfrost DRM Driver");
MODULE_LICENSE("GPL v2");
