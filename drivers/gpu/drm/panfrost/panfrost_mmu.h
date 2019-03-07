// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#ifndef __PANFROST_MMU_H__
#define __PANFROST_MMU_H__

struct panfrost_ip;

int panfrost_mmu_init(struct panfrost_ip *ip);
void panfrost_mmu_fini(struct panfrost_ip *ip);

#endif
