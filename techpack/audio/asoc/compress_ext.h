/*
* SPDX-License-Identifier: GPL-2.0-only
* Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
*/

#ifndef _COMPRESS_EXT
#define _COMPRESS_EXT

#include <sound/compress_offload.h>

/* The tundra kernel carries the 5.4 Qualcomm compress UAPI already. */
#ifndef SND_COMPR_AUDIO_INFO

#define SNDRV_COMPRESS_RENDER_MODE_AUDIO_MASTER 0
#define SNDRV_COMPRESS_RENDER_MODE_STC_MASTER 1

#define SNDRV_COMPRESS_CLK_REC_MODE_NONE 0
#define SNDRV_COMPRESS_CLK_REC_MODE_AUTO 1

/**
* @SNDRV_COMPRESS_PATH_DELAY: dsp path delay in microseconds
* @SNDRV_COMPRESS_RENDER_MODE: dsp render mode (audio master or stc)
* @SNDRV_COMPRESS_CLK_REC_MODE: clock recovery mode ( none or auto)
* @SNDRV_COMPRESS_RENDER_WINDOW: render window
* @SNDRV_COMPRESS_START_DELAY: start delay
* @SNDRV_COMPRESS_ENABLE_ADJUST_SESSION_CLOCK: enable dsp drift correction
* @SNDRV_COMPRESS_ADJUST_SESSION_CLOCK: set drift correction value
* @SNDRV_COMPRESS_LATENCY_MODE: dsp latency mode metadata key
 */
enum sndrv_compress_encoder_ext {
	SNDRV_COMPRESS_PATH_DELAY = 3,
	SNDRV_COMPRESS_RENDER_MODE = 4,
	SNDRV_COMPRESS_CLK_REC_MODE = 5,
	SNDRV_COMPRESS_RENDER_WINDOW = 6,
	SNDRV_COMPRESS_START_DELAY = 7,
	SNDRV_COMPRESS_ENABLE_ADJUST_SESSION_CLOCK = 8,
	SNDRV_COMPRESS_ADJUST_SESSION_CLOCK = 9,
	SNDRV_COMPRESS_LATENCY_MODE = 10,
};

#define SNDRV_COMPRESS_PATH_DELAY SNDRV_COMPRESS_PATH_DELAY
#define SNDRV_COMPRESS_RENDER_MODE SNDRV_COMPRESS_RENDER_MODE
#define SNDRV_COMPRESS_CLK_REC_MODE SNDRV_COMPRESS_CLK_REC_MODE
#define SNDRV_COMPRESS_RENDER_WINDOW SNDRV_COMPRESS_RENDER_WINDOW
#define SNDRV_COMPRESS_START_DELAY SNDRV_COMPRESS_START_DELAY
#define SNDRV_COMPRESS_ENABLE_ADJUST_SESSION_CLOCK \
		SNDRV_COMPRESS_ENABLE_ADJUST_SESSION_CLOCK
#define SNDRV_COMPRESS_ADJUST_SESSION_CLOCK SNDRV_COMPRESS_ADJUST_SESSION_CLOCK
#define SNDRV_COMPRESS_LATENCY_MODE SNDRV_COMPRESS_LATENCY_MODE

#define SNDRV_COMPRESS_DSP_POSITION 10

#define COMPRESSED_TIMESTAMP_FLAG 0x0001

#define SND_AUDIOCODEC_TRUEHD                ((__u32) 0x00001001)
#define SND_AUDIOCODEC_MP2                   ((__u32) 0x00001006)
#define SND_AUDIOCODEC_AC3                   ((__u32) 0x00001008)
#define SND_AUDIOCODEC_DTS                   ((__u32) 0x0000100A)
#define SND_AUDIOCODEC_EAC3                  ((__u32) 0x0000100B)

/** struct snd_codec_metadata
* @length: Length of the encoded buffer.
* @offset: Offset from the buffer address to the first byte of the first
*          encoded frame. All encoded frames are consecutive starting
*          from this offset.
* @timestamp: Session time in microseconds of the first sample in the buffer.
* @reserved: Reserved for future use.
*/
struct snd_codec_metadata {
	__u32 length;
	__u32 offset;
	__u64 timestamp;
	__u32 reserved[4];
};

#endif /* SND_COMPR_AUDIO_INFO */

#endif /* _COMPRESS_EXT */
