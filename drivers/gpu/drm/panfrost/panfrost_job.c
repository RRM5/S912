// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#include "panfrost_device.h"
#include "panfrost_job.h"
#include "panfrost_regs.h"

#define job_write(reg, data) writel(data, ip->iomem + PAN_JOB_##reg)
#define job_read(reg) readl(ip->iomem + PAN_JOB_##reg)
#define job_slot_write(js, reg, data) writel(data, ip->iomem + PAN_JOB_SLOT0 + ((js) << 7) + PAN_JS_##reg)
#define job_slot_read(js, reg) readl(ip->iomem + PAN_JOB_SLOT0 + ((js) << 7) + PAN_JS_##reg)

int panfrost_job_init(struct panfrost_ip *ip)
{
	return 0;
}

void panfrost_job_fini(struct panfrost_ip *ip)
{

}
