/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sm6150 (violet) built-in display feature set: SDE, DSI, MDSS PLLs, RSC
 * and writeback. No DisplayPort: violet's Type-C port is USB 2.0 only.
 */

#define CONFIG_DRM_MSM 1
#define CONFIG_DRM_MSM_SDE 1
#define CONFIG_SYNC_FILE 1
#define CONFIG_DRM_MSM_DSI 1
#define CONFIG_DRM_SDE_WB 1
#define CONFIG_DRM_SDE_EVTLOG_DEBUG 1
#define CONFIG_QCOM_MDSS_PLL 1
#define CONFIG_DRM_SDE_RSC 1
#define CONFIG_MSM_SDE_ROTATOR 1
#define CONFIG_MSM_SDE_ROTATOR_EVTLOG_DEBUG 1
