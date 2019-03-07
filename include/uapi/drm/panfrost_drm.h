// SPDX-License-Identifier:	GPL-2.0
/* Copyright 2018 Panfrost Team */

#ifndef __PANFROST_DRM_H__
#define __PANFROST_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define PANFROST_INFO_GPU_MALI_T760 0x00
#define PANFROST_INFO_GPU_MALI_T860 0x01

struct drm_panfrost_info {
	__u32 gpu_id;	/* out */
};

#define DRM_PANFROST_INFO 0x00

#define DRM_IOCTL_PANFROST_INFO DRM_IOR(DRM_COMMAND_BASE + DRM_PANFROST_INFO, struct drm_panfrost_info)

#if defined(__cplusplus)
}
#endif

#endif /* __PANFROST_DRM_H__ */
