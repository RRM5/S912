// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#include <linux/clk.h>
#include <linux/regulator/consumer.h>

#include "panfrost_device.h"
#include "panfrost_gpu.h"
#include "panfrost_job.h"
#include "panfrost_mmu.h"

struct panfrost_ip_desc {
	char *name;
	char *irq_name;
	int offset;
	int (*init)(struct panfrost_ip *);
	void (*fini)(struct panfrost_ip *);
};

#define PANFROST_IP_DESC(ipname, off, func, irq) \
	[panfrost_ip_##ipname] = { \
		.name = #ipname, \
		.irq_name = irq, \
		.offset = off, \
		.init = panfrost_##func##_init, \
		.fini = panfrost_##func##_fini, \
	}

static struct panfrost_ip_desc panfrost_ip_desc[panfrost_ip_num] = {
	/* PANFROST_IP_DESC(ipname, off, func, irq) */
	PANFROST_IP_DESC(gpu, 0x0000, gpu, "gpu"),
	PANFROST_IP_DESC(job, 0x1000, job, "job"),
	PANFROST_IP_DESC(mmu, 0x2000, mmu, "mmu"),
};

const char *panfrost_ip_name(struct panfrost_ip *ip)
{
	return panfrost_ip_desc[ip->id].name;
}

static int panfrost_clk_init(struct panfrost_device *pfdev)
{
	int err;
	unsigned long rate;

	pfdev->clock = devm_clk_get(pfdev->dev, NULL);
	if (IS_ERR(pfdev->clock)) {
		dev_err(pfdev->dev, "get clock failed %ld\n", PTR_ERR(pfdev->clock));
		return PTR_ERR(pfdev->clock);
	}

	rate = clk_get_rate(pfdev->clock);
	dev_info(pfdev->dev, "clock rate = %lu\n", rate);

	if ((err = clk_prepare_enable(pfdev->clock)))
		return err;

	return 0;
}

static void panfrost_clk_fini(struct panfrost_device *pfdev)
{
	clk_disable_unprepare(pfdev->clock);
}

static int panfrost_regulator_init(struct panfrost_device *pfdev)
{
	int ret;
	pfdev->regulator = devm_regulator_get_optional(pfdev->dev, "mali");
	if (IS_ERR(pfdev->regulator)) {
		ret = PTR_ERR(pfdev->regulator);
		pfdev->regulator = NULL;
		if (ret == -ENODEV)
			return 0;
		dev_err(pfdev->dev, "failed to get regulator: %ld\n", PTR_ERR(pfdev->regulator));
		return ret;
	}

	ret = regulator_enable(pfdev->regulator);
	if (ret < 0) {
		dev_err(pfdev->dev, "failed to enable regulator: %d\n", ret);
		return ret;
	}

	return 0;
}

static void panfrost_regulator_fini(struct panfrost_device *pfdev)
{
	if (pfdev->regulator)
		regulator_disable(pfdev->regulator);
}

static int panfrost_init_ip(struct panfrost_device *pfdev, int index)
{
	struct panfrost_ip_desc *desc = panfrost_ip_desc + index;
	struct panfrost_ip *ip = pfdev->ip + index;
	int offset = desc->offset;
	int err;

	if (offset < 0)
		return 0;

	ip->pfdev = pfdev;
	ip->id = index;
	ip->iomem = pfdev->iomem + offset;
	if (desc->irq_name) {
		err = platform_get_irq_byname(pfdev->pdev, desc->irq_name);
		if (err < 0)
			return err;
		ip->irq = err;
	}

	err = desc->init(ip);
	if (!err) {
		ip->present = true;
		return 0;
	}

	return err;
}

static void panfrost_fini_ip(struct panfrost_device *pfdev, int index)
{
	struct panfrost_ip_desc *desc = panfrost_ip_desc + index;
	struct panfrost_ip *ip = pfdev->ip + index;

	if (ip->present)
		desc->fini(ip);
}

int panfrost_device_init(struct panfrost_device *pfdev)
{
	int err, i;
	struct resource *res;

	err = panfrost_clk_init(pfdev);
	if (err) {
		dev_err(pfdev->dev, "clk init failed %d\n", err);
		return err;
	}

	if ((err = panfrost_regulator_init(pfdev))) {
		dev_err(pfdev->dev, "regulator init failed %d\n", err);
		goto err_out0;
	}

	res = platform_get_resource(pfdev->pdev, IORESOURCE_MEM, 0);
	pfdev->iomem = devm_ioremap_resource(pfdev->dev, res);
	if (IS_ERR(pfdev->iomem)) {
		dev_err(pfdev->dev, "failed to ioremap iomem\n");
		err = PTR_ERR(pfdev->iomem);
		goto err_out1;
	}

	for (i = 0; i < panfrost_ip_num; i++) {
		err = panfrost_init_ip(pfdev, i);
		if (err)
			goto err_out2;
	}

	return 0;
err_out2:
	while (--i >= 0)
		panfrost_fini_ip(pfdev, i);
err_out1:
	panfrost_regulator_fini(pfdev);
err_out0:
	panfrost_clk_fini(pfdev);
	return err;
}

void panfrost_device_fini(struct panfrost_device *pfdev)
{
	int i;

	for (i = panfrost_ip_num -1; i >= 0; i--)
		panfrost_fini_ip(pfdev, i);

	panfrost_regulator_fini(pfdev);

	panfrost_clk_fini(pfdev);
}
