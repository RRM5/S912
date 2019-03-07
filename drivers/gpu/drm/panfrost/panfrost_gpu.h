// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#ifndef __PANFROST_GPU_H__
#define __PANFROST_GPU_H__

struct panfrost_ip;
struct panfrost_device;

int panfrost_gpu_init(struct panfrost_ip *ip);
void panfrost_gpu_fini(struct panfrost_ip *ip);

int panfrost_gpu_pipe_init(struct panfrost_device *pfdev);
void panfrost_gpu_pipe_fini(struct panfrost_device *pfdev);

#endif
