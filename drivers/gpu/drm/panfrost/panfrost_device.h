// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#ifndef __PANFROST_DEVICE_H__
#define __PANFROST_DEVICE_H__

#include <drm/drm_device.h>
#include <drm/drmP.h>

enum panfrost_gpu_id {
	panfrost_gpu_malit760 = 0,
	panfrost_gpu_malit860,
	panfrost_gpu_num,
};

enum panfrost_ip_id {
	panfrost_ip_gpu,
	panfrost_ip_job,
	panfrost_ip_mmu,
	panfrost_ip_num,
};

struct panfrost_device;

struct panfrost_ip {
	struct panfrost_device *pfdev;
	enum panfrost_ip_id id;
	bool present;

	void __iomem *iomem;
	int irq;

	union {
		/* gpu */
		bool async_reset;
	} data;
};

struct panfrost_device {
	struct device *dev;
	struct drm_device *ddev;
	struct platform_device *pdev;

	enum panfrost_gpu_id id;

	void __iomem *iomem;
	struct clk *clock;
	struct regulator *regulator;

	struct panfrost_ip ip[panfrost_ip_num];
};

int panfrost_device_init(struct panfrost_device *pfdev);
void panfrost_device_fini(struct panfrost_device *pfdev);

const char *panfrost_ip_name(struct panfrost_ip *ip);

#endif
