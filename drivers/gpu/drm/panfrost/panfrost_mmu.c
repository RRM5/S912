// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#include "panfrost_device.h"
#include "panfrost_mmu.h"
#include "panfrost_regs.h"

#define mmu_write(reg, data) writel(data, ip->iomem + PAN_MMU_##reg)
#define mmu_read(reg) readl(ip->iomem + PAN_MMU_##reg)

static irqreturn_t panfrost_mmu_irq_handler(int irq, void *data)
{
	struct panfrost_ip *ip = data;
	struct panfrost_device *pfdev = ip->pfdev;
	u32 status = mmu_read(INT_STAT);

	if (!status)
		return IRQ_NONE;

	return IRQ_HANDLED;
};

int panfrost_mmu_init(struct panfrost_ip *ip)
{
	struct panfrost_device *pfdev = ip->pfdev;
	int err;

	err = devm_request_irq(pfdev->dev, ip->irq, panfrost_mmu_irq_handler,
			       IRQF_SHARED, panfrost_ip_name(ip), ip);

	if (err) {
		dev_err(pfdev->dev, "mmu %s failed to request irq\n", panfrost_ip_name(ip));
		return err;
	}
	return 0;
}

void panfrost_mmu_fini(struct panfrost_ip *ip)
{

}
