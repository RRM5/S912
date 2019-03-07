// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#ifndef __PANFROST_JOB_H__
#define __PANFROST_JOB_H__

struct panfrost_ip;
struct panfrost_device;

int panfrost_job_init(struct panfrost_ip *ip);
void panfrost_job_fini(struct panfrost_ip *ip);

#endif
