/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CAM_CX_IPEAK_H_
#define _CAM_CX_IPEAK_H_

struct cam_hw_soc_info;

/*
 * 6.1 has no CX peak-current limits driver (soc/qcom/cx_ipeak.h), so the
 * camera does not vote on it.
 */
static inline int cam_cx_ipeak_register_cx_ipeak(
	struct cam_hw_soc_info *soc_info)
{
	return 0;
}

static inline int cam_cx_ipeak_update_vote_cx_ipeak(
	struct cam_hw_soc_info *soc_info, int32_t apply_level)
{
	return 0;
}

static inline int cam_cx_ipeak_unvote_cx_ipeak(
	struct cam_hw_soc_info *soc_info)
{
	return 0;
}

#endif /* _CAM_CX_IPEAK_H_ */
