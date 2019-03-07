// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#include <linux/interrupt.h>

#include "panfrost_device.h"
#include "panfrost_gpu.h"
#include "panfrost_regs.h"

#define gpu_write(reg, data) writel(data, ip->iomem + PAN_GPU_##reg)
#define gpu_read(reg) readl(ip->iomem + PAN_GPU_##reg)

static irqreturn_t panfrost_gpu_irq_handler(int irq, void *data)
{
	struct panfrost_ip *ip = data;
	struct panfrost_device *pfdev = ip->pfdev;
	u32 state = gpu_read(INT_STAT);
	u32 status = gpu_read(STATUS);
	bool done = false;

	if (!state)
		return IRQ_NONE;

	if (state & PAN_GPU_IRQ_MASK_ERROR) {
		dev_err(pfdev->dev, "gpu error irq state=%x status=%x\n",
			state, status);

		gpu_write(INT_MASK, 0);

		done = true;
	}

	gpu_write(INT_CLEAR, state);

	return IRQ_HANDLED;
}

static void panfrost_gpu_soft_reset_async(struct panfrost_ip *ip)
{
	if (ip->data.async_reset)
		return;

	gpu_write(INT_MASK, 0);
	gpu_write(INT_CLEAR, PAN_GPU_IRQ_RESET_COMPLETED);
	gpu_write(CMD, PAN_GPU_CMD_SOFT_RESET);
	ip->data.async_reset = true;
}

static int panfrost_gpu_soft_reset_async_wait(struct panfrost_ip *ip)
{
	struct panfrost_device *pfdev = ip->pfdev;
	int timeout;

	if (!ip->data.async_reset)
		return 0;

	for (timeout = 500; timeout > 0; timeout--) {
		if (gpu_read(INT_RAWSTAT) & PAN_GPU_IRQ_RESET_COMPLETED)
			break;
	}

	if (!timeout) {
		dev_err(pfdev->dev, "gpu soft reset timed out\n");
		return -ETIMEDOUT;
	}

	gpu_write(INT_CLEAR, PAN_GPU_IRQ_MASK_ALL);
	gpu_write(INT_MASK, PAN_GPU_IRQ_MASK_ALL);

	ip->data.async_reset = false;
	return 0;
}

static void panfrost_gpu_print_version(struct panfrost_ip *ip)
{
	u32 gpu_id, version, major, minor, status;
	char *name;

	gpu_id = gpu_read(ID);
	version = gpu_id >> 16;
	major = (gpu_id >> 12) & 0xf;
	minor = (gpu_id >> 4) & 0xff;
	status = gpu_id & 0xf;
	switch (version) {
	case 0x860:
		name = "mali-t860";
		break;
	case 0x750:
		name = "mali-t760";
		break;
	default:
		name = "unknown";
		break;
	}
	dev_info(ip->pfdev->dev, "%s -  %s version 0x%x major 0x%x minor 0x%x status 0x%x\n",
		 panfrost_ip_name(ip), name, version, major, minor, status);
}

int panfrost_gpu_init(struct panfrost_ip *ip)
{
	struct panfrost_device *pfdev = ip->pfdev;
	int err;

	panfrost_gpu_print_version(ip);

	ip->data.async_reset = false;
	panfrost_gpu_soft_reset_async(ip);
	err = panfrost_gpu_soft_reset_async_wait(ip);
	if (err)
		return err;

	err = devm_request_irq(pfdev->dev, ip->irq, panfrost_gpu_irq_handler,
			       IRQF_SHARED, panfrost_ip_name(ip), ip);
	if (err) {
		dev_err(pfdev->dev, "gpu %s failed to request irq\n",
			panfrost_ip_name(ip));
		return err;
	}

	return 0;
}

void panfrost_gpu_fini(struct panfrost_ip *ip)
{

}
