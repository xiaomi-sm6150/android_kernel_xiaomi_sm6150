// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.​
 */
#include <linux/compat.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/freezer.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/timer.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/lsm_params.h>
#include <sound/pcm_params.h>
#include <dsp/msm_audio_ion.h>
#include <dsp/q6lsm.h>
#include "msm-pcm-routing-v2.h"

#include <linux/cdev.h>
#include <linux/file.h>

/*Defines number of minor numbers supported*/
#define MINOR_NUMBER_COUNT  1

#define DRV_NAME "msm-lsm-client"

#define CAPTURE_MIN_NUM_PERIODS     2
#define CAPTURE_MAX_NUM_PERIODS     8
#define CAPTURE_MAX_PERIOD_SIZE     61440
#define CAPTURE_MIN_PERIOD_SIZE     320
#define LISTEN_MAX_STATUS_PAYLOAD_SIZE 256


#define LAB_BUFFER_ALLOC 1
#define LAB_BUFFER_DEALLOC 0

#define LSM_IS_LAST_STAGE(client, stage_idx) \
	(client->num_stages == (stage_idx + 1))

#define LSM_ALIGN_4BYTE(x) (((x) + 3) & (~3))
#define LSM_POINTER_SIZE_32B 4
#define LSM_32B_ADDR_TRANSLATE(X) (X & 0xffffffff)
#define LSM_MAX_SESSION_COUNT 8

#define ENABLE_SVA_MIXER_CTL
#ifdef ENABLE_SVA_MIXER_CTL

/*The below one should be always aligned with that in ST HAL*/
#define LSM_LUT_CLEAR_INFO  _IOW('U', 0xF0, int)

/*Commands to control LSM module from HAL*/
enum lsm_ape_control {
	LSM_START = 1,
	LSM_STOP_LAB,
	LSM_ABORT_EVENT,
	LSM_STOP
};

/*
 * lsm cdev:
 * This is introduced to handle the communcation between
 * HAL and Kernel layer through IOCTL.
 * Older implementation was under QGKI flag,
 * which is removed from Android 13.
 */

struct lsm_cdev_info
{
	int dev_num;
	int det_status;
};

struct lsm_char_dev {
	struct class *cls;
	struct device *dev;
	struct cdev cdev;
	dev_t dev_num;
	/* Protects access to LSM client sessions and shared resources */
	struct mutex lock;
};

/*Commands for lookup table*/
enum
{
	SET_INFO = 1,
	GET_INFO,
	CLEAR_INFO
};

#endif

#ifdef ENABLE_SVA_MIXER_CTL
static struct snd_pcm_substream sub_str_info[LSM_MAX_SESSION_COUNT] = {0};
#endif

static struct snd_pcm_hardware msm_pcm_hardware_capture = {
	.info =                 (SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE),
	.rates =		(SNDRV_PCM_RATE_16000 |
				SNDRV_PCM_RATE_48000),
	.rate_min =             16000,
	.rate_max =             48000,
	.channels_min =         LSM_INPUT_NUM_CHANNELS_MIN,
	.channels_max =         LSM_INPUT_NUM_CHANNELS_MAX,
	.buffer_bytes_max =     CAPTURE_MAX_NUM_PERIODS *
				CAPTURE_MAX_PERIOD_SIZE,
	.period_bytes_min =	CAPTURE_MIN_PERIOD_SIZE,
	.period_bytes_max =     CAPTURE_MAX_PERIOD_SIZE,
	.periods_min =          CAPTURE_MIN_NUM_PERIODS,
	.periods_max =          CAPTURE_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	16000, 48000,
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

struct lsm_priv {
	struct snd_pcm_substream *substream;
	struct lsm_client *lsm_client;
	struct snd_lsm_event_status_v3 *event_status;
	struct snd_lsm_event_status *det_event;
	spinlock_t event_lock;
	wait_queue_head_t event_wait;
	unsigned long event_avail;
	atomic_t event_wait_stop;
	atomic_t buf_count;
	atomic_t read_abort;
	wait_queue_head_t period_wait;
	struct mutex lsm_api_lock;
	int appl_cnt;
	int dma_write;
	int xrun_count;
	int xrun_index;
	spinlock_t xrun_lock;
};

enum { /* lsm session states */
	IDLE = 0,
	RUNNING,
};

static int msm_lsm_queue_lab_buffer(struct lsm_priv *prtd, int i)
{
	int rc = 0;
	struct lsm_cmd_read cmd_read;
	struct snd_soc_pcm_runtime *rtd;

	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: Invalid params prtd %pK lsm client %pK\n",
			__func__, prtd, ((!prtd) ? NULL : prtd->lsm_client));
		return -EINVAL;
	}
	if (!prtd->substream || !prtd->substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!prtd->substream) ? "substream" : "private_data");
		return -EINVAL;
	}
	rtd = prtd->substream->private_data;

	if (!prtd->lsm_client->lab_buffer ||
		i >= prtd->lsm_client->out_hw_params.period_count) {
		dev_err(rtd->dev,
			"%s: Lab buffer not setup %pK incorrect index %d period count %d\n",
			__func__, prtd->lsm_client->lab_buffer, i,
			prtd->lsm_client->out_hw_params.period_count);
		return -EINVAL;
	}
	cmd_read.buf_addr_lsw =
		lower_32_bits(prtd->lsm_client->lab_buffer[i].phys);
	cmd_read.buf_addr_msw =
		msm_audio_populate_upper_32_bits(
				prtd->lsm_client->lab_buffer[i].phys);
	cmd_read.buf_size = prtd->lsm_client->lab_buffer[i].size;
	cmd_read.mem_map_handle =
		prtd->lsm_client->lab_buffer[i].mem_map_handle;
	rc = q6lsm_read(prtd->lsm_client, &cmd_read);
	if (rc)
		dev_err(rtd->dev,
			"%s: error in queuing the lab buffer rc %d\n",
			__func__, rc);
	return rc;
}

static int lsm_lab_buffer_sanity(struct lsm_priv *prtd,
		struct lsm_cmd_read_done *read_done, int *index)
{
	int i = 0, rc = -EINVAL;
	struct snd_soc_pcm_runtime *rtd;

	if (!prtd || !read_done || !index) {
		pr_err("%s: Invalid params prtd %pK read_done %pK index %pK\n",
			__func__, prtd, read_done, index);
		return -EINVAL;
	}

	if (!prtd->substream || !prtd->substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!prtd->substream) ? "substream" : "private_data");
		return -EINVAL;
	}
	rtd = prtd->substream->private_data;

	if (!prtd->lsm_client->lab_enable || !prtd->lsm_client->lab_buffer) {
		dev_err(rtd->dev,
			"%s: Lab not enabled %d invalid lab buffer %pK\n",
			__func__, prtd->lsm_client->lab_enable,
			prtd->lsm_client->lab_buffer);
		return -EINVAL;
	}
	for (i = 0; i < prtd->lsm_client->out_hw_params.period_count; i++) {
		if ((lower_32_bits(prtd->lsm_client->lab_buffer[i].phys) ==
			read_done->buf_addr_lsw) &&
			(msm_audio_populate_upper_32_bits
				(prtd->lsm_client->lab_buffer[i].phys) ==
			read_done->buf_addr_msw) &&
			(prtd->lsm_client->lab_buffer[i].mem_map_handle ==
			read_done->mem_map_handle)) {
			dev_dbg(rtd->dev,
				"%s: Buffer found %pK memmap handle %d\n",
				__func__, &prtd->lsm_client->lab_buffer[i].phys,
			prtd->lsm_client->lab_buffer[i].mem_map_handle);
			if (read_done->total_size >
				prtd->lsm_client->lab_buffer[i].size) {
				dev_err(rtd->dev,
					"%s: Size mismatch call back size %d actual size %zd\n",
					__func__, read_done->total_size,
				prtd->lsm_client->lab_buffer[i].size);
				rc = -EINVAL;
				break;
			} else {
				*index = i;
				rc = 0;
				break;
			}
		}
	}
	return rc;
}

static void lsm_event_handler(uint32_t opcode, uint32_t token,
			      uint32_t *payload, uint16_t client_size,
				void *priv)
{
	unsigned long flags;
	struct lsm_priv *prtd = priv;
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_lsm_event_status_v3 *temp;
	uint16_t status = 0;
	uint16_t payload_size = 0;
	uint16_t index = 0;
	uint32_t event_ts_lsw = 0;
	uint32_t event_ts_msw = 0;

	if (!substream || !substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!substream) ? "substream" : "private_data");
		return;
	}
	rtd = substream->private_data;

	dev_dbg(rtd->dev, "%s: opcode %x\n", __func__, opcode);
	switch (opcode) {
	case LSM_DATA_EVENT_READ_DONE: {
		int rc;
		struct lsm_cmd_read_done *read_done = (struct lsm_cmd_read_done *)payload;
		int buf_index = 0;
		unsigned long flags = 0;

		if (prtd->lsm_client->session != token ||
		    !read_done) {
			dev_err(rtd->dev,
				"%s: EVENT_READ_DONE invalid callback, session %d callback %d payload %pK",
				__func__, prtd->lsm_client->session,
				token, read_done);
			return;
		}
		if (atomic_read(&prtd->read_abort)) {
			dev_dbg(rtd->dev,
				"%s: read abort set skip data\n", __func__);
			return;
		}
		if (!lsm_lab_buffer_sanity(prtd, read_done, &buf_index)) {
			dev_dbg(rtd->dev,
				"%s: process read done index %d\n",
				__func__, buf_index);
			if (buf_index >=
				prtd->lsm_client->out_hw_params.period_count) {
				dev_err(rtd->dev,
					"%s: Invalid index %d buf_index max cnt %d\n",
					__func__, buf_index,
				prtd->lsm_client->out_hw_params.period_count);
				return;
			}
			spin_lock_irqsave(&prtd->xrun_lock, flags);
			prtd->dma_write += read_done->total_size;
			atomic_inc(&prtd->buf_count);
			snd_pcm_period_elapsed(substream);
			wake_up(&prtd->period_wait);
			if (atomic_read(&prtd->buf_count) <
				prtd->lsm_client->out_hw_params.period_count) {
				/* queue the next period buffer */
				buf_index = (buf_index + 1) %
				prtd->lsm_client->out_hw_params.period_count;
				rc = msm_lsm_queue_lab_buffer(prtd, buf_index);
				if (rc)
					dev_err(rtd->dev,
						"%s: error in queuing the lab buffer rc %d\n",
						__func__, rc);
			} else {
				dev_dbg(rtd->dev,
					"%s: xrun: further lab to be queued after read from user\n",
					 __func__);
				if (!prtd->xrun_count)
					prtd->xrun_index = buf_index;
				(prtd->xrun_count)++;
			}
			spin_unlock_irqrestore(&prtd->xrun_lock, flags);
		} else
			dev_err(rtd->dev, "%s: Invalid lab buffer returned by dsp\n",
				__func__);
		break;
	}

	case LSM_SESSION_EVENT_DETECTION_STATUS:
                if (client_size < 3 * sizeof(uint8_t)) {
			dev_err(rtd->dev,
					"%s: client_size has invalid size[%d]\n",
					__func__, client_size);
			return;
		}
		status = (uint16_t)((uint8_t *)payload)[0];
		payload_size = (uint16_t)((uint8_t *)payload)[2];
		index = 4;
		dev_dbg(rtd->dev,
			"%s: event detect status = %d payload size = %d\n",
			__func__, status, payload_size);
		break;

	case LSM_SESSION_EVENT_DETECTION_STATUS_V2:
		if (client_size < 2 * sizeof(uint8_t)) {
			dev_err(rtd->dev,
					"%s: client_size has invalid size[%d]\n",
					__func__, client_size);
			return;
		}
		status = (uint16_t)((uint8_t *)payload)[0];
		payload_size = (uint16_t)((uint8_t *)payload)[1];
		index = 2;
		dev_dbg(rtd->dev,
			"%s: event detect status_v2 = %d payload size = %d\n",
			__func__, status, payload_size);
		break;

	case LSM_SESSION_EVENT_DETECTION_STATUS_V3:
		if (client_size < 2 * (sizeof(uint32_t) + sizeof(uint8_t))) {
			dev_err(rtd->dev,
					"%s: client_size has invalid size[%d]\n",
					__func__, client_size);
			return;
		}
		event_ts_lsw = ((uint32_t *)payload)[0];
		event_ts_msw = ((uint32_t *)payload)[1];
		status = (uint16_t)((uint8_t *)payload)[8];
		payload_size = (uint16_t)((uint8_t *)payload)[9];
		index = 10;
		dev_dbg(rtd->dev,
			"%s: ts_msw = %u, ts_lsw = %u, event detect status = %d payload size = %d\n",
			__func__, event_ts_msw, event_ts_lsw, status,
			payload_size);
		break;

	case LSM_SESSION_DETECTION_ENGINE_GENERIC_EVENT: {
		struct snd_lsm_event_status *tmp;
		if (client_size < 2 * sizeof(uint16_t)) {
			dev_err(rtd->dev,
					"%s: client_size has invalid size[%d]\n",
					__func__, client_size);
			return;
		}


		status = ((uint16_t *)payload)[0];
		payload_size = ((uint16_t *)payload)[1];

		spin_lock_irqsave(&prtd->event_lock, flags);
		tmp = krealloc(prtd->det_event,
			       sizeof(struct snd_lsm_event_status) +
			       payload_size, GFP_ATOMIC);
		if (!tmp) {
			spin_unlock_irqrestore(&prtd->event_lock, flags);
			dev_err(rtd->dev,
				"%s: Failed to allocate memory for %s, size = %zu\n",
				__func__,
				"LSM_SESSION_DETECTION_ENGINE_GENERIC_EVENT",
				sizeof(struct snd_lsm_event_status) +
				payload_size);
			return;
		}

		prtd->det_event = tmp;
		prtd->det_event->status = status;
		prtd->det_event->payload_size = payload_size;
		if (client_size >= payload_size + 4) {
			memcpy(prtd->det_event->payload,
				&((uint8_t *)payload)[4], payload_size);
		} else {
			spin_unlock_irqrestore(&prtd->event_lock, flags);
			dev_err(rtd->dev,
				"%s: Failed to copy memory with invalid size = %d\n",
				__func__, payload_size);
			return;
		}
		prtd->event_avail = 1;
		spin_unlock_irqrestore(&prtd->event_lock, flags);
		wake_up(&prtd->event_wait);

#ifdef CONFIG_SND_SVA_SYMBOL_TRIMMING
		if (substream->timer_running)
			snd_timer_interrupt(substream->timer, 1);
#endif
		dev_dbg(rtd->dev,
			"%s: Generic det event status = %d payload size = %d\n",
			__func__, prtd->det_event->status,
			prtd->det_event->payload_size);
		break;
	}

	default:
		break;
	}

	if (opcode == LSM_SESSION_EVENT_DETECTION_STATUS ||
		opcode == LSM_SESSION_EVENT_DETECTION_STATUS_V2 ||
		opcode == LSM_SESSION_EVENT_DETECTION_STATUS_V3) {
		spin_lock_irqsave(&prtd->event_lock, flags);
		dev_dbg(rtd->dev, "%s: detection status\n", __func__);
		temp = krealloc(prtd->event_status,
				sizeof(struct snd_lsm_event_status_v3) +
				payload_size, GFP_ATOMIC);
		if (!temp) {
			dev_err(rtd->dev, "%s: no memory for event status\n",
				__func__);
			spin_unlock_irqrestore(&prtd->event_lock, flags);
			return;
		}
		/*
		 * event status timestamp will be non-zero and valid if
		 * opcode is LSM_SESSION_EVENT_DETECTION_STATUS_V3
		 */
		prtd->event_status = temp;
		prtd->event_status->timestamp_lsw = event_ts_lsw;
		prtd->event_status->timestamp_msw = event_ts_msw;
		prtd->event_status->status = status;
		prtd->event_status->payload_size = payload_size;

		if (likely(prtd->event_status)) {
			if (client_size >= (payload_size + index)) {
				memcpy(prtd->event_status->payload,
					&((uint8_t *)payload)[index],
					payload_size);
				prtd->event_avail = 1;
				spin_unlock_irqrestore(&prtd->event_lock, flags);
				dev_dbg(rtd->dev, "%s: wakeup event_wait\n", __func__);
				wake_up(&prtd->event_wait);
			} else {
				spin_unlock_irqrestore(&prtd->event_lock, flags);
				dev_err(rtd->dev,
						"%s: Failed to copy memory with invalid size = %d\n",
						__func__, payload_size);
				return;
			}
		} else {
			spin_unlock_irqrestore(&prtd->event_lock, flags);
			dev_err(rtd->dev,
				"%s: Couldn't allocate %d bytes of memory\n",
				__func__, payload_size);
		}

#ifdef CONFIG_SND_SVA_SYMBOL_TRIMMING
		if (substream->timer_running)
			snd_timer_interrupt(substream->timer, 1);
#endif
	}
	dev_dbg(rtd->dev, "%s: leave\n", __func__);
}

static int msm_lsm_lab_buffer_alloc(struct lsm_priv *lsm, int alloc)
{
	int ret = 0;
	struct snd_dma_buffer *dma_buf = NULL;

	if (!lsm) {
		pr_err("%s: Invalid param lsm %pK\n", __func__, lsm);
		return -EINVAL;
	}
	if (alloc) {
		if (!lsm->substream) {
			pr_err("%s: substream is NULL\n", __func__);
			return -EINVAL;
		}
		ret = q6lsm_lab_buffer_alloc(lsm->lsm_client, alloc);
		if (ret) {
			pr_err("%s: alloc lab buffer failed ret %d\n",
				__func__, ret);
			goto exit;
		}
		dma_buf = &lsm->substream->dma_buffer;
		dma_buf->dev.type = SNDRV_DMA_TYPE_DEV;
		dma_buf->dev.dev = lsm->substream->pcm->card->dev;
		dma_buf->private_data = NULL;
		dma_buf->area = lsm->lsm_client->lab_buffer[0].data;
		dma_buf->addr = lsm->lsm_client->lab_buffer[0].phys;
		dma_buf->bytes = lsm->lsm_client->out_hw_params.buf_sz *
		lsm->lsm_client->out_hw_params.period_count;
		snd_pcm_set_runtime_buffer(lsm->substream, dma_buf);
	} else {
		ret = q6lsm_lab_buffer_alloc(lsm->lsm_client, alloc);
		if (ret)
			pr_err("%s: free lab buffer failed ret %d\n",
				__func__, ret);
		kfree(lsm->lsm_client->lab_buffer);
		lsm->lsm_client->lab_buffer = NULL;
	}
exit:
	return ret;
}

static int msm_lsm_get_conf_levels(struct lsm_client *client,
				   u8 *conf_levels_ptr)
{
	int rc = 0;

	if (client->num_sound_models != 0) {
		if (client->num_keywords == 0) {
			pr_debug("%s: no number of confidence_values provided\n",
				 __func__);
			client->multi_snd_model_confidence_levels = NULL;
			goto done;
		}

		client->multi_snd_model_confidence_levels =
			kzalloc((sizeof(uint32_t) * client->num_keywords),
				 GFP_KERNEL);
		if (!client->multi_snd_model_confidence_levels) {
			pr_err("%s: No memory for confidence\n"
				"levels num of level from user = %d\n",
				__func__, client->num_keywords);
				rc = -ENOMEM;
				goto done;
		}

		if (copy_from_user((u8 *)client->multi_snd_model_confidence_levels,
				   conf_levels_ptr,
				   sizeof(uint32_t) * client->num_keywords)) {
			pr_err("%s: copy from user failed, number of keywords = %d\n",
			       __func__, client->num_keywords);
			rc = -EFAULT;
			goto copy_err;
		}
	} else {
		if (client->num_confidence_levels == 0) {
			pr_debug("%s: no confidence levels provided\n",
				 __func__);
			client->confidence_levels = NULL;
			goto done;
		}

		client->confidence_levels =
			kzalloc((sizeof(uint8_t) * client->num_confidence_levels),
				 GFP_KERNEL);
		if (!client->confidence_levels) {
			pr_err("%s: No memory for confidence\n"
				"levels num of level from user = %d\n",
				__func__, client->num_confidence_levels);
				rc = -ENOMEM;
				goto done;
		}

		if (copy_from_user(client->confidence_levels,
				   conf_levels_ptr,
				   client->num_confidence_levels)) {
			pr_err("%s: copy from user failed, size = %d\n",
			       __func__, client->num_confidence_levels);
			rc = -EFAULT;
			goto copy_err;
		}
	}
	return rc;

copy_err:
	if (client->num_sound_models != 0) {
		kfree(client->multi_snd_model_confidence_levels);
		client->multi_snd_model_confidence_levels = NULL;
	} else {
		kfree(client->confidence_levels);
		client->confidence_levels = NULL;
	}
done:
	return rc;

}

static int msm_lsm_set_epd(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int rc = 0;
	struct snd_lsm_ep_det_thres epd_th;

	if (p_info->param_size != sizeof(epd_th)) {
		dev_err(rtd->dev,
			"%s: Invalid param_size %d\n",
			__func__, p_info->param_size);
		rc = -EINVAL;
		goto done;
	}

	if (copy_from_user(&epd_th, p_info->param_data,
			   p_info->param_size)) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed, size = %d\n",
			__func__, p_info->param_size);
		rc = -EFAULT;
		goto done;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 &epd_th, LSM_ENDPOINT_DETECT_THRESHOLD);
	if (rc)
		dev_err(rtd->dev,
			"%s: Failed to set epd param, err = %d\n",
			__func__, rc);
done:
	return rc;
}

static int msm_lsm_set_mode(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_lsm_detect_mode mode;
	int rc = 0;

	if (p_info->param_size != sizeof(mode)) {
		dev_err(rtd->dev,
			"%s: Invalid param_size %d\n",
			__func__, p_info->param_size);
		rc = -EINVAL;
		goto done;
	}

	if (copy_from_user(&mode, p_info->param_data,
			   sizeof(mode))) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed, size = %zd\n",
			__func__, sizeof(mode));
		rc = -EFAULT;
		goto done;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 &mode, LSM_OPERATION_MODE);
	if (rc)
		dev_err(rtd->dev,
			"%s: Failed to set det_mode param, err = %d\n",
			__func__, rc);
done:
	return rc;
}

static int msm_lsm_set_gain(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_lsm_gain gain;
	int rc = 0;

	if (p_info->param_size != sizeof(gain)) {
		dev_err(rtd->dev,
			"%s: Invalid param_size %d\n",
			__func__, p_info->param_size);
		rc = -EINVAL;
		goto done;
	}

	if (copy_from_user(&gain, p_info->param_data,
			   sizeof(gain))) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed, size = %zd\n",
			__func__, sizeof(gain));
		rc = -EFAULT;
		goto done;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 &gain, LSM_GAIN);
	if (rc)
		dev_err(rtd->dev,
			"%s: Failed to set det_mode param, err = %d\n",
			__func__, rc);
done:
	return rc;
}

static int msm_lsm_set_conf(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = NULL;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int rc = 0;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;

	if (!rtd) {
		pr_err("%s substream runtime or private_data not found\n",
			__func__);
		return -EINVAL;
	}
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	if (p_info->param_type == LSM_MULTI_SND_MODEL_CONFIDENCE_LEVELS) {
		if (p_info->param_size > MAX_KEYWORDS_SUPPORTED) {
			dev_err(rtd->dev,
				"%s: invalid number of snd_model keywords %d, the max is %d\n",
				__func__, p_info->param_size, MAX_KEYWORDS_SUPPORTED);
			mutex_unlock(&lsm_dev->lock);
			return -EINVAL;
		}

		prtd->lsm_client->num_keywords = p_info->param_size;
		rc = msm_lsm_get_conf_levels(prtd->lsm_client,
					     p_info->param_data);
		if (rc) {
			dev_err(rtd->dev,
				"%s: get_conf_levels failed for snd_model %d, err = %d\n",
				__func__, p_info->model_id, rc);
			mutex_unlock(&lsm_dev->lock);
			return rc;
		}

		rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
					 prtd->lsm_client->multi_snd_model_confidence_levels,
					 LSM_MULTI_SND_MODEL_CONFIDENCE_LEVELS);
		if (rc)
			dev_err(rtd->dev,
				"%s: Failed to set multi_snd_model_confidence_levels, err = %d\n",
				__func__, rc);

		if (prtd->lsm_client->multi_snd_model_confidence_levels) {
			kfree(prtd->lsm_client->multi_snd_model_confidence_levels);
			prtd->lsm_client->multi_snd_model_confidence_levels = NULL;
		}
	} else {
		if (p_info->param_size > MAX_NUM_CONFIDENCE) {
			dev_err(rtd->dev,
				"%s: invalid confidence levels %d\n",
				__func__, p_info->param_size);
			mutex_unlock(&lsm_dev->lock);
			return -EINVAL;
		}

		prtd->lsm_client->num_confidence_levels =
				p_info->param_size;
		rc = msm_lsm_get_conf_levels(prtd->lsm_client,
					     p_info->param_data);
		if (rc) {
			dev_err(rtd->dev,
				"%s: get_conf_levels failed, err = %d\n",
				__func__, rc);
			mutex_unlock(&lsm_dev->lock);
			return rc;
		}

		rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
					 prtd->lsm_client->confidence_levels,
					 LSM_MIN_CONFIDENCE_LEVELS);
		if (rc)
			dev_err(rtd->dev,
				"%s: Failed to set min_conf_levels, err = %d\n",
				__func__, rc);

		if (prtd->lsm_client->confidence_levels) {
			kfree(prtd->lsm_client->confidence_levels);
			prtd->lsm_client->confidence_levels = NULL;
		}
	}
	mutex_unlock(&lsm_dev->lock);
	return rc;
}

static int msm_lsm_reg_model(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int rc = 0, stage_idx = p_info->stage_idx;
	struct lsm_sound_model *sm = NULL;
	size_t offset = sizeof(union param_hdrs);
	struct lsm_client *client = prtd->lsm_client;

	if (p_info->model_id != 0 &&
	    p_info->param_type == LSM_REG_MULTI_SND_MODEL) {
		sm = kzalloc(sizeof(*sm), GFP_KERNEL);
		if (sm == NULL) {
			dev_err(rtd->dev, "%s: snd_model kzalloc failed\n", __func__);
			return -ENOMEM;
		}

		INIT_LIST_HEAD(&sm->list);

		rc = q6lsm_snd_model_buf_alloc(client, p_info->param_size, p_info, sm);
		if (rc) {
			dev_err(rtd->dev, "%s: snd_model buf alloc failed, size = %d\n",
				__func__, p_info->param_size);
			goto err_buf_alloc;
		}

		q6lsm_sm_set_param_data(client, p_info, &offset, sm);

		if ((sm->size - offset) < p_info->param_size) {
			dev_err(rtd->dev, "%s: user buff size is greater than expected\n",
				__func__);
			rc = -EINVAL;
			goto err_copy;
		}

		/*
		 * For set_param, advance the sound model data with the
		 * number of bytes required by param_data.
		 */
		if (copy_from_user((u8 *)sm->data + offset,
			p_info->param_data, p_info->param_size)) {
			dev_err(rtd->dev,
				"%s: copy_from_user for snd_model %d failed, size = %d\n",
				__func__, p_info->model_id, p_info->param_size);
			rc = -EFAULT;
			goto err_copy;
		}
		/* Add this sound model to the list of multi sound models */
		list_add_tail(&sm->list, &client->stage_cfg[stage_idx].sound_models);

		rc = q6lsm_set_one_param(client, p_info, NULL,
					 LSM_REG_MULTI_SND_MODEL);
		if (rc) {
			dev_err(rtd->dev,
				"%s: Failed to register snd_model %d, err = %d\n",
				__func__, p_info->model_id, rc);
			goto err_copy;
		}

		client->num_sound_models++;
		dev_dbg(rtd->dev,
			"%s: registered snd_model: %d, total num of snd_model: %d\n",
			__func__, p_info->model_id, client->num_sound_models);
	} else if (p_info->model_id == 0 &&
		   p_info->param_type == LSM_REG_SND_MODEL) {
		sm = &client->stage_cfg[stage_idx].sound_model;

		rc = q6lsm_snd_model_buf_alloc(client, p_info->param_size, p_info, sm);
		if (rc) {
			dev_err(rtd->dev, "%s: snd_model buf alloc failed, size = %d\n",
				__func__, p_info->param_size);
			return rc;
		}

		q6lsm_sm_set_param_data(client, p_info, &offset, sm);

		if ((sm->size - offset) < p_info->param_size) {
			dev_err(rtd->dev, "%s: user buff size is greater than expected\n",
				__func__);
			rc = -EINVAL;
			goto err_copy;
		}

		/*
		 * For set_param, advance the sound model data with the
		 * number of bytes required by param_data.
		 */

		if (copy_from_user((u8 *)sm->data + offset,
		    p_info->param_data, p_info->param_size)) {
			dev_err(rtd->dev,
				"%s: copy_from_user for snd_model failed, size = %d\n",
				__func__, p_info->param_size);
			rc = -EFAULT;
			goto err_copy;
		}
		rc = q6lsm_set_one_param(client, p_info, NULL, LSM_REG_SND_MODEL);
		if (rc) {
			dev_err(rtd->dev, "%s: Failed to register snd_model, err = %d\n",
				__func__, rc);
			goto err_copy;
		}
	} else {
		dev_err(rtd->dev,
			"%s: snd_model id %d is invalid for param type %d\n",
			__func__, p_info->model_id, p_info->param_type);
	}
	return rc;

err_copy:
	q6lsm_snd_model_buf_free(client, p_info, sm);
err_buf_alloc:
	if (p_info->model_id != 0) {
		list_del(&sm->list);
		kfree(sm);
		sm = NULL;
	}
	return rc;
}

static int msm_lsm_dereg_model(struct snd_pcm_substream *substream,
			struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct lsm_sound_model *sm = NULL;
	struct lsm_client *client = prtd->lsm_client;
	int rc = 0;

	if (p_info->model_id != 0 &&
		p_info->param_type == LSM_DEREG_MULTI_SND_MODEL) {

		if (list_empty(&client->stage_cfg[p_info->stage_idx].sound_models)) {
			pr_err("%s: sound_models list is empty\n",
				__func__);
			return -EINVAL;
		}

		list_for_each_entry(sm,
				   &client->stage_cfg[p_info->stage_idx].sound_models,
				   list) {
			dev_dbg(rtd->dev,
				"%s: current snd_model: %d, looking for snd_model %d\n",
				 __func__, sm->model_id, p_info->model_id);
			if (sm->model_id == p_info->model_id)
				break;
		}

		if (sm->model_id == p_info->model_id) {
			if (p_info->param_size != sizeof(p_info->model_id)) {
				rc = -EINVAL;
				pr_err("%s: %s failed, p_info->param_size is invalid: %d\n",
				       __func__,  "LSM_DEREG_MULTI_SND_MODEL",
				       p_info->param_size);
			} else {
				rc = q6lsm_set_one_param(client, p_info, NULL,
							 LSM_DEREG_MULTI_SND_MODEL);
			}

			if (rc)
				dev_err(rtd->dev,
					"%s: Failed to deregister snd_model %d, err = %d\n",
					__func__, p_info->model_id, rc);

			q6lsm_snd_model_buf_free(client, p_info, sm);
			list_del(&sm->list);
			kfree(sm);
			sm = NULL;
			client->num_sound_models--;
		} else {
			rc = -EINVAL;
			dev_err(rtd->dev,
				"%s: Failed to find snd_model, invalid model_id %d\n",
				__func__, p_info->model_id);
		}
	} else if (p_info->model_id == 0 &&
		   p_info->param_type == LSM_DEREG_SND_MODEL) {
		rc = q6lsm_set_one_param(client, p_info, NULL,
					 LSM_DEREG_SND_MODEL);
		if (rc)
			dev_err(rtd->dev,
				"%s: Failed to deregister snd_model, err = %d\n",
				__func__, rc);

		sm = &client->stage_cfg[p_info->stage_idx].sound_model;
		q6lsm_snd_model_buf_free(client, p_info, sm);
	} else {
		rc = -EINVAL;
		dev_err(rtd->dev,
			"%s: snd_model id %d is invalid for param type %d\n",
			__func__, p_info->model_id, p_info->param_type);
	}

	return rc;
}

static int msm_lsm_set_custom(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	u8 *data;
	int rc = 0;

	data = kzalloc(p_info->param_size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (copy_from_user(data, p_info->param_data,
			   p_info->param_size)) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed for custom params, size = %d\n",
			__func__, p_info->param_size);
		rc = -EFAULT;
		goto err_ret;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 data, LSM_CUSTOM_PARAMS);
	if (rc)
		dev_err(rtd->dev,
			"%s: Failed to set custom param, err = %d\n",
			__func__, rc);

err_ret:
	kfree(data);
	return rc;
}

static int msm_lsm_check_and_set_lab_controls(struct snd_pcm_substream *substream,
			u32 enable, struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = NULL;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct lsm_hw_params *out_hw_params = NULL;
	struct snd_soc_component *component = NULL;
	struct lsm_char_dev *lsm_dev = NULL;
	u8 *chmap = NULL;
	u32 ch_idx;
	int rc = 0, stage_idx = p_info->stage_idx;

	if (!rtd) {
		pr_err("%s substream runtime or private_data not found\n",
			__func__);
		return -EINVAL;
	}
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	out_hw_params = &prtd->lsm_client->out_hw_params;
	if (!out_hw_params) {
		pr_err("%s: Invalid hw params\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	if (prtd->lsm_client->stage_cfg[stage_idx].lab_enable == enable) {
		dev_dbg(rtd->dev, "%s: Lab for session %d, stage %d already %s\n",
				__func__, prtd->lsm_client->session,
				stage_idx, enable ? "enabled" : "disabled");
		mutex_unlock(&lsm_dev->lock);
		return rc;
	}

	chmap = kzalloc(out_hw_params->num_chs, GFP_KERNEL);
	if (!chmap) {
		mutex_unlock(&lsm_dev->lock);
		return -ENOMEM;
	}

	rc = q6lsm_lab_control(prtd->lsm_client, enable, p_info);
	if (rc) {
		dev_err(rtd->dev, "%s: Failed to set lab_control param, err = %d\n",
			__func__, rc);
		goto fail;
	} else {
		if (LSM_IS_LAST_STAGE(prtd->lsm_client, stage_idx)) {
			rc = msm_lsm_lab_buffer_alloc(prtd,
					enable ? LAB_BUFFER_ALLOC : LAB_BUFFER_DEALLOC);
			if (rc) {
				dev_err(rtd->dev,
					"%s: msm_lsm_lab_buffer_alloc failed rc %d for %s\n",
					__func__, rc, enable ? "ALLOC" : "DEALLOC");
				goto fail;
			} else {
				/* set client level flag based on last stage control */
				prtd->lsm_client->lab_enable = enable;
			}
		}
		if (!rc)
			prtd->lsm_client->stage_cfg[stage_idx].lab_enable = enable;
	}

	/*
	 * First channel to be read from lab is always the
	 * best channel (0xff). For second channel onwards,
	 * the channel indices are 0, 1, .. etc
	 */
	chmap[0] = 0xFF;
	for (ch_idx = 1; ch_idx < out_hw_params->num_chs; ch_idx++)
		chmap[ch_idx] = ch_idx - 1;

	rc = q6lsm_lab_out_ch_cfg(prtd->lsm_client, chmap, p_info);
	if (rc)
		dev_err(rtd->dev, "%s: Failed to set lab out ch cfg %d\n",
			__func__, rc);

fail:
	kfree(chmap);
	mutex_unlock(&lsm_dev->lock);
	return rc;
}

static int msm_lsm_set_lab_control(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_lsm_lab_control lab_ctrl;
	int rc = 0;

	if (p_info->param_size != sizeof(lab_ctrl))
		return -EINVAL;

	if (prtd->lsm_client->started) {
		dev_err(rtd->dev, "%s: lab control sent after start\n", __func__);
		return -EAGAIN;
	}

	if (copy_from_user(&lab_ctrl, p_info->param_data,
			   p_info->param_size)) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed for lab_control params, size = %d\n",
			__func__, p_info->param_size);
		return  -EFAULT;
	}

	rc = msm_lsm_check_and_set_lab_controls(substream, lab_ctrl.enable, p_info);
	return rc;
}

static int msm_lsm_set_poll_enable(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_lsm_poll_enable poll_enable;
	int rc = 0;

	if (p_info->param_size != sizeof(poll_enable)) {
		dev_err(rtd->dev,
			"%s: Invalid param_size %d\n",
			__func__, p_info->param_size);
		rc = -EINVAL;
		goto done;
	}

	if (copy_from_user(&poll_enable, p_info->param_data,
			   sizeof(poll_enable))) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed, size = %zd\n",
			__func__, sizeof(poll_enable));
		rc = -EFAULT;
		goto done;
	}

	if (prtd->lsm_client->poll_enable == poll_enable.poll_en) {
		dev_dbg(rtd->dev,
			"%s: Polling for session %d already %s\n",
			__func__, prtd->lsm_client->session,
			(poll_enable.poll_en ? "enabled" : "disabled"));
		rc = 0;
		goto done;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 &poll_enable, LSM_POLLING_ENABLE);
	if (!rc) {
		prtd->lsm_client->poll_enable = poll_enable.poll_en;
	} else {
		dev_err(rtd->dev,
			"%s: Failed to set poll enable, err = %d\n",
			__func__, rc);
	}
done:
	return rc;
}

static int msm_lsm_set_det_event_type(struct snd_pcm_substream *substream,
				      struct lsm_params_info_v2 *p_info)
{
	struct snd_lsm_det_event_type det_event_type;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int rc = 0;

	if (p_info->param_size != sizeof(det_event_type)) {
		dev_err(rtd->dev,
			"%s: Invalid param_size %d\n",
			__func__, p_info->param_size);
		rc = -EINVAL;
		goto done;
	}

	if (copy_from_user(&det_event_type, p_info->param_data,
			   sizeof(det_event_type))) {
		dev_err(rtd->dev,
			"%s: copy_from_user failed, size = %zd\n",
			__func__, sizeof(det_event_type));
		rc = -EFAULT;
		goto done;
	}

	rc = q6lsm_set_one_param(prtd->lsm_client, p_info,
				 &det_event_type, LSM_DET_EVENT_TYPE);
	if (!rc)
		prtd->lsm_client->event_type = det_event_type.event_type;
	else
		dev_err(rtd->dev,
			"%s: Failed to set detection event type %s, err = %d\n",
			__func__, (det_event_type.event_type ?
			"LSM_DET_EVENT_TYPE_GENERIC" :
			"LSM_DET_EVENT_TYPE_LEGACY"), rc);
done:
	return rc;
}

static int msm_lsm_process_params(struct snd_pcm_substream *substream,
		struct lsm_params_info_v2 *p_info)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int rc = 0;

	dev_dbg(rtd->dev,
		"%s: mid=0x%x, pid=0x%x, iid=0x%x, stage_idx=%d, size=0x%x, type=%d\n",
		__func__, p_info->module_id, p_info->param_id, p_info->instance_id,
		p_info->stage_idx, p_info->param_size, p_info->param_type);

	if (!prtd->lsm_client ||
		prtd->lsm_client->num_stages <= p_info->stage_idx) {
		dev_err(rtd->dev,
			"%s: invalid stage_idx(%d) for client(%p) having num_stages(%d)\n",
			__func__, p_info->stage_idx, prtd->lsm_client,
			prtd->lsm_client ? prtd->lsm_client->num_stages : 0);
		return -EINVAL;
	}

	if (p_info->param_type == LSM_REG_MULTI_SND_MODEL &&
		prtd->lsm_client->num_sound_models == LSM_MAX_SOUND_MODELS_SUPPORTED) {
		dev_err(rtd->dev,
			"%s: maximum supported sound models(8) have already reached\n",
			__func__);
		return -EINVAL;
	}

	if (p_info->param_type == LSM_DEREG_MULTI_SND_MODEL &&
		prtd->lsm_client->num_sound_models == 0) {
		dev_err(rtd->dev,
			"%s: no available sound model to be deregistered\n",
			__func__);
		return -EINVAL;
	}

	switch (p_info->param_type) {
	case LSM_ENDPOINT_DETECT_THRESHOLD:
		rc = msm_lsm_set_epd(substream, p_info);
		break;
	case LSM_OPERATION_MODE:
		rc = msm_lsm_set_mode(substream, p_info);
		break;
	case LSM_GAIN:
		rc = msm_lsm_set_gain(substream, p_info);
		break;
	case LSM_MIN_CONFIDENCE_LEVELS:
	case LSM_MULTI_SND_MODEL_CONFIDENCE_LEVELS:
		rc = msm_lsm_set_conf(substream, p_info);
		break;
	case LSM_REG_SND_MODEL:
	case LSM_REG_MULTI_SND_MODEL:
		rc = msm_lsm_reg_model(substream, p_info);
		break;
	case LSM_DEREG_SND_MODEL:
	case LSM_DEREG_MULTI_SND_MODEL:
		rc = msm_lsm_dereg_model(substream, p_info);
		break;
	case LSM_CUSTOM_PARAMS:
		rc = msm_lsm_set_custom(substream, p_info);
		break;
	case LSM_POLLING_ENABLE:
		rc = msm_lsm_set_poll_enable(substream, p_info);
		break;
	case LSM_DET_EVENT_TYPE:
		rc = msm_lsm_set_det_event_type(substream, p_info);
		break;
	case LSM_LAB_CONTROL:
		rc = msm_lsm_set_lab_control(substream, p_info);
		break;
	default:
		dev_err(rtd->dev,
			"%s: Invalid param_type %d\n",
			__func__, p_info->param_type);
		rc = -EINVAL;
		break;
	}
	if (rc) {
		pr_err("%s: set_param fail for param_type %d\n",
			__func__, p_info->param_type);
	}

	return rc;
}

static int msm_lsm_start_lab_buffer(struct lsm_priv *prtd, uint16_t status)
{
	struct lsm_client *lsm_client = prtd->lsm_client;
	int rc = 0;

	if (lsm_client && lsm_client->lab_enable &&
	    !lsm_client->lab_started &&
	    status == LSM_VOICE_WAKEUP_STATUS_DETECTED) {
		atomic_set(&prtd->read_abort, 0);
		atomic_set(&prtd->buf_count, 0);
		prtd->appl_cnt = 0;
		prtd->dma_write = 0;
		prtd->xrun_count = 0;
		prtd->xrun_index = 0;

		rc = msm_lsm_queue_lab_buffer(prtd, 0);
		if (rc)
			pr_err("%s: Queue buffer failed for lab rc = %d\n",
				__func__, rc);
		else
			prtd->lsm_client->lab_started = true;
	}

	return rc;
}

static int msm_lsm_ioctl_shared(struct snd_pcm_substream *substream,
			 unsigned int cmd, void *arg)
{
	struct snd_soc_pcm_runtime *rtd;
	unsigned long flags = 0;
	int ret;
	struct snd_lsm_sound_model_v2 snd_model_v2;
	struct snd_lsm_session_data session_data;
	struct snd_lsm_session_data_v2 ses_data_v2 = {0};
	int rc = 0, stage_idx;
	int xchg = 0;
	struct snd_pcm_runtime *runtime;
	struct lsm_priv *prtd;
	struct snd_lsm_detection_params det_params;
	uint32_t max_detection_stages_supported = LSM_MAX_STAGES_PER_SESSION;

	if (!substream || !substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!substream) ? "substream" : "private_data");
		return -EINVAL;
	}

	runtime = substream->runtime;
	prtd = runtime->private_data;
	rtd = substream->private_data;

	dev_dbg(rtd->dev, "%s: enter, cmd %x\n", __func__, cmd);
	switch (cmd) {
	case SNDRV_LSM_SET_SESSION_DATA:
	case SNDRV_LSM_SET_SESSION_DATA_V2:

		if (cmd == SNDRV_LSM_SET_SESSION_DATA) {
			dev_dbg(rtd->dev, "%s: set session data\n", __func__);
			rc = copy_from_user(&session_data, arg, sizeof(session_data));
			if (!rc) {
				ses_data_v2.app_id = session_data.app_id;
				ses_data_v2.num_stages = 1;
			}
		} else {
			dev_dbg(rtd->dev, "%s: set session data_v2\n", __func__);
			rc = copy_from_user(&ses_data_v2, arg, sizeof(ses_data_v2));
		}
		if (rc) {
			dev_err(rtd->dev, "%s: %s: copy_from_user failed\n",
				__func__, "LSM_SET_SESSION_DATA(_V2)");
			return -EFAULT;
		}

		if (ses_data_v2.app_id != LSM_VOICE_WAKEUP_APP_ID_V2) {
			dev_err(rtd->dev,
				"%s:Invalid App id %d for Listen client\n",
			       __func__, ses_data_v2.app_id);
			rc = -EINVAL;
			break;
		}

		/*
		 * Before validating num_stages from user argument.
		 * Check ADSP support for multi-stage session,
		 * and reset max_detection_stages_supported to "1" if required.
		 */
		if (!q6lsm_adsp_supports_multi_stage_detection()) {
			dev_dbg(rtd->dev,
				"%s: multi-stage session not supported by adsp\n", __func__);
			max_detection_stages_supported = 1;
		}

		if (ses_data_v2.num_stages <= 0 ||
			ses_data_v2.num_stages > max_detection_stages_supported) {
			dev_err(rtd->dev,
				"%s: Unsupported number of stages req(%d)/max(%d)\n",
				 __func__, ses_data_v2.num_stages,
				max_detection_stages_supported);
			rc = -EINVAL;
			break;
		}

		prtd->lsm_client->app_id = ses_data_v2.app_id;
		prtd->lsm_client->num_stages = ses_data_v2.num_stages;
		for (stage_idx = LSM_STAGE_INDEX_FIRST;
			stage_idx < ses_data_v2.num_stages; stage_idx++) {
			prtd->lsm_client->stage_cfg[stage_idx].app_type =
				ses_data_v2.stage_info[stage_idx].app_type;
			prtd->lsm_client->stage_cfg[stage_idx].lpi_enable =
				ses_data_v2.stage_info[stage_idx].lpi_enable;
		}

		ret = q6lsm_open(prtd->lsm_client, ses_data_v2.app_id);
		if (ret < 0) {
			dev_err(rtd->dev,
				"%s: lsm open failed, %d\n",
				__func__, ret);
			return ret;
		}
		prtd->lsm_client->opened = true;
		dev_dbg(rtd->dev, "%s: Session_ID = %d, APP ID = %d, Num stages %d\n",
			__func__,
			prtd->lsm_client->session,
			prtd->lsm_client->app_id,
			prtd->lsm_client->num_stages);
		break;
	case SNDRV_LSM_REG_SND_MODEL_V2: {
		/*
		 * With multi-stage support sm buff allocation/free usage param info
		 * to check stage index for which this sound model is being set, and
		 * to check whether sm data is sent using set param command or not.
		 * Hence, set param ids to '0' to indicate allocation is for legacy
		 * reg_sm cmd, where buffer for param header need not be allocated,
		 * also set stage index to LSM_STAGE_INDEX_FIRST.
		 */
		struct lsm_params_info_v2 p_info = {0};
		struct lsm_sound_model *sm = NULL;
		p_info.stage_idx = LSM_STAGE_INDEX_FIRST;
		p_info.param_type = LSM_DEREG_SND_MODEL;
		sm = &prtd->lsm_client->stage_cfg[p_info.stage_idx].sound_model;

		dev_dbg(rtd->dev, "%s: Registering sound model V2\n",
			__func__);
		memcpy(&snd_model_v2, arg,
		       sizeof(struct snd_lsm_sound_model_v2));
		if (snd_model_v2.num_confidence_levels >
		    MAX_NUM_CONFIDENCE) {
			dev_err(rtd->dev,
				"%s: Invalid conf_levels = %d, maximum allowed = %d\n",
				__func__, snd_model_v2.num_confidence_levels,
				MAX_NUM_CONFIDENCE);
			rc = -EINVAL;
			break;
		}
		rc = q6lsm_snd_model_buf_alloc(prtd->lsm_client,
					snd_model_v2.data_size, &p_info, sm);
		if (rc) {
			dev_err(rtd->dev,
				"%s: q6lsm buffer alloc failed V2, size %d\n",
			       __func__, snd_model_v2.data_size);
			break;
		}
		if (copy_from_user(sm->data, snd_model_v2.data,
						   snd_model_v2.data_size)) {
			dev_err(rtd->dev,
				"%s: copy from user data failed\n"
			       "data %pK size %d\n", __func__,
			       snd_model_v2.data, snd_model_v2.data_size);
			q6lsm_snd_model_buf_free(prtd->lsm_client, &p_info, sm);
			rc = -EFAULT;
			break;
		}

		dev_dbg(rtd->dev, "SND Model Magic no byte[0] %x,\n"
			 "byte[1] %x, byte[2] %x byte[3] %x\n",
			 snd_model_v2.data[0], snd_model_v2.data[1],
			 snd_model_v2.data[2], snd_model_v2.data[3]);
		prtd->lsm_client->num_confidence_levels =
			snd_model_v2.num_confidence_levels;

		rc = msm_lsm_get_conf_levels(prtd->lsm_client,
				snd_model_v2.confidence_level);
		if (rc) {
			dev_err(rtd->dev,
				"%s: get_conf_levels failed, err = %d\n",
				__func__, rc);
			break;
		}

		rc = q6lsm_register_sound_model(prtd->lsm_client,
					snd_model_v2.detection_mode,
					snd_model_v2.detect_failure);
		if (rc < 0) {
			dev_err(rtd->dev,
				"%s: Register snd Model v2 failed =%d\n",
			       __func__, rc);
			q6lsm_snd_model_buf_free(prtd->lsm_client, &p_info, sm);
		}
		if (prtd->lsm_client->confidence_levels) {
			kfree(prtd->lsm_client->confidence_levels);
			prtd->lsm_client->confidence_levels = NULL;
		}
		break;
	}
	case SNDRV_LSM_SET_PARAMS:
		dev_dbg(rtd->dev, "%s: set_params\n", __func__);
		memcpy(&det_params, arg,
			sizeof(det_params));
		if (det_params.num_confidence_levels >
		    MAX_NUM_CONFIDENCE) {
			rc = -EINVAL;
			break;
		}

		prtd->lsm_client->num_confidence_levels =
			det_params.num_confidence_levels;

		rc = msm_lsm_get_conf_levels(prtd->lsm_client,
				det_params.conf_level);
		if (rc) {
			dev_err(rtd->dev,
				"%s: Failed to get conf_levels, err = %d\n",
				__func__, rc);
			break;
		}

		rc = q6lsm_set_data(prtd->lsm_client,
			       det_params.detect_mode,
			       det_params.detect_failure);
		if (rc)
			dev_err(rtd->dev,
				"%s: Failed to set params, err = %d\n",
				__func__, rc);
		if (prtd->lsm_client->confidence_levels) {
			kfree(prtd->lsm_client->confidence_levels);
			prtd->lsm_client->confidence_levels = NULL;
		}
		break;

	case SNDRV_LSM_DEREG_SND_MODEL:
		dev_dbg(rtd->dev, "%s: Deregistering sound model\n",
			__func__);
		rc = q6lsm_deregister_sound_model(prtd->lsm_client);
		if (rc)
			dev_err(rtd->dev,
				"%s: Sound model de-register failed, err = %d\n",
				__func__, rc);
		break;

	case SNDRV_LSM_EVENT_STATUS:
	case SNDRV_LSM_EVENT_STATUS_V3: {
		uint32_t ts_lsw, ts_msw;
		uint16_t status = 0, payload_size = 0;

		dev_dbg(rtd->dev, "%s: Get event status cmd %xx\n", __func__, cmd);
		atomic_set(&prtd->event_wait_stop, 0);

		/*
		 * Release the api lock before wait to allow
		 * other IOCTLs to be invoked while waiting
		 * for event
		 */
		mutex_unlock(&prtd->lsm_api_lock);
		rc = wait_event_freezable(prtd->event_wait,
				(cmpxchg(&prtd->event_avail, 1, 0) ||
				 (xchg = atomic_cmpxchg(&prtd->event_wait_stop,
							1, 0))));
		dev_dbg(rtd->dev, "%s: wait event is done\n", __func__);
		mutex_lock(&prtd->lsm_api_lock);
		dev_dbg(rtd->dev, "%s: wait_event_freezable %d event_wait_stop %d\n",
			 __func__, rc, xchg);
		if (!rc && !xchg) {
			dev_dbg(rtd->dev, "%s: New event available %ld\n",
				__func__, prtd->event_avail);
			spin_lock_irqsave(&prtd->event_lock, flags);

			if (prtd->event_status) {
				payload_size = prtd->event_status->payload_size;
				ts_lsw = prtd->event_status->timestamp_lsw;
				ts_msw = prtd->event_status->timestamp_msw;
				status = prtd->event_status->status;
				spin_unlock_irqrestore(&prtd->event_lock,
						       flags);
			} else {
				spin_unlock_irqrestore(&prtd->event_lock,
						       flags);
				rc = -EINVAL;
				dev_err(rtd->dev,
					"%s: prtd->event_status is NULL\n",
					__func__);
				break;
			}

			if (cmd == SNDRV_LSM_EVENT_STATUS) {
				struct snd_lsm_event_status *user = arg;

				if (user->payload_size < payload_size) {
					dev_dbg(rtd->dev,
						"%s: provided %d bytes isn't enough, needs %d bytes\n",
						__func__, user->payload_size,
						payload_size);
					rc = -ENOMEM;
				} else {
					user->status = status;
					user->payload_size = payload_size;
					memcpy(user->payload,
						prtd->event_status->payload,
						payload_size);
				}
			} else {
				struct snd_lsm_event_status_v3 *user_v3 = arg;

				if (user_v3->payload_size < payload_size) {
					dev_dbg(rtd->dev,
						"%s: provided %d bytes isn't enough, needs %d bytes\n",
						__func__, user_v3->payload_size,
						payload_size);
					rc = -ENOMEM;
				} else {
					user_v3->timestamp_lsw = ts_lsw;
					user_v3->timestamp_msw = ts_msw;
					user_v3->status = status;
					user_v3->payload_size = payload_size;
					memcpy(user_v3->payload,
						prtd->event_status->payload,
						payload_size);
				}
			}

			if (!rc)
				rc = msm_lsm_start_lab_buffer(prtd, status);
		} else if (xchg) {
			dev_dbg(rtd->dev, "%s: Wait aborted\n", __func__);
			rc = 0;
		}
		break;
	}

	case SNDRV_LSM_GENERIC_DET_EVENT: {
		struct snd_lsm_event_status *user = arg;
		uint16_t status = 0;
		uint16_t payload_size = 0;

		dev_dbg(rtd->dev,
			"%s: SNDRV_LSM_GENERIC_DET_EVENT\n", __func__);

		atomic_set(&prtd->event_wait_stop, 0);

		/*
		 * Release the api lock before wait to allow
		 * other IOCTLs to be invoked while waiting
		 * for event
		 */
		mutex_unlock(&prtd->lsm_api_lock);
		rc = wait_event_freezable(prtd->event_wait,
				(cmpxchg(&prtd->event_avail, 1, 0) ||
				(xchg = atomic_cmpxchg(&prtd->event_wait_stop,
							1, 0))));
		mutex_lock(&prtd->lsm_api_lock);

		dev_dbg(rtd->dev, "%s: wait_event_freezable %d event_wait_stop %d\n",
			 __func__, rc, xchg);

		if (!rc && !xchg) {
			dev_dbg(rtd->dev, "%s: %s: New event available %ld\n",
				__func__, "SNDRV_LSM_GENERIC_DET_EVENT",
				prtd->event_avail);

			spin_lock_irqsave(&prtd->event_lock, flags);

			if (prtd->det_event) {
				payload_size = prtd->det_event->payload_size;
				status = prtd->det_event->status;
				spin_unlock_irqrestore(&prtd->event_lock,
						       flags);
			} else {
				spin_unlock_irqrestore(&prtd->event_lock,
						       flags);
				dev_err(rtd->dev,
					"%s: %s: prtd->event_status is NULL\n",
					__func__,
					"SNDRV_LSM_GENERIC_DET_EVENT");
				rc = -EINVAL;
				break;
			}

			if (user->payload_size < payload_size) {
				dev_err(rtd->dev,
					"%s: provided %d bytes isn't enough, needs %d bytes\n",
					__func__, user->payload_size,
					payload_size);
				rc = -ENOMEM;
				break;
			}
			user->status = status;
			user->payload_size = payload_size;
			memcpy(user->payload, prtd->det_event->payload,
			       payload_size);

			rc = msm_lsm_start_lab_buffer(prtd, status);
		} else if (xchg) {
			dev_dbg(rtd->dev, "%s: %s: Wait aborted\n",
				__func__, "SNDRV_LSM_GENERIC_DET_EVENT");
			rc = 0;
		}
		break;
	}

	case SNDRV_LSM_ABORT_EVENT:
		dev_dbg(rtd->dev, "%s: Aborting event status wait\n",
			__func__);
		atomic_set(&prtd->event_wait_stop, 1);
		wake_up(&prtd->event_wait);
		break;

	case SNDRV_LSM_START:
		dev_dbg(rtd->dev, "%s: Starting LSM client session\n",
			__func__);
		if (!prtd->lsm_client->started) {
			rc = q6lsm_start(prtd->lsm_client, true);
			if (!rc) {
				prtd->lsm_client->started = true;
				dev_dbg(rtd->dev, "%s: LSM client session started\n",
					 __func__);
			}
		}
		break;

	case SNDRV_LSM_STOP: {
		dev_dbg(rtd->dev,
			"%s: Stopping LSM client session\n",
			__func__);
		if (prtd->lsm_client->started) {
			if (prtd->lsm_client->lab_enable) {
				atomic_set(&prtd->read_abort, 1);
				if (prtd->lsm_client->lab_started) {
					rc = q6lsm_stop_lab(prtd->lsm_client);
					if (rc)
						dev_err(rtd->dev,
							"%s: stop lab failed rc %d\n",
							__func__, rc);
					prtd->lsm_client->lab_started = false;
				}
			}

			if (!atomic_read(&prtd->read_abort)) {
				dev_dbg(rtd->dev,
					"%s: set read_abort to stop buffering\n", __func__);
				atomic_set(&prtd->read_abort, 1);
			}
			rc = q6lsm_stop(prtd->lsm_client, true);
			if (!rc)
				dev_dbg(rtd->dev,
					"%s: LSM client session stopped %d\n",
					__func__, rc);
			prtd->lsm_client->started = false;
		}
		break;
	}
	case SNDRV_LSM_LAB_CONTROL: {
		u32 enable = 0;
		struct lsm_params_info_v2 p_info = {0};

		if (prtd->lsm_client->num_stages > 1) {
			dev_err(rtd->dev, "%s: %s: not supported for multi stage session\n",
				__func__, "LSM_LAB_CONTROL");
			return -EINVAL;
		}

		if (copy_from_user(&enable, arg, sizeof(enable))) {
			dev_err(rtd->dev, "%s: %s: copy_frm_user failed\n",
				__func__, "LSM_LAB_CONTROL");
			return -EFAULT;
		}

		dev_dbg(rtd->dev, "%s: ioctl %s, enable = %d\n",
			 __func__, "SNDRV_LSM_LAB_CONTROL", enable);

		if (prtd->lsm_client->started) {
			dev_err(rtd->dev, "%s: ioctl %s issued after start",
				__func__, "SNDRV_LSM_LAB_CONTROL");
			rc = -EINVAL;
			break;
		}

		/*
		 * With multi-stage support lab control needs to set param info
		 * specifying stage index for which this lab control is issued,
		 * along with values of module/instance ids applicable for the stage.
		 * Hence, set param info with default lab module/instance ids, and
		 * set stage index to LSM_STAGE_INDEX_FIRST.
		 */
		p_info.param_type = LSM_LAB_CONTROL;
		p_info.module_id = LSM_MODULE_ID_LAB;
		p_info.instance_id = INSTANCE_ID_0;
		p_info.stage_idx = LSM_STAGE_INDEX_FIRST;
		p_info.param_size = 0;
		rc = msm_lsm_check_and_set_lab_controls(substream, enable, &p_info);
		break;
	}
	case SNDRV_LSM_STOP_LAB:
		dev_dbg(rtd->dev, "%s: stopping LAB\n", __func__);
		if (prtd->lsm_client->lab_enable &&
			prtd->lsm_client->lab_started) {
			atomic_set(&prtd->read_abort, 1);
			rc = q6lsm_stop_lab(prtd->lsm_client);
			if (rc)
				dev_err(rtd->dev,
					"%s: Lab stop failed for session %d rc %d\n",
					__func__,
					prtd->lsm_client->session, rc);
			prtd->lsm_client->lab_started = false;
		}
	break;

	case SNDRV_LSM_SET_PORT:
		dev_dbg(rtd->dev, "%s: set LSM port\n", __func__);
		rc = q6lsm_set_port_connected(prtd->lsm_client);
		break;

	case SNDRV_LSM_SET_FWK_MODE_CONFIG: {
		u32 mode;

		if (copy_from_user(&mode, arg, sizeof(mode))) {
			dev_err(rtd->dev, "%s: %s: copy_frm_user failed\n",
				__func__, "LSM_SET_FWK_MODE_CONFIG");
			return -EFAULT;
		}

		dev_dbg(rtd->dev, "%s: ioctl %s, enable = %d\n",
			__func__, "SNDRV_LSM_SET_FWK_MODE_CONFIG", mode);
		if (prtd->lsm_client->event_mode == mode) {
			dev_dbg(rtd->dev,
				"%s: mode for %d already set to %d\n",
				__func__, prtd->lsm_client->session, mode);
			rc = 0;
		} else {
			dev_dbg(rtd->dev, "%s: Event mode = %d\n",
				 __func__, mode);
			rc = q6lsm_set_fwk_mode_cfg(prtd->lsm_client, mode);
			if (!rc)
				prtd->lsm_client->event_mode = mode;
			else
				dev_err(rtd->dev,
					"%s: set event mode failed %d\n",
					__func__, rc);
		}
		break;
	}
	case SNDRV_LSM_SET_INPUT_HW_PARAMS: {
		struct lsm_hw_params *in_params;
		struct snd_lsm_input_hw_params params;

		if (copy_from_user(&params, arg, sizeof(params))) {
			dev_err(rtd->dev, "%s: %s: copy_from_user failed\n",
				__func__, "LSM_SET_INPUT_HW_PARAMS");
			return -EFAULT;
		}

		in_params = &prtd->lsm_client->in_hw_params;
		in_params->sample_rate = params.sample_rate;
		in_params->sample_size = params.bit_width;
		in_params->num_chs = params.num_channels;

		break;
	}

	default:
		dev_dbg(rtd->dev,
			"%s: Falling into default snd_lib_ioctl cmd 0x%x\n",
			 __func__, cmd);
		rc = snd_pcm_lib_ioctl(substream, cmd, arg);
		break;
	}

	if (!rc)
		dev_dbg(rtd->dev, "%s: leave (%d)\n",
			__func__, rc);
	else
		dev_err(rtd->dev, "%s: cmd 0x%x failed %d\n",
			__func__, cmd, rc);

	return rc;
}

static int msm_lsm_check_event_type(struct lsm_client *lsm_client,
				    unsigned int cmd)
{
	int err = 0;
	uint32_t event_type = lsm_client->event_type;

	if (cmd == SNDRV_LSM_EVENT_STATUS &&
	    event_type != LSM_DET_EVENT_TYPE_LEGACY) {
		pr_err("%s: %s: Invalid event request\n",
		       __func__, "SNDRV_LSM_EVENT_STATUS");
		err = -EINVAL;
	} else if (cmd == SNDRV_LSM_GENERIC_DET_EVENT &&
		   event_type != LSM_DET_EVENT_TYPE_GENERIC) {
		pr_err("%s: %s: Invalid event request\n",
		       __func__, "SNDRV_LSM_GENERIC_DET_EVENT");
		err = -EINVAL;
	}

	return err;
}

#ifdef CONFIG_COMPAT

struct snd_lsm_event_status32 {
	u16 status;
	u16 payload_size;
	u8 payload[0];
};

struct snd_lsm_event_status_v3_32 {
	u32 timestamp_lsw;
	u32 timestamp_msw;
	u16 status;
	u16 payload_size;
	u8 payload[0];
};

struct snd_lsm_sound_model_v2_32 {
	compat_uptr_t data;
	compat_uptr_t confidence_level;
	u32 data_size;
	enum lsm_detection_mode detection_mode;
	u8 num_confidence_levels;
	bool detect_failure;
};

struct snd_lsm_detection_params_32 {
	compat_uptr_t conf_level;
	enum lsm_detection_mode detect_mode;
	u8 num_confidence_levels;
	bool detect_failure;
};

struct lsm_params_info_32 {
	u32 module_id;
	u32 param_id;
	u32 param_size;
	compat_uptr_t param_data;
	uint32_t param_type;
};

struct lsm_params_info_v2_32 {
	u32 module_id;
	u32 param_id;
	u32 param_size;
	compat_uptr_t param_data;
	uint32_t param_type;
	u16 instance_id;
	u16 stage_idx;
	u32 model_id;
};

struct lsm_params_get_info_32 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
	uint32_t param_type;
	__u16 stage_idx;
	u8 payload[0];
} __packed;

struct snd_lsm_module_params_32 {
	compat_uptr_t params;
	u32 num_params;
	u32 data_size;
};

enum {
	SNDRV_LSM_REG_SND_MODEL_V2_32 =
		_IOW('U', 0x07, struct snd_lsm_sound_model_v2_32),
	SNDRV_LSM_SET_PARAMS_32 =
		_IOW('U', 0x0A, struct snd_lsm_detection_params_32),
	SNDRV_LSM_SET_MODULE_PARAMS_32 =
		_IOW('U', 0x0B, struct snd_lsm_module_params_32),
	SNDRV_LSM_EVENT_STATUS_V3_32 =
		_IOW('U', 0x0F, struct snd_lsm_event_status_v3_32),
	SNDRV_LSM_SET_MODULE_PARAMS_V2_32 =
		_IOW('U', 0x13, struct snd_lsm_module_params_32),
	SNDRV_LSM_GET_MODULE_PARAMS_32 =
		_IOWR('U', 0x14, struct lsm_params_get_info_32),
};

#if IS_ENABLED(CONFIG_AUDIO_QGKI)
static int msm_lsm_ioctl_compat(struct snd_soc_component *component,
				struct snd_pcm_substream *substream,
				unsigned int cmd, void __user *arg)
{
	struct snd_pcm_runtime *runtime;
	struct lsm_priv *prtd;
	struct snd_soc_pcm_runtime *rtd;
	int err = 0;
	u32 size = 0;

	if (!substream || !substream->runtime)
		return -ENXIO;

	if (!substream || !substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!substream) ? "substream" : "private_data");
		return -EINVAL;
	}
	runtime = substream->runtime;
	rtd = substream->private_data;
	prtd = runtime->private_data;

	mutex_lock(&prtd->lsm_api_lock);

	switch (cmd) {
	case SNDRV_LSM_EVENT_STATUS:
	case SNDRV_LSM_GENERIC_DET_EVENT: {
		struct snd_lsm_event_status userarg32, *user32 = NULL;
		struct snd_lsm_event_status *user = NULL;

		dev_dbg(rtd->dev,
			"%s: %s\n", __func__,
			(cmd == SNDRV_LSM_EVENT_STATUS) ?
			"SNDRV_LSM_EVENT_STATUS" :
			"SNDRV_LSM_GENERIC_DET_EVENT");

		err = msm_lsm_check_event_type(prtd->lsm_client, cmd);
		if (err)
			goto done;

		if (copy_from_user(&userarg32, arg, sizeof(userarg32))) {
			dev_err(rtd->dev, "%s: %s: Failed to copy from user\n",
				__func__, (cmd == SNDRV_LSM_EVENT_STATUS) ?
				"SNDRV_LSM_EVENT_STATUS" :
				"SNDRV_LSM_GENERIC_DET_EVENT");
			err = -EFAULT;
			goto done;
		}

		if (userarg32.payload_size >
		    LISTEN_MAX_STATUS_PAYLOAD_SIZE) {
			dev_err(rtd->dev,
				"%s: payload_size %d is invalid, max allowed = %d\n",
				__func__, userarg32.payload_size,
				LISTEN_MAX_STATUS_PAYLOAD_SIZE);
			err = -EINVAL;
			goto done;
		}

		size = sizeof(*user) + userarg32.payload_size;
		user = kzalloc(size, GFP_KERNEL);
		if (!user) {
			err = -ENOMEM;
			goto done;
		}

		user->payload_size = userarg32.payload_size;
		err = msm_lsm_ioctl_shared(substream, cmd, user);
		if (err) {
			dev_err(rtd->dev,
				"%s: msm_lsm_ioctl_shared() failed, err = %d",
				__func__, err);
			kfree(user);
			goto done;
		}

		/* Update size with actual payload size */
		size = sizeof(userarg32) + user->payload_size;
		if (!access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: Failed to verify write, size = %d\n",
				__func__, size);
			err = -EFAULT;
			kfree(user);
			goto done;
		}

		user32 = kzalloc(size, GFP_KERNEL);
		if (!user32) {
			err = -ENOMEM;
			kfree(user);
			goto done;
		}
		user32->status = user->status;
		user32->payload_size = user->payload_size;
		memcpy(user32->payload, user->payload,
		       user32->payload_size);

		if (copy_to_user(arg, user32, size)) {
			dev_err(rtd->dev,
				"%s: Failed to copy payload to user, size = %d",
				__func__, size);
			err = -EFAULT;
		}
		kfree(user);
		kfree(user32);
		break;
	}

	case SNDRV_LSM_EVENT_STATUS_V3_32: {
		struct snd_lsm_event_status_v3_32 userarg32, *user32 = NULL;
		struct snd_lsm_event_status_v3 *user = NULL;

		if (prtd->lsm_client->event_type !=
		    LSM_DET_EVENT_TYPE_LEGACY) {
			dev_err(rtd->dev,
				"%s: %s: Invalid event request\n",
				__func__, "SNDRV_LSM_EVENT_STATUS_V3_32");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&userarg32, arg, sizeof(userarg32))) {
			dev_err(rtd->dev, "%s: err copyuser ioctl %s\n",
				__func__, "SNDRV_LSM_EVENT_STATUS_V3_32");
			err = -EFAULT;
			goto done;
		}

		if (userarg32.payload_size >
		    LISTEN_MAX_STATUS_PAYLOAD_SIZE) {
			pr_err("%s: payload_size %d is invalid, max allowed = %d\n",
				__func__, userarg32.payload_size,
				LISTEN_MAX_STATUS_PAYLOAD_SIZE);
			err = -EINVAL;
			goto done;
		}

		size = sizeof(*user) + userarg32.payload_size;
		user = kzalloc(size, GFP_KERNEL);
		if (!user) {
			dev_err(rtd->dev,
				"%s: Allocation failed event status size %d\n",
				__func__, size);
			err = -ENOMEM;
			goto done;
		}
		cmd = SNDRV_LSM_EVENT_STATUS_V3;
		user->payload_size = userarg32.payload_size;
		err = msm_lsm_ioctl_shared(substream, cmd, user);

		/* Update size with actual payload size */
		size = sizeof(userarg32) + user->payload_size;
		if (!err && !access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: write verify failed size %d\n",
				__func__, size);
			err = -EFAULT;
		}
		if (!err) {
			user32 = kzalloc(size, GFP_KERNEL);
			if (!user32) {
				dev_err(rtd->dev,
					"%s: Allocation event user status size %d\n",
					__func__, size);
				err = -EFAULT;
			} else {
				user32->timestamp_lsw = user->timestamp_lsw;
				user32->timestamp_msw = user->timestamp_msw;
				user32->status = user->status;
				user32->payload_size = user->payload_size;
				memcpy(user32->payload,
				user->payload, user32->payload_size);
			}
		}
		if (!err && (copy_to_user(arg, user32, size))) {
			dev_err(rtd->dev, "%s: failed to copy payload %d",
				__func__, size);
			err = -EFAULT;
		}
		kfree(user);
		kfree(user32);
		if (err)
			dev_err(rtd->dev, "%s: lsmevent failed %d",
				__func__, err);
		break;
	}

	case SNDRV_LSM_REG_SND_MODEL_V2_32: {
		struct snd_lsm_sound_model_v2_32 snd_modelv232;
		struct snd_lsm_sound_model_v2 snd_modelv2;

		if (prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if using topology\n",
				__func__, "REG_SND_MODEL_V2");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&snd_modelv232, arg,
			sizeof(snd_modelv232))) {
			err = -EFAULT;
			dev_err(rtd->dev,
				"%s: copy user failed, size %zd %s\n",
				__func__,
				sizeof(struct snd_lsm_sound_model_v2_32),
				"SNDRV_LSM_REG_SND_MODEL_V2_32");
		} else {
			snd_modelv2.confidence_level =
			compat_ptr(snd_modelv232.confidence_level);
			snd_modelv2.data = compat_ptr(snd_modelv232.data);
			snd_modelv2.data_size = snd_modelv232.data_size;
			snd_modelv2.detect_failure =
			snd_modelv232.detect_failure;
			snd_modelv2.detection_mode =
			snd_modelv232.detection_mode;
			snd_modelv2.num_confidence_levels =
			snd_modelv232.num_confidence_levels;
			cmd = SNDRV_LSM_REG_SND_MODEL_V2;
			err = msm_lsm_ioctl_shared(substream, cmd,
				&snd_modelv2);
			if (err)
				dev_err(rtd->dev,
					"%s: ioctl %s failed\n", __func__,
					"SNDDRV_LSM_REG_SND_MODEL_V2_32");
		}
		break;
	}

	case SNDRV_LSM_SET_PARAMS_32:{
		struct snd_lsm_detection_params_32 det_params32;
		struct snd_lsm_detection_params det_params;

		if (prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if using topology\n",
				__func__, "SET_PARAMS_32");
			err = -EINVAL;
		}

		if (copy_from_user(&det_params32, arg,
				   sizeof(det_params32))) {
			err = -EFAULT;
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %zd\n",
				__func__, "SNDRV_LSM_SET_PARAMS_32",
				sizeof(det_params32));
		} else {
			det_params.conf_level =
				compat_ptr(det_params32.conf_level);
			det_params.detect_mode =
				det_params32.detect_mode;
			det_params.num_confidence_levels =
				det_params32.num_confidence_levels;
			det_params.detect_failure =
				det_params32.detect_failure;
			cmd = SNDRV_LSM_SET_PARAMS;
			err = msm_lsm_ioctl_shared(substream, cmd,
					&det_params);
			if (err)
				dev_err(rtd->dev,
					"%s: ioctl %s failed\n", __func__,
					"SNDRV_LSM_SET_PARAMS");
		}
		break;
	}

	case SNDRV_LSM_SET_MODULE_PARAMS_32:
	case SNDRV_LSM_SET_MODULE_PARAMS_V2_32: {
		struct snd_lsm_module_params_32 p_data_32;
		struct snd_lsm_module_params p_data;
		u8 *params32;
		size_t expected_size = 0, count;
		struct lsm_params_info_32 *p_info_32 = NULL;
		struct lsm_params_info_v2_32 *p_info_v2_32 = NULL;
		struct lsm_params_info_v2 p_info;

		if (!prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if not using topology\n",
				__func__, "SET_MODULE_PARAMS(_V2)_32");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&p_data_32, arg,
				   sizeof(p_data_32))) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %zd\n",
				__func__, "SET_MODULE_PARAMS(_V2)_32",
				sizeof(p_data_32));
			err = -EFAULT;
			goto done;
		}

		p_data.params = compat_ptr(p_data_32.params);
		p_data.num_params = p_data_32.num_params;
		p_data.data_size = p_data_32.data_size;

		if (p_data.num_params > LSM_PARAMS_MAX) {
			dev_err(rtd->dev,
				"%s: %s: Invalid num_params %d\n",
				__func__, "SET_MODULE_PARAMS(_V2)_32",
				p_data.num_params);
			err = -EINVAL;
			goto done;
		}

		expected_size = (cmd == SNDRV_LSM_SET_MODULE_PARAMS_32) ?
					p_data.num_params * sizeof(struct lsm_params_info_32) :
					p_data.num_params * sizeof(struct lsm_params_info_v2_32);

		if (p_data.data_size != expected_size) {
			dev_err(rtd->dev,
				"%s: %s: Invalid size %d, expected_size %d\n",
				__func__, "SET_MODULE_PARAMS(_V2)_32",
				p_data.data_size, expected_size);
			err = -EINVAL;
			goto done;
		}

		params32 = kzalloc(p_data.data_size, GFP_KERNEL);
		if (!params32) {
			err = -ENOMEM;
			goto done;
		}

		if (copy_from_user(params32, p_data.params,
				   p_data.data_size)) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %d\n",
				__func__, "params32", p_data.data_size);
			kfree(params32);
			err = -EFAULT;
			goto done;
		}

		if (cmd == SNDRV_LSM_SET_MODULE_PARAMS_32)
			p_info_32 = (struct lsm_params_info_32 *) params32;
		else
			p_info_v2_32 = (struct lsm_params_info_v2_32 *) params32;

		for (count = 0; count < p_data.num_params; count++) {
			if (cmd == SNDRV_LSM_SET_MODULE_PARAMS_32) {
				p_info.module_id = p_info_32->module_id;
				p_info.param_id = p_info_32->param_id;
				p_info.param_size = p_info_32->param_size;
				p_info.param_data = compat_ptr(p_info_32->param_data);
				p_info.param_type = p_info_32->param_type;

				p_info.instance_id = INSTANCE_ID_0;
				p_info.stage_idx = LSM_STAGE_INDEX_FIRST;
				p_info.model_id = 0;

				p_info_32++;
			} else {
				p_info.module_id = p_info_v2_32->module_id;
				p_info.param_id = p_info_v2_32->param_id;
				p_info.param_size = p_info_v2_32->param_size;
				p_info.param_data = compat_ptr(p_info_v2_32->param_data);
				p_info.param_type = p_info_v2_32->param_type;

				p_info.instance_id = p_info_v2_32->instance_id;
				p_info.stage_idx = p_info_v2_32->stage_idx;
				/* set sound model id to 0 for backward compatibility */
				p_info.model_id = 0;

				if (LSM_REG_MULTI_SND_MODEL == p_info_v2_32->param_type ||
				    LSM_DEREG_MULTI_SND_MODEL == p_info_v2_32->param_type ||
				    LSM_MULTI_SND_MODEL_CONFIDENCE_LEVELS ==
								p_info_v2_32->param_type)
					p_info.model_id = p_info_v2_32->model_id;

				p_info_v2_32++;
			}

			err = msm_lsm_process_params(substream, &p_info);
			if (err)
				dev_err(rtd->dev,
					"%s: Failed to process param, type%d stage=%d err=%d\n",
					__func__, p_info.param_type, p_info.stage_idx, err);
		}

		kfree(params32);
		break;
	}
	case SNDRV_LSM_GET_MODULE_PARAMS_32: {
		struct lsm_params_get_info_32 p_info_32, *param_info_rsp = NULL;
		struct lsm_params_get_info *p_info = NULL;
		prtd->lsm_client->get_param_payload = NULL;

		memset(&p_info_32, 0 , sizeof(p_info_32));
		if (!prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if not using topology\n",
				__func__, "GET_MODULE_PARAMS_32");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&p_info_32, arg, sizeof(p_info_32))) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %zd\n",
				__func__, "GET_MODULE_PARAMS_32",
				sizeof(p_info_32));
			err = -EFAULT;
			goto done;
		}
		size = sizeof(p_info_32);
		p_info = kzalloc(size, GFP_KERNEL);

		if (!p_info) {
			err = -ENOMEM;
			goto done;
		}

		p_info->module_id = p_info_32.module_id;
		p_info->param_id = p_info_32.param_id;
		p_info->param_size = p_info_32.param_size;
		p_info->param_type = p_info_32.param_type;
		p_info->instance_id = p_info_32.instance_id;
		p_info->stage_idx = p_info_32.stage_idx;

		prtd->lsm_client->get_param_payload = kzalloc(p_info_32.param_size,
							      GFP_KERNEL);
		if (!prtd->lsm_client->get_param_payload) {
			err = -ENOMEM;
			kfree(p_info);
			goto done;
		}
		prtd->lsm_client->param_size = p_info_32.param_size;

		err = q6lsm_get_one_param(prtd->lsm_client, p_info,
					  LSM_GET_CUSTOM_PARAMS);
		if (err) {
			dev_err(rtd->dev,
				"%s: Failed to get custom param, err=%d\n",
				__func__, err);
			kfree(p_info);
			kfree(prtd->lsm_client->get_param_payload);
			prtd->lsm_client->get_param_payload = NULL;
			goto done;
		}
		if (__builtin_uadd_overflow(sizeof(p_info_32), p_info_32.param_size, &size)) {
			pr_err("%s: param size exceeds limit of %u bytes.\n",
				__func__, UINT_MAX);
			err = -EINVAL;
			goto done;
		}

		param_info_rsp = kzalloc(size, GFP_KERNEL);

		if (!param_info_rsp) {
			err = -ENOMEM;
			kfree(p_info);
			kfree(prtd->lsm_client->get_param_payload);
			prtd->lsm_client->get_param_payload = NULL;
			goto done;
		}

		if (!access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: Failed to verify write, size = %d\n",
				__func__, size);
			err = -EFAULT;
			goto free;
		}

		memcpy(param_info_rsp, &p_info_32, sizeof(p_info_32));
		memcpy(param_info_rsp->payload, prtd->lsm_client->get_param_payload,
			p_info_32.param_size);

		if (copy_to_user(arg, param_info_rsp, size)) {
			dev_err(rtd->dev, "%s: Failed to copy payload to user, size = %d\n",
				__func__, size);
			err = -EFAULT;
		}
free:
		kfree(p_info);
		kfree(param_info_rsp);
		kfree(prtd->lsm_client->get_param_payload);
		prtd->lsm_client->get_param_payload = NULL;
		break;
	}
	case SNDRV_LSM_REG_SND_MODEL_V2:
	case SNDRV_LSM_SET_PARAMS:
	case SNDRV_LSM_SET_MODULE_PARAMS:
	case SNDRV_LSM_SET_MODULE_PARAMS_V2:
		/*
		 * In ideal cases, the compat_ioctl should never be called
		 * with the above unlocked ioctl commands. Print error
		 * and return error if it does.
		 */
		dev_err(rtd->dev,
			"%s: Invalid cmd for compat_ioctl\n",
			__func__);
		err = -EINVAL;
		break;
	default:
		err = msm_lsm_ioctl_shared(substream, cmd, arg);
		break;
	}
done:
	mutex_unlock(&prtd->lsm_api_lock);
	return err;
}
#endif /* CONFIG_AUDIO_QGKI */
#else
#define msm_lsm_ioctl_compat NULL
#endif

static int msm_lsm_ioctl(struct snd_soc_component *component, struct snd_pcm_substream *substream,
			 unsigned int cmd, void __user *arg)
{
	int err = 0;
	u32 size = 0;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_priv *prtd;

	if (!substream || !substream->private_data) {
		pr_err("%s: Invalid %s\n", __func__,
			(!substream) ? "substream" : "private_data");
		return -EINVAL;
	}
	runtime = substream->runtime;
	prtd = runtime->private_data;
	rtd = substream->private_data;

	mutex_lock(&prtd->lsm_api_lock);
	switch (cmd) {
	case SNDRV_LSM_REG_SND_MODEL_V2: {
		struct snd_lsm_sound_model_v2 snd_model_v2;

		if (prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if using topology\n",
				__func__, "REG_SND_MODEL_V2");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&snd_model_v2, arg, sizeof(snd_model_v2))) {
			err = -EFAULT;
			dev_err(rtd->dev,
				"%s: copy from user failed, size %zd\n",
				__func__,
				sizeof(struct snd_lsm_sound_model_v2));
		}
		if (!err)
			err = msm_lsm_ioctl_shared(substream, cmd,
						   &snd_model_v2);
		if (err)
			dev_err(rtd->dev,
				"%s REG_SND_MODEL failed err %d\n",
				__func__, err);
		goto done;
		}
		break;
	case SNDRV_LSM_SET_PARAMS: {
		struct snd_lsm_detection_params det_params;

		if (prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if using topology\n",
				__func__, "SET_PARAMS");
			err = -EINVAL;
			goto done;
		}

		pr_debug("%s: SNDRV_LSM_SET_PARAMS\n", __func__);

		if (copy_from_user(&det_params, arg,
				   sizeof(det_params))) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size %zd\n",
				__func__, "SNDRV_LSM_SET_PARAMS",
				sizeof(det_params));
			err = -EFAULT;
		}

		if (!err)
			err = msm_lsm_ioctl_shared(substream, cmd,
						   &det_params);
		else
			dev_err(rtd->dev,
				"%s: LSM_SET_PARAMS failed, err %d\n",
				__func__, err);

		goto done;
	}

	case SNDRV_LSM_SET_MODULE_PARAMS:
	case SNDRV_LSM_SET_MODULE_PARAMS_V2: {
		struct snd_lsm_module_params p_data;
		struct lsm_params_info *temp_ptr_info = NULL;
		struct lsm_params_info_v2 info_v2;
		struct lsm_params_info_v2 *ptr_info_v2 = NULL, *temp_ptr_info_v2 = NULL;
		size_t p_size = 0, count;
		u8 *params;

		memset(&info_v2, 0, sizeof(info_v2));

		if (!prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if not using topology\n",
				__func__, "SET_MODULE_PARAMS(_V2)");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&p_data, arg,
				   sizeof(p_data))) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %zd\n",
				__func__, "p_data", sizeof(p_data));
			err = -EFAULT;
			goto done;
		}

		if (p_data.num_params > LSM_PARAMS_MAX) {
			dev_err(rtd->dev,
				"%s: %s: Invalid num_params %d\n",
				__func__, "SET_MODULE_PARAMS(_V2)",
				p_data.num_params);
			err = -EINVAL;
			goto done;
		}

		if (cmd == SNDRV_LSM_SET_MODULE_PARAMS)
			p_size = p_data.num_params * sizeof(struct lsm_params_info);
		else
			p_size = p_data.num_params * sizeof(struct lsm_params_info_v2);

		if (p_data.data_size != p_size) {
			dev_err(rtd->dev,
				"%s: %s: Invalid data_size(%u) against expected(%zd)\n",
				__func__, "SET_MODULE_PARAMS(_V2)",
				p_data.data_size, p_size);
			err = -EFAULT;
			goto done;
		}

		params = kzalloc(p_size, GFP_KERNEL);
		if (!params) {
			err = -ENOMEM;
			goto done;
		}

		if (copy_from_user(params, p_data.params,
				  p_data.data_size)) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %d\n",
				__func__, "set module params", p_data.data_size);
			kfree(params);
			err = -EFAULT;
			goto done;
		}

		if (cmd == SNDRV_LSM_SET_MODULE_PARAMS)
			temp_ptr_info = (struct lsm_params_info *)params;
		else
			temp_ptr_info_v2 = (struct lsm_params_info_v2 *)params;

		for (count = 0; count < p_data.num_params; count++) {
			if (cmd == SNDRV_LSM_SET_MODULE_PARAMS) {
				/* convert to V2 param info struct from legacy param info */
				info_v2.module_id = temp_ptr_info->module_id;
				info_v2.param_id = temp_ptr_info->param_id;
				info_v2.param_size = temp_ptr_info->param_size;
				info_v2.param_data = temp_ptr_info->param_data;
				info_v2.param_type = temp_ptr_info->param_type;

				info_v2.instance_id = INSTANCE_ID_0;
				info_v2.stage_idx = LSM_STAGE_INDEX_FIRST;
				info_v2.model_id = 0;

				ptr_info_v2 = &info_v2;
				temp_ptr_info++;
			} else {
				if (LSM_REG_MULTI_SND_MODEL != temp_ptr_info_v2->param_type ||
				    LSM_DEREG_MULTI_SND_MODEL !=
								temp_ptr_info_v2->param_type ||
				    LSM_MULTI_SND_MODEL_CONFIDENCE_LEVELS !=
								temp_ptr_info_v2->param_type) {
					/* set sound model id to 0 for backward compatibility */
					temp_ptr_info_v2->model_id = 0;
				}
				/*
				 * Just copy the pointer as user
				 * already provided sound model id
				 */
				ptr_info_v2 = temp_ptr_info_v2;
				temp_ptr_info_v2++;
			}
			err = msm_lsm_process_params(substream, ptr_info_v2);
			if (err)
				dev_err(rtd->dev,
					"%s: Failed to process param, type=%d stage=%d err=%d\n",
					__func__, ptr_info_v2->param_type,
					ptr_info_v2->stage_idx, err);
		}
		kfree(params);
		break;
	}

	case SNDRV_LSM_GET_MODULE_PARAMS: {
		struct lsm_params_get_info temp_p_info, *p_info = NULL;
		prtd->lsm_client->get_param_payload = NULL;

		memset(&temp_p_info, 0, sizeof(temp_p_info));
		if (!prtd->lsm_client->use_topology) {
			dev_err(rtd->dev,
				"%s: %s: not supported if not using topology\n",
				__func__, "GET_MODULE_PARAMS_32");
			err = -EINVAL;
			goto done;
		}

		if (copy_from_user(&temp_p_info, arg, sizeof(temp_p_info))) {
			dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %zd\n",
				__func__, "GET_MODULE_PARAMS_32",
				sizeof(temp_p_info));
			err = -EFAULT;
			goto done;
		}

		if (temp_p_info.param_size > 0 &&
			((INT_MAX - sizeof(temp_p_info)) <
				temp_p_info.param_size)) {
			pr_err("%s: Integer overflow\n", __func__);
			err = -EINVAL;
			goto done;
		}

		size = sizeof(temp_p_info) +  temp_p_info.param_size;
		p_info = kzalloc(size, GFP_KERNEL);

		if (!p_info) {
			err = -ENOMEM;
			goto done;
		}

		p_info->module_id = temp_p_info.module_id;
		p_info->param_id = temp_p_info.param_id;
		p_info->param_size = temp_p_info.param_size;
		p_info->param_type = temp_p_info.param_type;
		p_info->instance_id = temp_p_info.instance_id;
		p_info->stage_idx = temp_p_info.stage_idx;

		prtd->lsm_client->get_param_payload = kzalloc(temp_p_info.param_size,
							      GFP_KERNEL);
		if (!prtd->lsm_client->get_param_payload) {
			err = -ENOMEM;
			kfree(p_info);
			goto done;
		}

		prtd->lsm_client->param_size =  p_info->param_size;
		err = q6lsm_get_one_param(prtd->lsm_client, p_info,
					  LSM_GET_CUSTOM_PARAMS);
		if (err) {
			dev_err(rtd->dev,
				"%s: Failed to get custom param, err=%d\n",
				__func__, err);
			goto free;
		}

		if (!access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: Failed to verify write, size = %d\n",
				__func__, size);
			err = -EFAULT;
			goto free;
		}

		memcpy(p_info->payload, prtd->lsm_client->get_param_payload,
			temp_p_info.param_size);

		if (copy_to_user(arg, p_info, sizeof(struct lsm_params_get_info) +
				 p_info->param_size)) {
			dev_err(rtd->dev, "%s: Failed to copy payload to user, size = %d\n",
				__func__, size);
			err = -EFAULT;
		}
free:
		kfree(p_info);
		kfree(prtd->lsm_client->get_param_payload);
		prtd->lsm_client->get_param_payload = NULL;
		break;
	}
	case SNDRV_LSM_EVENT_STATUS:
	case SNDRV_LSM_GENERIC_DET_EVENT: {
		struct snd_lsm_event_status *user = NULL;
		struct snd_lsm_event_status userarg;

		dev_dbg(rtd->dev,
			"%s: %s\n", __func__,
			(cmd == SNDRV_LSM_EVENT_STATUS) ?
			"SNDRV_LSM_EVENT_STATUS" :
			"SNDRV_LSM_GENERIC_DET_EVENT");

		err = msm_lsm_check_event_type(prtd->lsm_client, cmd);
		if (err)
			goto done;

		if (copy_from_user(&userarg, arg, sizeof(userarg))) {
			dev_err(rtd->dev,
				"%s: %s: Copy from user failed\n", __func__,
				(cmd == SNDRV_LSM_EVENT_STATUS) ?
				"SNDRV_LSM_EVENT_STATUS" :
				"SNDRV_LSM_GENERIC_DET_EVENT");
			err = -EFAULT;
			goto done;
		}

		if (userarg.payload_size >
		    LISTEN_MAX_STATUS_PAYLOAD_SIZE) {
			dev_err(rtd->dev,
				"%s: payload_size %d is invalid, max allowed = %d\n",
				__func__, userarg.payload_size,
				LISTEN_MAX_STATUS_PAYLOAD_SIZE);
			err = -EINVAL;
			goto done;
		}

		size = sizeof(struct snd_lsm_event_status) +
			userarg.payload_size;
		user = kzalloc(size, GFP_KERNEL);
		if (!user) {
			err = -ENOMEM;
			goto done;
		}

		user->payload_size = userarg.payload_size;
		err = msm_lsm_ioctl_shared(substream, cmd, user);
		if (err) {
			dev_err(rtd->dev,
				"%s: msm_lsm_ioctl_shared() failed, err = %d",
				__func__, err);
			kfree(user);
			goto done;
		}

		/* Update size with actual payload size */
		size = sizeof(*user) + user->payload_size;
		if (!access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: Failed to verify write, size = %d\n",
				__func__, size);
			err = -EFAULT;
		}
		if (!err && copy_to_user(arg, user, size)) {
			dev_err(rtd->dev,
				"%s: Failed to copy payload to user, size = %d\n",
				__func__, size);
			err = -EFAULT;
		}

		kfree(user);
		break;
	}

	case SNDRV_LSM_EVENT_STATUS_V3: {
		struct snd_lsm_event_status_v3 *user = NULL;
		struct snd_lsm_event_status_v3 userarg;

		dev_dbg(rtd->dev,
			"%s: SNDRV_LSM_EVENT_STATUS_V3\n", __func__);

		if (prtd->lsm_client->event_type !=
		    LSM_DET_EVENT_TYPE_LEGACY) {
			dev_err(rtd->dev,
				"%s: %s: Invalid event request\n",
				__func__, "SNDRV_LSM_EVENT_STATUS_V3");
			err = -EINVAL;
			goto done;
		}

		if (!arg) {
			dev_err(rtd->dev,
				"%s: Invalid params event_status_v3\n",
				__func__);
			err = -EINVAL;
			goto done;
		}
		if (copy_from_user(&userarg, arg, sizeof(userarg))) {
			dev_err(rtd->dev,
				"%s: err copyuser event_status_v3\n",
				__func__);
			err = -EFAULT;
			goto done;
		}

		if (userarg.payload_size >
		    LISTEN_MAX_STATUS_PAYLOAD_SIZE) {
			pr_err("%s: payload_size %d is invalid, max allowed = %d\n",
				__func__, userarg.payload_size,
				LISTEN_MAX_STATUS_PAYLOAD_SIZE);
			err = -EINVAL;
			goto done;
		}

		size = sizeof(struct snd_lsm_event_status_v3) +
			userarg.payload_size;
		user = kzalloc(size, GFP_KERNEL);
		if (!user) {
			dev_err(rtd->dev,
				"%s: Allocation failed event status size %d\n",
				__func__, size);
			err = -EFAULT;
			goto done;
		}
		user->payload_size = userarg.payload_size;
		err = msm_lsm_ioctl_shared(substream, cmd, user);

		/* Update size with actual payload size */
		size = sizeof(*user) + user->payload_size;
		if (!err && !access_ok(arg, size)) {
			dev_err(rtd->dev,
				"%s: write verify failed size %d\n",
				__func__, size);
			err = -EFAULT;
		}
		if (!err && (copy_to_user(arg, user, size))) {
			dev_err(rtd->dev,
				"%s: failed to copy payload %d",
				__func__, size);
			err = -EFAULT;
		}
		kfree(user);
		if (err)
			dev_err(rtd->dev,
				"%s: lsm_event_v3 failed %d", __func__, err);
		break;
	}

	default:
		err = msm_lsm_ioctl_shared(substream, cmd, arg);
	break;
	}
done:
	mutex_unlock(&prtd->lsm_api_lock);
	return err;
}

#ifdef ENABLE_SVA_MIXER_CTL

static int msm_lsm_cdev_session_lut(struct snd_pcm_substream *substream, uint8_t operation)
{

	uint8_t i = 0;
	int ret = 0;

	if (!substream)
		return -EINVAL;

	switch (operation)
	{
		case SET_INFO:
			for (i = 0; i < LSM_MAX_SESSION_COUNT; i++)
				if (sub_str_info[i].pcm == NULL)
					memcpy(&sub_str_info[i], substream,
							sizeof(struct snd_pcm_substream));
				else
					ret = -ENODEV;
			break;
		case GET_INFO:
			for (i = 0; i < LSM_MAX_SESSION_COUNT; i++)
			{
				if (sub_str_info[i].pcm == NULL)
					continue;

				if (substream->number == sub_str_info[i].pcm->device)
				{
					memcpy(substream, &sub_str_info[i],
							sizeof(struct snd_pcm_substream));
					ret = 0;
					break;
				}
				else
				{
					ret = -ENODEV;
				}
			}
			break;
		case CLEAR_INFO:
			for (i = 0; i < LSM_MAX_SESSION_COUNT; i++)
			{
				if (sub_str_info[i].pcm == NULL)
					continue;

				if (substream->number == sub_str_info[i].pcm->device)
				{
					memset(&sub_str_info[i], 0,
							sizeof(struct snd_pcm_substream));
					ret = 0;
					break;
				}
				else
				{
					ret = -ENODEV;
				}
			}
			break;
		default:
			ret = -EINVAL;
	}
	return ret;
}
#endif

static int msm_lsm_open(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret = 0, i;

	pr_debug("%s\n", __func__);
	prtd = kzalloc(sizeof(struct lsm_priv), GFP_KERNEL);
	if (!prtd) {
		pr_err("%s: Failed to allocate memory for lsm_priv\n",
		       __func__);
		return -ENOMEM;
	}
	mutex_init(&prtd->lsm_api_lock);
	spin_lock_init(&prtd->event_lock);
	spin_lock_init(&prtd->xrun_lock);
	init_waitqueue_head(&prtd->event_wait);
	init_waitqueue_head(&prtd->period_wait);
	prtd->substream = substream;
	runtime->private_data = prtd;
	runtime->hw = msm_pcm_hardware_capture;

	ret = snd_pcm_hw_constraint_list(runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&constraints_sample_rates);
	if (ret < 0)
		pr_info("%s: snd_pcm_hw_constraint_list failed ret %d\n",
			 __func__, ret);
	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
			    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		pr_info("%s: snd_pcm_hw_constraint_integer failed ret %d\n",
			__func__, ret);

	ret = snd_pcm_hw_constraint_minmax(runtime,
		SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
		CAPTURE_MIN_NUM_PERIODS * CAPTURE_MIN_PERIOD_SIZE,
		CAPTURE_MAX_NUM_PERIODS * CAPTURE_MAX_PERIOD_SIZE);
	if (ret < 0)
		pr_info("%s: constraint for buffer bytes min max ret = %d\n",
			__func__, ret);
	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
	if (ret < 0) {
		pr_info("%s: constraint for period bytes step ret = %d\n",
			__func__, ret);
	}
	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 32);
	if (ret < 0)
		pr_info("%s: constraint for buffer bytes step ret = %d\n",
			__func__, ret);
	prtd->lsm_client = q6lsm_client_alloc(
				lsm_event_handler, prtd);
	if (!prtd->lsm_client) {
		pr_err("%s: Could not allocate memory\n", __func__);
		kfree(prtd);
		runtime->private_data = NULL;
		return -ENOMEM;
	}
	prtd->lsm_client->opened = false;
	prtd->lsm_client->started = false;
	prtd->lsm_client->session_state = IDLE;
	prtd->lsm_client->poll_enable = true;
	prtd->lsm_client->perf_mode = 0;
	prtd->lsm_client->event_mode = LSM_EVENT_NON_TIME_STAMP_MODE;
	prtd->lsm_client->event_type = LSM_DET_EVENT_TYPE_LEGACY;
	prtd->lsm_client->fe_id = rtd->dai_link->id;
	prtd->lsm_client->unprocessed_data = 0;
	prtd->lsm_client->num_sound_models = 0;
	prtd->lsm_client->num_keywords = 0;
	prtd->lsm_client->multi_snd_model_confidence_levels = NULL;

	for (i = 0; i < LSM_MAX_STAGES_PER_SESSION; i++)
		INIT_LIST_HEAD(&prtd->lsm_client->stage_cfg[i].sound_models);


#ifdef ENABLE_SVA_MIXER_CTL
	msm_lsm_cdev_session_lut(substream, SET_INFO);
#endif

	return 0;
}

static int msm_lsm_send_ch_mix_config(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_hw_params *in_params;
	int pp_ch_cnt;
	int *ch_wght_coeff;
	int ret = 0, i, idx;

	/*
	 * The output channels from channel mixer is the input to LSM (stream)
	 * side and is read from in_params->num_chs.
	 *
	 * The input channels to channel mixer are the output channels from
	 * the device side (routing) and is obtained by reading the
	 * pp_ch_cnt.
	 *
	 * For LSM to be functional, only unity channel mixing is allowed.
	 */

	in_params = &prtd->lsm_client->in_hw_params;
	rtd = prtd->substream->private_data;
	pp_ch_cnt = msm_pcm_routing_get_pp_ch_cnt(rtd->dai_link->id,
						  SESSION_TYPE_TX);
	if (pp_ch_cnt < 0 ||
	    pp_ch_cnt > LSM_V3P0_MAX_NUM_CHANNELS ||
	     in_params->num_chs > LSM_V3P0_MAX_NUM_CHANNELS) {
		dev_err(rtd->dev,
			"%s: invalid ch cnt, pp_ch_cnt %d in_ch_cnt %d\n",
			__func__, pp_ch_cnt, in_params->num_chs);
		return -EINVAL;
	}

	if (!pp_ch_cnt ||
	    (pp_ch_cnt == in_params->num_chs)) {
		dev_dbg(rtd->dev,
			"%s: Skip ch mixing, pp_ch_cnt %d in_ch_cnt %d\n",
			__func__, pp_ch_cnt, in_params->num_chs);
		return 0;
	}

	ch_wght_coeff = kzalloc(in_params->num_chs * pp_ch_cnt * sizeof(int),
				GFP_KERNEL);
	if (!ch_wght_coeff)
		return -ENOMEM;

	/*
	 * channel weight co-efficients is a m X n array, where
	 * m = number of input channels to ch mixer (pp_ch_cnt)
	 * n = number of output channels from ch mixer (in_params->num_chs)
	 */
	for (i = 0; i < in_params->num_chs; i++) {
		idx = (i * pp_ch_cnt) + i;
		ch_wght_coeff[idx] = 1;
	}

	ret = msm_pcm_routing_send_chmix_cfg(rtd->dai_link->id,
					     pp_ch_cnt, in_params->num_chs,
					     ch_wght_coeff,
					     SESSION_TYPE_TX, STREAM_TYPE_LSM);
	if (ret)
		dev_err(rtd->dev,
			"%s: Failed to configure channel mixer err %d\n",
			__func__, ret);

	kfree(ch_wght_coeff);

	return ret;
}

static int msm_lsm_prepare(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd;
	int ret = 0;

	if (!substream->private_data) {
		pr_err("%s: Invalid private_data", __func__);
		return -EINVAL;
	}

	rtd = prtd->substream->private_data;

	if (!prtd->lsm_client) {
		dev_err(rtd->dev,
			"%s: LSM client data ptr is NULL\n", __func__);
		return -EINVAL;
	}

	if (q6lsm_set_media_fmt_v2_params(prtd->lsm_client))
		dev_dbg(rtd->dev,
			"%s: failed to set lsm media fmt params\n", __func__);

	if (prtd->lsm_client->session_state == IDLE) {
		ret = msm_pcm_routing_reg_phy_compr_stream(
				rtd->dai_link->id,
				prtd->lsm_client->perf_mode,
				prtd->lsm_client->session,
				SNDRV_PCM_STREAM_CAPTURE,
				LISTEN);
		if (ret) {
			dev_err(rtd->dev,
				"%s: register phy compr stream failed %d\n",
					__func__, ret);
			return ret;
		}

		ret = msm_lsm_send_ch_mix_config(substream);
		if (ret) {
			msm_pcm_routing_dereg_phy_stream(rtd->dai_link->id,
					SNDRV_PCM_STREAM_CAPTURE);
			return ret;
		}
	}

	prtd->lsm_client->session_state = RUNNING;
	runtime->private_data = prtd;
	return ret;
}

static int msm_lsm_close(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = NULL;
	struct snd_soc_pcm_runtime *rtd;
	struct msm_pcm_stream_app_type_cfg cfg_data = {0};
	struct lsm_char_dev *lsm_dev;
	int ret = 0;
	int be_id = 0;
	int fe_id = 0;

	if (!substream->private_data) {
		pr_err("%s: Invalid private_data", __func__);
		return -EINVAL;
	}
	if (!component || !component->dev) {
		pr_err("%s: Invalid component\n", __func__);
		return -EINVAL;
	}
	rtd = substream->private_data;
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	dev_dbg(rtd->dev, "%s\n", __func__);
	if (prtd->lsm_client->started) {
		if (prtd->lsm_client->lab_enable) {
			atomic_set(&prtd->read_abort, 1);
			if (prtd->lsm_client->lab_started) {
				ret = q6lsm_stop_lab(prtd->lsm_client);
				if (ret)
					dev_err(rtd->dev,
						"%s: stop lab failed ret %d\n",
						__func__, ret);
				prtd->lsm_client->lab_started = false;
			}
		}

		if (!atomic_read(&prtd->read_abort)) {
			dev_dbg(rtd->dev,
				"%s: set read_abort to stop buffering\n", __func__);
			atomic_set(&prtd->read_abort, 1);
		}
		ret = q6lsm_stop(prtd->lsm_client, true);
		if (ret)
			dev_err(rtd->dev,
				"%s: session stop failed, err = %d\n",
				__func__, ret);
		else
			dev_dbg(rtd->dev,
				"%s: LSM client session stopped %d\n",
				 __func__, ret);

		prtd->lsm_client->started = false;
	}

	if (prtd->lsm_client->lab_enable && prtd->lsm_client->lab_buffer) {
		ret = msm_lsm_lab_buffer_alloc(prtd,
				LAB_BUFFER_DEALLOC);
		if (ret)
			dev_err(rtd->dev,
				"%s: lab buffer dealloc failed ret %d\n",
				__func__, ret);
	}

	/*
	 * De-register existing sound models
	 * to free SM and CAL buffer, even if
	 * lsm client is not started.
	 */
	ret = q6lsm_deregister_sound_model(prtd->lsm_client);
	if (ret)
		dev_err(rtd->dev, "%s: dereg_snd_model failed, err = %d\n",
			__func__, ret);
	else
		dev_dbg(rtd->dev, "%s: dereg_snd_model successful\n",
			__func__);

	msm_pcm_routing_dereg_phy_stream(rtd->dai_link->id,
					SNDRV_PCM_STREAM_CAPTURE);

	if (prtd->lsm_client->opened) {
		if (!atomic_read(&prtd->read_abort)) {
			dev_dbg(rtd->dev,
				"%s: set read_abort to stop buffering\n", __func__);
			atomic_set(&prtd->read_abort, 1);
		}
		q6lsm_close(prtd->lsm_client);
		prtd->lsm_client->opened = false;
	}

	fe_id = prtd->lsm_client->fe_id;
	ret = msm_pcm_routing_get_stream_app_type_cfg(fe_id, SESSION_TYPE_TX,
						      &be_id, &cfg_data);
	if (ret < 0)
		dev_dbg(rtd->dev,
			"%s: get stream app type cfg failed, err = %d\n",
			__func__, ret);
	/*
	 * be_id will be 0 in case of LSM directly connects to AFE due to
	 * last_be_id_configured[fedai_id][session_type] has not been updated.
	 * And then the cfg_data from wrong combination would be reset without
	 * this if check. We reset only if app_type, acdb_dev_id, and sample_rate
	 * are valid.
	 */
	if (!cfg_data.app_type &&
	    !cfg_data.acdb_dev_id && !cfg_data.sample_rate) {
		dev_dbg(rtd->dev, "%s: no need to reset app type configs\n",
			__func__);
	} else {
		memset(&cfg_data, 0, sizeof(cfg_data));
		ret = msm_pcm_routing_reg_stream_app_type_cfg(fe_id,
							      SESSION_TYPE_TX,
							      be_id,
							      &cfg_data);
		if (ret < 0)
			dev_dbg(rtd->dev,
				"%s: set stream app type cfg failed, err = %d\n",
				__func__, ret);
	}

	q6lsm_client_free(prtd->lsm_client);

	spin_lock_irqsave(&prtd->event_lock, flags);
	kfree(prtd->event_status);
	prtd->event_status = NULL;
	kfree(prtd->det_event);
	prtd->det_event = NULL;
	spin_unlock_irqrestore(&prtd->event_lock, flags);
	mutex_destroy(&prtd->lsm_api_lock);
	kfree(prtd);
	runtime->private_data = NULL;
	mutex_unlock(&lsm_dev->lock);

	return 0;
}

static int msm_lsm_hw_params(struct snd_soc_component *component,
				struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = NULL;
	struct lsm_hw_params *out_hw_params = NULL;
	struct lsm_hw_params *in_hw_params = NULL;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_char_dev *lsm_dev = NULL;

	if (!substream->private_data) {
		pr_err("%s: Invalid private_data", __func__);
		return -EINVAL;
	}
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: Invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}
	mutex_lock(&lsm_dev->lock);
	prtd = runtime->private_data;

	if (!prtd || !params) {
		dev_err(rtd->dev,
			"%s: invalid params prtd %pK params %pK",
			 __func__, prtd, params);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	in_hw_params = &prtd->lsm_client->in_hw_params;
	out_hw_params = &prtd->lsm_client->out_hw_params;
	out_hw_params->num_chs = params_channels(params);
	out_hw_params->period_count = params_periods(params);
	out_hw_params->sample_rate = params_rate(params);
	if (((out_hw_params->sample_rate != 16000) &&
		(out_hw_params->sample_rate != 48000)) ||
		(out_hw_params->period_count == 0)) {
		dev_err(rtd->dev,
			"%s: Invalid Params sample rate %d period count %d\n",
			__func__, out_hw_params->sample_rate,
			out_hw_params->period_count);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	if (params_format(params) == SNDRV_PCM_FORMAT_S16_LE) {
		out_hw_params->sample_size = 16;
	} else if (params_format(params) == SNDRV_PCM_FORMAT_S24_LE) {
		out_hw_params->sample_size = 24;
	} else {
		dev_err(rtd->dev, "%s: Invalid Format 0x%x\n",
			__func__, params_format(params));
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	out_hw_params->buf_sz = params_buffer_bytes(params) /
			out_hw_params->period_count;
	dev_dbg(rtd->dev,
		"%s: channels %d sample rate %d sample size %d buffer size %d period count %d\n",
		__func__, out_hw_params->num_chs, out_hw_params->sample_rate,
		out_hw_params->sample_size, out_hw_params->buf_sz,
		out_hw_params->period_count);

	/*
	 * copy the out_hw_params to in_hw_params. in_hw_params will be
	 * over-written with LSM_SET_INPUT_HW_PARAMS ioctl from userspace.
	 * If this ioctl is not set, then it is assumed that input and
	 * output hw params for LSM are the same.
	 * Currently the period_count and buf_sz are unused for input params.
	 */
	memcpy(in_hw_params, out_hw_params,
	       sizeof(struct lsm_hw_params));
	mutex_unlock(&lsm_dev->lock);
	return 0;
}

static snd_pcm_uframes_t msm_lsm_pcm_pointer(
	struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd;

	if (!substream->private_data) {
		pr_err("%s: Invalid private_data", __func__);
		return -EINVAL;
	}
	rtd = substream->private_data;

	if (!prtd) {
		dev_err(rtd->dev,
			"%s: Invalid param %pK\n", __func__, prtd);
		return 0;
	}

	if (prtd->dma_write >= snd_pcm_lib_buffer_bytes(substream))
		prtd->dma_write = 0;
	dev_dbg(rtd->dev,
		"%s: dma post = %d\n", __func__, prtd->dma_write);
	return bytes_to_frames(runtime, prtd->dma_write);
}

static int msm_lsm_pcm_copy(struct snd_soc_component *component,
	struct snd_pcm_substream *substream, int ch,
	unsigned long hwoff, void __user *buf, unsigned long fbytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct lsm_priv *prtd = runtime->private_data;
	char *pcm_buf = NULL;
	int rc = 0, buf_index = 0;
	unsigned long flags = 0;
	struct snd_soc_pcm_runtime *rtd;

	if (!substream->private_data) {
		pr_err("%s: Invalid private_data", __func__);
		return -EINVAL;
	}
	rtd = substream->private_data;

	if (!prtd) {
		dev_err(rtd->dev,
			"%s: Invalid param %pK\n", __func__, prtd);
		return -EINVAL;
	}

	if (runtime->status->state == SNDRV_PCM_STATE_XRUN ||
	    runtime->status->state == SNDRV_PCM_STATE_PREPARED) {
		dev_err(rtd->dev,
			"%s: runtime state incorrect %d", __func__,
			runtime->status->state);
		return 0;
	}
	rc = wait_event_timeout(prtd->period_wait,
		(atomic_read(&prtd->buf_count) |
		atomic_read(&prtd->read_abort)), (2 * HZ));
	if (!rc) {
		dev_err(rtd->dev,
			"%s: timeout for read retry\n", __func__);
		return -EAGAIN;
	}
	if (atomic_read(&prtd->read_abort)) {
		dev_err(rtd->dev,
			"%s: Read abort received\n", __func__);
		return -EIO;
	}
	prtd->appl_cnt = prtd->appl_cnt %
		prtd->lsm_client->out_hw_params.period_count;
	pcm_buf = prtd->lsm_client->lab_buffer[prtd->appl_cnt].data;
	dev_dbg(rtd->dev,
		"%s: copy the pcm data size %lu\n",
		__func__, fbytes);
	if (pcm_buf) {
		if (copy_to_user(buf, pcm_buf, fbytes)) {
			dev_err(rtd->dev,
				"%s: failed to copy bytes %lu\n",
				__func__, fbytes);
			return -EINVAL;
		}
	} else {
		dev_err(rtd->dev,
			"%s: Invalid pcm buffer\n", __func__);
		return -EINVAL;
	}
	prtd->appl_cnt = (prtd->appl_cnt + 1) %
		prtd->lsm_client->out_hw_params.period_count;

	spin_lock_irqsave(&prtd->xrun_lock, flags);
	/* Queue lab buffer here if in xrun */
	if (prtd->xrun_count > 0) {
		(prtd->xrun_count)--;
		buf_index = (prtd->xrun_index + 1) %
			prtd->lsm_client->out_hw_params.period_count;
		rc = msm_lsm_queue_lab_buffer(prtd, buf_index);
		if (rc)
			dev_err(rtd->dev,
				"%s: error in queuing the lab buffer rc %d\n",
				__func__, rc);
		prtd->xrun_index = buf_index;
	}
	atomic_dec(&prtd->buf_count);
	spin_unlock_irqrestore(&prtd->xrun_lock, flags);

	return 0;
}

static int msm_lsm_app_type_cfg_ctl_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_TX;
	int be_id = ucontrol->value.integer.value[3];
	struct msm_pcm_stream_app_type_cfg cfg_data = {0};
	int ret = 0;

	cfg_data.app_type = ucontrol->value.integer.value[0];
	cfg_data.acdb_dev_id = ucontrol->value.integer.value[1];
	cfg_data.sample_rate = ucontrol->value.integer.value[2];

	pr_debug("%s: fe_id- %llu session_type- %d be_id- %d app_type- %d acdb_dev_id- %d sample_rate- %d\n",
		__func__, fe_id, session_type, be_id,
		cfg_data.app_type, cfg_data.acdb_dev_id, cfg_data.sample_rate);
	ret = msm_pcm_routing_reg_stream_app_type_cfg(fe_id, session_type,
						      be_id, &cfg_data);
	if (ret < 0)
		pr_err("%s: msm_pcm_routing_reg_stream_app_type_cfg failed returned %d\n",
			__func__, ret);

	return 0;
}

static int msm_lsm_app_type_cfg_ctl_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_TX;
	int be_id = 0;
	struct msm_pcm_stream_app_type_cfg cfg_data = {0};
	int ret = 0;

	ret = msm_pcm_routing_get_stream_app_type_cfg(fe_id, session_type,
						      &be_id, &cfg_data);
	if (ret < 0) {
		pr_err("%s: msm_pcm_routing_get_stream_app_type_cfg failed returned %d\n",
			__func__, ret);
		goto done;
	}

	ucontrol->value.integer.value[0] = cfg_data.app_type;
	ucontrol->value.integer.value[1] = cfg_data.acdb_dev_id;
	ucontrol->value.integer.value[2] = cfg_data.sample_rate;
	ucontrol->value.integer.value[3] = be_id;
	pr_debug("%s: fedai_id %llu, session_type %d, be_id %d, app_type %d, acdb_dev_id %d, sample_rate %d\n",
		__func__, fe_id, session_type, be_id,
		cfg_data.app_type, cfg_data.acdb_dev_id, cfg_data.sample_rate);
done:
	return ret;
}

static int msm_lsm_add_app_type_controls(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;
	struct snd_pcm_usr *app_type_info;
	struct snd_kcontrol *kctl;
	const char *mixer_ctl_name	= "Listen Stream";
	const char *deviceNo		= "NN";
	const char *suffix		= "App Type Cfg";
	int ctl_len, ret = 0;
	char kctl_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = {0};

	ctl_len = strlen(mixer_ctl_name) + 1 +
			strlen(deviceNo) + 1 + strlen(suffix) + 1;
	snprintf(kctl_name, ctl_len, "%s %d %s",
		mixer_ctl_name, rtd->pcm->device, suffix);
	pr_debug("%s: Listen app type cntrl add\n", __func__);
	ret = snd_pcm_add_usr_ctls(pcm, SNDRV_PCM_STREAM_CAPTURE,
				NULL, 1, kctl_name, rtd->dai_link->id,
				&app_type_info);
	if (ret < 0) {
		pr_err("%s: Listen app type cntrl add failed: %d\n",
			__func__, ret);
		return ret;
	}
	kctl = app_type_info->kctl;
	kctl->put = msm_lsm_app_type_cfg_ctl_put;
	kctl->get = msm_lsm_app_type_cfg_ctl_get;
	return 0;
}

static int msm_lsm_afe_data_ctl_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	uint16_t afe_data_format = 0;
	int ret = 0;

	afe_data_format = ucontrol->value.integer.value[0];
	pr_debug("%s: afe data is %s\n", __func__,
		 afe_data_format ? "unprocessed" : "processed");

	ret = q6lsm_set_afe_data_format(fe_id, afe_data_format);
	if (ret)
		pr_err("%s: q6lsm_set_afe_data_format failed, ret = %d\n",
			__func__, ret);

	return ret;
}

static int msm_lsm_afe_data_ctl_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	uint16_t afe_data_format = 0;
	int ret = 0;

	q6lsm_get_afe_data_format(fe_id, &afe_data_format);
	ucontrol->value.integer.value[0] = afe_data_format;
	pr_debug("%s: afe data is %s\n", __func__,
		 afe_data_format ? "unprocessed" : "processed");

	return ret;
}

static int msm_lsm_add_afe_data_controls(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;
	struct snd_pcm_usr *afe_data_info;
	struct snd_kcontrol *kctl;
	const char *mixer_ctl_name	= "Listen Stream";
	const char *deviceNo		= "NN";
	const char *suffix		= "Unprocessed Data";
	int ctl_len, ret = 0;
	char kctl_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN] = {0};

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1 +
		  strlen(suffix) + 1;
	snprintf(kctl_name, ctl_len, "%s %d %s",
		 mixer_ctl_name, rtd->pcm->device, suffix);
	pr_debug("%s: Adding Listen afe data cntrls\n", __func__);
	ret = snd_pcm_add_usr_ctls(pcm, SNDRV_PCM_STREAM_CAPTURE,
				   NULL, 1, kctl_name, rtd->dai_link->id,
				   &afe_data_info);
	if (ret < 0) {
		pr_err("%s: Adding Listen afe data cntrls failed: %d\n",
		       __func__, ret);
		return ret;
	}
	kctl = afe_data_info->kctl;
	kctl->put = msm_lsm_afe_data_ctl_put;
	kctl->get = msm_lsm_afe_data_ctl_get;

	return 0;
}

#ifdef ENABLE_SVA_MIXER_CTL
static int get_substream_info(struct snd_kcontrol *kcontrol,
		struct snd_pcm_substream **substream)
{
	struct snd_pcm_va_info *pdata;
	struct snd_soc_pcm_runtime *soc_prtd;

	pdata = snd_kcontrol_chip(kcontrol);
	if (!pdata)
	{
		pr_err("%s: pdata is NULL", __func__);
		return -ENODEV;
	}

	if (!pdata->pcm)
	{
		pr_err("%s: pdata->pcm is NULL\n", __func__);
		return -ENODEV;
	}

	*substream = pdata->pcm->streams[pdata->stream].substream;
	if (!(*substream)) {
		pr_err("%s substream not found\n", __func__);
		return -ENODEV;
	}

	soc_prtd = (*substream)->private_data;
	if (!(*substream)->runtime || !soc_prtd) {
		pr_err("%s substream runtime or private_data not found\n",
				__func__);
		return -ENODEV;
	}

	return 0;

}

static int msm_lsm_va_ctl_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	u32 enable = 0;
	struct snd_pcm_runtime *runtime = NULL;
	struct lsm_params_info_v2 p_info = {0};
	struct snd_pcm_substream *substream = NULL;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;

	enable = ucontrol->value.integer.value[0];
	p_info.param_type = LSM_LAB_CONTROL;
	p_info.module_id = LSM_MODULE_ID_LAB;
	p_info.instance_id = INSTANCE_ID_0;
	p_info.stage_idx = LSM_STAGE_INDEX_FIRST;
	p_info.param_size = 0;
	ret = msm_lsm_check_and_set_lab_controls(substream, enable, &p_info);

	return ret;
}

/*place holder for control get function*/
static int msm_lsm_va_ctl_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

/*place holder for control get function*/
static int msm_lsm_va_sess_data_ctl_get(struct snd_kcontrol *kcontrol,
		unsigned int __user *bytes, unsigned int size)
{
	return 0;
}

static int msm_lsm_va_sess_data_ctl_put(struct snd_kcontrol *kcontrol,
		const unsigned int __user *bytes, unsigned int size)
{
	int ret;
	int stage_idx;
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_lsm_session_data_v2 ses_data;
	struct snd_pcm_substream *substream = NULL;
	struct snd_soc_component *component = NULL;
	struct lsm_char_dev *lsm_dev = NULL;
	uint32_t max_detection_stages_supported = LSM_MAX_STAGES_PER_SESSION;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: Invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	if (size != sizeof(struct snd_lsm_session_data_v2))
	{
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}

	if (copy_from_user(&ses_data, bytes, size))
		pr_err("%s: Error copying from user", __func__);

	if (ses_data.app_id != LSM_VOICE_WAKEUP_APP_ID_V2) {
		dev_err(rtd->dev,
				"%s:Invalid App id %d for Listen client\n",
				__func__, ses_data.app_id);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	/*
	 * Before validating num_stages from user argument.
	 * Check ADSP support for multi-stage session,
	 * and reset max_detection_stages_supported to "1" if required.
	 */
	if (!q6lsm_adsp_supports_multi_stage_detection()) {
		dev_dbg(rtd->dev,
			"%s: multi-stage session not supported by adsp\n",
			__func__);
		max_detection_stages_supported = 1;
	}

	if (ses_data.num_stages <= 0 ||
			ses_data.num_stages > max_detection_stages_supported) {
		dev_err(rtd->dev,
			"%s: Unsupported number of stages req(%d)/max(%d)\n",
				__func__, ses_data.num_stages,
				max_detection_stages_supported);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}

	prtd->lsm_client->app_id = ses_data.app_id;
	prtd->lsm_client->num_stages = ses_data.num_stages;

	for (stage_idx = LSM_STAGE_INDEX_FIRST;
			stage_idx < ses_data.num_stages; stage_idx++) {
		prtd->lsm_client->stage_cfg[stage_idx].app_type =
			ses_data.stage_info[stage_idx].app_type;
		prtd->lsm_client->stage_cfg[stage_idx].lpi_enable =
			ses_data.stage_info[stage_idx].lpi_enable;
	}

	ret = q6lsm_open(prtd->lsm_client, ses_data.app_id);
	if (ret < 0) {
		dev_err(rtd->dev, "%s: lsm open failed, %d\n", __func__, ret);
		mutex_unlock(&lsm_dev->lock);
		return ret;
	}
	prtd->lsm_client->opened = true;
	dev_dbg(rtd->dev, "%s: Session_ID = %d, APP ID = %d, Num stages %d\n",
		__func__, prtd->lsm_client->session, prtd->lsm_client->app_id,
			prtd->lsm_client->num_stages);
	mutex_unlock(&lsm_dev->lock);
	return 0;
}

/*place holder for control get function*/
static int msm_lsm_module_params_get(struct snd_kcontrol *kcontrol,
		unsigned int __user *bytes, unsigned int size)
{
	return 0;
}

int pack_lsm_params_info(struct lsm_params_info *temp_ptr_info, void * ptr)
{
	int size = 0;
	uintptr_t ptr_32;
	temp_ptr_info->module_id = *((__u32 *)ptr);
	size += LSM_ALIGN_4BYTE(sizeof(__u32));
	temp_ptr_info->param_id = *((__u32 *)(ptr + size ));
	size += LSM_ALIGN_4BYTE(sizeof(__u32));
	temp_ptr_info->param_size = *((__u32 *)(ptr + size ));
	size += LSM_ALIGN_4BYTE(sizeof(__u32));
	ptr_32 = (uintptr_t)(*((__u8 **) (ptr + size)));
	temp_ptr_info->param_data = (__u8 *)LSM_32B_ADDR_TRANSLATE(ptr_32);
	size += LSM_POINTER_SIZE_32B;
	temp_ptr_info->param_type = *((__u32 *)(ptr + size));
	size += LSM_ALIGN_4BYTE(sizeof(__u32));

	/* add size for v2 version members,
	 * sthal is sending v2 version structure. lsm_params_info_v2
	 */
	size += LSM_ALIGN_4BYTE(sizeof(__u32));
	size += LSM_ALIGN_4BYTE(sizeof(__u32));
	return size;
}

static int msm_lsm_module_params_put(struct snd_kcontrol *kcontrol,
		const unsigned int __user *bytes, unsigned int size)
{
	int err;
	u8 *params;
	int param_size;
	u8 *params_temp;
#ifndef __LP64__
	uintptr_t ptr_32;
	void *temp_ptr = NULL;
#endif
	struct lsm_priv *prtd;
	size_t p_size = 0, count;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_params_info_v2 info_v2;
	struct snd_lsm_module_params *p_data = NULL;
	struct snd_lsm_module_params lsm_params;
	struct snd_pcm_substream *substream = NULL;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;
#ifndef __LP64__
	struct lsm_params_info temp_ptr_info;
#endif
	struct lsm_params_info_v2 *ptr_info_v2 = NULL;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	if (size > sizeof(struct snd_lsm_module_params))
		return -EFAULT;

	p_data = kzalloc(sizeof(*p_data), GFP_KERNEL);
	if (!p_data)
		return -ENOMEM;

	if (copy_from_user(p_data, bytes, size))
		pr_err("%s: Error copying from user", __func__);

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	mutex_lock(&prtd->lsm_api_lock);
#ifdef __LP64__
	lsm_params = *p_data;
#else
	/*pack lsm_params to handle 32/64 bit issue*/
	temp_ptr = (void *)p_data;
	ptr_32 = (uintptr_t)(*((__u8 **) temp_ptr));
	lsm_params.params = (__u8 *)LSM_32B_ADDR_TRANSLATE(ptr_32);
	temp_ptr = temp_ptr + LSM_ALIGN_4BYTE(sizeof(__u8));
	lsm_params.num_params = *((__u32 *) temp_ptr);
	temp_ptr = temp_ptr + LSM_ALIGN_4BYTE(sizeof(__u32));
	lsm_params.data_size = *((__u32 *) temp_ptr);
#endif

	if (lsm_params.num_params > LSM_PARAMS_MAX) {
		dev_err(rtd->dev, "%s: %s: Invalid num_params %d\n",
				__func__, "SET_MODULE_PARAMS(_V2)",
				lsm_params.num_params);
		err = -EINVAL;
		goto err_free_pdata;
	}

#ifdef __LP64__
	p_size = lsm_params.num_params *
			(sizeof(struct lsm_params_info_v2));
#else
	p_size = lsm_params.num_params *
			(sizeof(struct lsm_params_info) - LSM_POINTER_SIZE_32B);
#endif

	if (lsm_params.data_size != p_size) {
		dev_err(rtd->dev,
			"%s: %s: Invalid data_size(%u) against expected(%zd)\n",
				__func__, "SET_MODULE_PARAMS(_V2)",
				lsm_params.data_size, p_size);
		err = -EFAULT;
		goto err_free_pdata;
	}

	params = kzalloc(p_size, GFP_KERNEL);
	if (!params) {
		err = -ENOMEM;
		goto err_free_pdata;
	}

	if (copy_from_user(params, lsm_params.params,
				lsm_params.data_size)) {
		dev_err(rtd->dev,
				"%s: %s: copy_from_user failed, size = %d\n",
				__func__, "set module params", lsm_params.data_size);
		kfree(params);
		err = -EFAULT;
		goto err_free_pdata;
	}

	memset(&info_v2, 0, sizeof(info_v2));

	params_temp = params;

	for (count = 0; count < lsm_params.num_params; count++) {
#ifdef __LP64__
		param_size = sizeof(struct lsm_params_info_v2);
		info_v2.module_id = ((struct lsm_params_info_v2 *)(params))->module_id;
		info_v2.param_id = ((struct lsm_params_info_v2 *)(params))->param_id;
		info_v2.param_size = ((struct lsm_params_info_v2 *)(params))->param_size;
		info_v2.param_data = ((struct lsm_params_info_v2 *)(params))->param_data;
		info_v2.param_type = ((struct lsm_params_info_v2 *)(params))->param_type;
		info_v2.instance_id = INSTANCE_ID_0;
		info_v2.stage_idx = LSM_STAGE_INDEX_FIRST;
		info_v2.model_id = 0;
#else
		/* convert to V2 param info struct from legacy param info */
		param_size = pack_lsm_params_info(&temp_ptr_info, params);
		info_v2.module_id = temp_ptr_info.module_id;
		info_v2.param_id = temp_ptr_info.param_id;
		info_v2.param_size = temp_ptr_info.param_size;
		info_v2.param_data = temp_ptr_info.param_data;
		info_v2.param_type = temp_ptr_info.param_type;
#endif
		info_v2.instance_id = INSTANCE_ID_0;
		info_v2.stage_idx = LSM_STAGE_INDEX_FIRST;
		info_v2.model_id = 0;
		params = params + param_size;
		ptr_info_v2 = &info_v2;

		err = msm_lsm_process_params(substream, ptr_info_v2);
		if (err)
			dev_err(rtd->dev,
				"%s: Failed to process param, type=%d stage=%d err=%d\n",
					__func__, ptr_info_v2->param_type,
					ptr_info_v2->stage_idx, err);
	}
	kfree(params_temp);
	kfree(p_data);
	mutex_unlock(&prtd->lsm_api_lock);
	mutex_unlock(&lsm_dev->lock);
	return 0;

err_free_pdata:
	kfree(p_data);
	mutex_unlock(&prtd->lsm_api_lock);
	mutex_unlock(&lsm_dev->lock);
	return err;
}

/*place holder for control get function*/
static int msm_lsm_fwk_mode_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}
static int msm_lsm_fwk_mode_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	u32 mode;
	int ret = 0;
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream *substream;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;


	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	mode = ucontrol->value.integer.value[0];
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	if (prtd->lsm_client->event_mode == mode) {
		dev_dbg(rtd->dev,
			"%s: mode for %d already set to %d\n",
			__func__, prtd->lsm_client->session, mode);
		ret = 0;
	}
	else {
		dev_dbg(rtd->dev, "%s: Event mode = %d\n",
				__func__, mode);
		ret = q6lsm_set_fwk_mode_cfg(prtd->lsm_client, mode);
		if (!ret)
			prtd->lsm_client->event_mode = mode;
		else
			dev_err(rtd->dev,
				"%s: set event mode failed %d\n",
				__func__, ret);
	}

	mutex_unlock(&lsm_dev->lock);
	return ret;
}

/*place holder for control get function*/
static int msm_lsm_port_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int msm_lsm_port_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_substream *substream;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;
	struct snd_soc_pcm_runtime *rtd;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}
	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	ret = q6lsm_set_port_connected(prtd->lsm_client);
	mutex_unlock(&lsm_dev->lock);
	return ret;
}

/*place holder for control get function*/
static int msm_lsm_ape_control_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int msm_lsm_ape_control_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream *substream;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;
	int operation = ucontrol->value.integer.value[0];

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	switch (operation) {
	case LSM_START:
		dev_dbg(rtd->dev, "%s: Starting LSM client session\n",
				__func__);
		if (!prtd->lsm_client->started) {
			ret = q6lsm_start(prtd->lsm_client, true);
			if (!ret) {
				prtd->lsm_client->started = true;
				dev_dbg(rtd->dev, "%s: LSM client session started\n",
						__func__);
			}
		}
		break;
	case LSM_STOP:
		dev_dbg(rtd->dev,
				"%s: Stopping LSM client session\n",
				__func__);
		if (prtd->lsm_client->started) {
			if (prtd->lsm_client->lab_enable) {
				atomic_set(&prtd->read_abort, 1);
				if (prtd->lsm_client->lab_started) {
					ret = q6lsm_stop_lab(prtd->lsm_client);
					if (ret)
						dev_err(rtd->dev,
						"%s: stop lab failed rc %d\n",
								__func__, ret);
					prtd->lsm_client->lab_started = false;
				}
			}

			if (!atomic_read(&prtd->read_abort)) {
				dev_dbg(rtd->dev,
					"%s: set read_abort to stop buffering\n",
					__func__);
				atomic_set(&prtd->read_abort, 1);
			}
			ret = q6lsm_stop(prtd->lsm_client, true);
			if (!ret)
				dev_dbg(rtd->dev,
					"%s: LSM client session stopped %d\n",
						__func__, ret);
			prtd->lsm_client->started = false;
		}
		break;
	case LSM_STOP_LAB:
		dev_dbg(rtd->dev, "%s: stopping LAB\n", __func__);
		if (prtd->lsm_client->lab_enable &&
				prtd->lsm_client->lab_started) {
			atomic_set(&prtd->read_abort, 1);
			ret = q6lsm_stop_lab(prtd->lsm_client);
			if (ret)
				dev_err(rtd->dev,
				"%s: Lab stop failed for session %d rc %d\n",
						__func__,
					prtd->lsm_client->session, ret);
			prtd->lsm_client->lab_started = false;
		}
		break;
	case LSM_ABORT_EVENT:
		dev_dbg(rtd->dev, "%s: Aborting event status wait\n",
				__func__);
		atomic_set(&prtd->event_wait_stop, 1);
		wake_up(&prtd->event_wait);
		break;
	}
	mutex_unlock(&lsm_dev->lock);
	return 0;
}

/*place holder for control get function*/
static int msm_lsm_reg_snd_model_get(struct snd_kcontrol *kcontrol,
		unsigned int __user *bytes, unsigned int size)
{
	return 0;
}

static int msm_lsm_reg_snd_model_put(struct snd_kcontrol *kcontrol,
		const unsigned int __user *bytes, unsigned int size)
{
	/*
	 * With multi-stage support sm buff allocation/free usage param info
	 * to check stage index for which this sound model is being set, and
	 * to check whether sm data is sent using set param command or not.
	 * Hence, set param ids to '0' to indicate allocation is for legacy
	 * reg_sm cmd, where buffer for param header need not be allocated,
	 * also set stage index to LSM_STAGE_INDEX_FIRST.
	 */
	int rc = 0;
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_sound_model *sm = NULL;
	struct snd_pcm_substream *substream;
	struct lsm_params_info_v2 p_info = {0};
	struct snd_lsm_sound_model_v2 snd_model_v2;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	p_info.stage_idx = LSM_STAGE_INDEX_FIRST;
	p_info.param_type = LSM_DEREG_SND_MODEL;
	sm = &prtd->lsm_client->stage_cfg[p_info.stage_idx].sound_model;

	dev_dbg(rtd->dev, "%s: Registering sound model V2\n",
			__func__);
	if (copy_from_user(&snd_model_v2, bytes,
			sizeof(struct snd_lsm_sound_model_v2))) {
		dev_err(rtd->dev, "%s: copy_from_user failed\n",
				__func__);
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}
	if (snd_model_v2.num_confidence_levels >
			MAX_NUM_CONFIDENCE) {
		dev_err(rtd->dev,
				"%s: Invalid conf_levels = %d, maximum allowed = %d\n",
				__func__, snd_model_v2.num_confidence_levels,
				MAX_NUM_CONFIDENCE);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	rc = q6lsm_snd_model_buf_alloc(prtd->lsm_client,
			snd_model_v2.data_size, &p_info, sm);
	if (rc) {
		dev_err(rtd->dev,
				"%s: q6lsm buffer alloc failed V2, size %d\n",
				__func__, snd_model_v2.data_size);
		mutex_unlock(&lsm_dev->lock);
		return rc;
	}
	if (memcpy(sm->data, snd_model_v2.data,
				snd_model_v2.data_size)) {
		dev_err(rtd->dev,
				"%s: copy from user data failed\n"
				"data %pK size %d\n", __func__,
				snd_model_v2.data, snd_model_v2.data_size);
		q6lsm_snd_model_buf_free(prtd->lsm_client, &p_info, sm);
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}

	dev_dbg(rtd->dev, "SND Model Magic no byte[0] %x,\n"
			"byte[1] %x, byte[2] %x byte[3] %x\n",
			snd_model_v2.data[0], snd_model_v2.data[1],
			snd_model_v2.data[2], snd_model_v2.data[3]);
	prtd->lsm_client->num_confidence_levels =
		snd_model_v2.num_confidence_levels;

	rc = msm_lsm_get_conf_levels(prtd->lsm_client,
			snd_model_v2.confidence_level);
	if (rc) {
		dev_err(rtd->dev,
				"%s: get_conf_levels failed, err = %d\n",
				__func__, rc);
		mutex_unlock(&lsm_dev->lock);
		return rc;
	}

	rc = q6lsm_register_sound_model(prtd->lsm_client,
			snd_model_v2.detection_mode,
			snd_model_v2.detect_failure);
	if (rc < 0) {
		dev_err(rtd->dev,
				"%s: Register snd Model v2 failed =%d\n",
				__func__, rc);
		q6lsm_snd_model_buf_free(prtd->lsm_client, &p_info, sm);
	}
	if (prtd->lsm_client->confidence_levels) {
		kfree(prtd->lsm_client->confidence_levels);
		prtd->lsm_client->confidence_levels = NULL;
	}
	mutex_unlock(&lsm_dev->lock);
	return 0;
}

/*place holder for control get function*/
static int msm_lsm_input_hw_params_get(struct snd_kcontrol *kcontrol,
		unsigned int __user *bytes, unsigned int size)
{
	return 0;
}

static int msm_lsm_input_hw_params_put(struct snd_kcontrol *kcontrol,
		const unsigned int __user *bytes, unsigned int size)
{
	struct lsm_priv *prtd;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct lsm_hw_params *in_params;
	struct snd_pcm_substream *substream;
	struct snd_lsm_input_hw_params params;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	if (size > sizeof(struct snd_lsm_input_hw_params)) {
		dev_err(rtd->dev, "%s: %s: Invalid size: %d\n",
				__func__, "LSM_SET_INPUT_HW_PARAMS", size);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	if (copy_from_user(&params, bytes, size)) {
		dev_err(rtd->dev, "%s: %s: copy_from_user failed\n",
				__func__, "LSM_SET_INPUT_HW_PARAMS");
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}

	in_params = &prtd->lsm_client->in_hw_params;
	in_params->sample_rate = params.sample_rate;
	in_params->sample_size = params.bit_width;
	in_params->num_chs = params.num_channels;
	mutex_unlock(&lsm_dev->lock);

	return 0;
}

/*place holder for control put function*/
static int msm_lsm_det_event_info_put(struct snd_kcontrol *kcontrol,
		const unsigned int __user *bytes, unsigned int size)
{
	return 0;
}

static int msm_lsm_det_event_info_get(struct snd_kcontrol *kcontrol,
		unsigned int __user *bytes, unsigned int size)
{
	int err = 0;
	struct lsm_priv *prtd;
	unsigned long flags = 0;
	int user_payload_size = 0;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_lsm_event_status *user;
	struct snd_pcm_substream *substream;
	uint16_t status = 0, payload_size = 0;
	struct lsm_char_dev *lsm_dev;
	struct snd_soc_component *component = NULL;

	if (get_substream_info(kcontrol, &substream))
		return -ENODEV;

	runtime = substream->runtime;
	rtd = substream->private_data;
	component = snd_soc_rtdcom_lookup(rtd, DRV_NAME);
	if (!component || !component->dev) {
		pr_err("%s: invalid component\n", __func__);
		return -EINVAL;
	}
	lsm_dev = (struct lsm_char_dev *) dev_get_drvdata(component->dev);
	if (!lsm_dev) {
		pr_err("%s: platform data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&lsm_dev->lock);
	if (!runtime) {
		pr_err("%s: Invalid runtime\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	prtd = runtime->private_data;
	if (!prtd || !prtd->lsm_client) {
		pr_err("%s: No LSM session active\n", __func__);
		mutex_unlock(&lsm_dev->lock);
		return -EINVAL;
	}
	user_payload_size = size - sizeof(struct snd_lsm_event_status);

	if (user_payload_size > LISTEN_MAX_STATUS_PAYLOAD_SIZE) {
		dev_err(rtd->dev,
			"%s: payload_size %d is invalid, max allowed = %d\n",
				__func__, user_payload_size,
				LISTEN_MAX_STATUS_PAYLOAD_SIZE);
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}

	user = kzalloc(size, GFP_KERNEL);
	if (!user) {
		mutex_unlock(&lsm_dev->lock);
		return -ENOMEM;
	}

	user->payload_size = user_payload_size;

	mutex_lock(&prtd->lsm_api_lock);

	{
		dev_dbg(rtd->dev, "%s: %s: New event available %ld\n",
				__func__, "SNDRV_LSM_GENERIC_DET_EVENT",
				prtd->event_avail);

		spin_lock_irqsave(&prtd->event_lock, flags);

		if (prtd->det_event) {
			payload_size = prtd->det_event->payload_size;
			status = prtd->det_event->status;
			spin_unlock_irqrestore(&prtd->event_lock,
					flags);
		}
		else {
			spin_unlock_irqrestore(&prtd->event_lock,
					flags);
			dev_err(rtd->dev,
					"%s: %s: prtd->event_status is NULL\n",
					__func__,
					"SNDRV_LSM_GENERIC_DET_EVENT");
			err = -EINVAL;
			goto err_free_user_unlock;
		}

		if (user->payload_size < payload_size) {
			dev_err(rtd->dev,
				"%s: provided %d bytes isn't enough, needs %d bytes\n",
					__func__, user->payload_size,
					payload_size);
			spin_unlock_irqrestore(&prtd->event_lock,
					flags);
			err = -ENOMEM;
			goto err_free_user_unlock;
		}

		user->status = status;
		user->payload_size = payload_size;
		memcpy(user->payload, prtd->det_event->payload,
				payload_size);
		spin_unlock_irqrestore(&prtd->event_lock,
					flags);

		err =  msm_lsm_start_lab_buffer(prtd, status);
	}

	mutex_unlock(&prtd->lsm_api_lock);

	if (!access_ok(bytes, size)) {
		dev_err(rtd->dev,
				"%s: Failed to verify write, size = %d\n",
				__func__, size);
		kfree(user);
		mutex_unlock(&lsm_dev->lock);
		return -EFAULT;
	}

	if (copy_to_user(bytes, user, size)) {
		dev_err(rtd->dev,
				"%s: Failed to copy payload to user, size = %d",
				__func__, size);
		err = -EFAULT;
		goto err_free_user;
	}
	kfree(user);
	mutex_unlock(&lsm_dev->lock);
	return 0;

err_free_user_unlock:
	mutex_unlock(&prtd->lsm_api_lock);
err_free_user:
	kfree(user);
	mutex_unlock(&lsm_dev->lock);
	return err;
}

static int msm_lsm_fwk_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = INT_MAX;
	return 0;
}

static int msm_lsm_port_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = INT_MAX;
	return 0;
}

static int msm_lsm_ape_control_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = INT_MAX;
	return 0;
}

static int msm_lsm_va_ctl_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = INT_MAX;
	return 0;
}

static int msm_va_sess_data_ctl_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct snd_lsm_session_data_v2);
	params->get = msm_lsm_va_sess_data_ctl_get;
	params->put = msm_lsm_va_sess_data_ctl_put;
	params->max = uinfo->count;
	return 0;
}

static int msm_va_module_params_ctl_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct snd_lsm_module_params);
	params->get = msm_lsm_module_params_get;
	params->put = msm_lsm_module_params_put;
	params->max = uinfo->count;
	return 0;
}

static int msm_va_reg_snd_model_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct snd_lsm_sound_model_v2);
	params->get = msm_lsm_reg_snd_model_get;
	params->put = msm_lsm_reg_snd_model_put;
	params->max = uinfo->count;
	return 0;
}

static int msm_va_input_hw_params_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct snd_lsm_input_hw_params);
	params->get = msm_lsm_input_hw_params_get;
	params->put = msm_lsm_input_hw_params_put;
	params->max = uinfo->count;
	return 0;
}

static int msm_va_get_det_event_info(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo)
{
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = 512;
	params->get = msm_lsm_det_event_info_get;
	params->put = msm_lsm_det_event_info_put;
	params->max = uinfo->count;
	return 0;
}

struct snd_kcontrol_new va_mixer_ctl[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.name	= "LSM LAB_CONTROL SET",
		.info = msm_lsm_va_ctl_info,
		.get = msm_lsm_va_ctl_get,
		.put = msm_lsm_va_ctl_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.name	= "LSM FWK_MODE SET",
		.info = msm_lsm_fwk_info,
		.get = msm_lsm_fwk_mode_get,
		.put = msm_lsm_fwk_mode_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.name = "LSM PORT SET",
		.info = msm_lsm_port_info,
		.get = msm_lsm_port_get,
		.put = msm_lsm_port_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.name	= "LSM APE_CONTROL CMD",
		.info = msm_lsm_ape_control_info,
		.get = msm_lsm_ape_control_get,
		.put = msm_lsm_ape_control_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.name	= "LSM SESSION_DATA SET",
		.tlv.c	= snd_soc_bytes_tlv_callback,
		.info = msm_va_sess_data_ctl_info,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.name	= "LSM MODULE_PARAMS SET",
		.tlv.c	= snd_soc_bytes_tlv_callback,
		.info = msm_va_module_params_ctl_info,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.name	= "LSM SND_MODEL REG",
		.tlv.c	= snd_soc_bytes_tlv_callback,
		.info = msm_va_reg_snd_model_info,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.name	= "LSM DET_EVENT_INFO GET",
		.tlv.c	= snd_soc_bytes_tlv_callback,
		.info = msm_va_get_det_event_info,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.name	= "LSM INPUT_HW_PARAMS SET",
		.tlv.c	= snd_soc_bytes_tlv_callback,
		.info = msm_va_input_hw_params_info,
	}
};

static int msm_lsm_add_va_controls(struct snd_soc_pcm_runtime *rtd)
{
	int i = 0;
	int ret = 0;
	struct soc_bytes_ext *sbe;
	unsigned long pvt_value = 0;
	struct snd_pcm *pcm = rtd->pcm;
	struct snd_pcm_va_info *va_dev_info = NULL;


	for (i = 0; i < ARRAY_SIZE(va_mixer_ctl); i++)
	{
		if (va_mixer_ctl[i].tlv.c &&
				(va_mixer_ctl[i].access &
				 SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK))
		{
			sbe = kzalloc(sizeof(*sbe), GFP_KERNEL);
			if (sbe == NULL)
				return -ENOMEM;
			pvt_value = (long)sbe;
		}
		pvt_value = (pvt_value == 0) ? rtd->dai_link->id : pvt_value;
		ret = snd_pcm_add_va_ctls(pcm, SNDRV_PCM_STREAM_CAPTURE,
				pvt_value, &va_dev_info, &va_mixer_ctl[i]);
		if (ret < 0) {
			pr_err("%s adding va control failed ret %d\n", __func__, ret);
			return ret;
		}
	}

	return 0;
}
#endif

static int msm_lsm_add_controls(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;

	ret = msm_lsm_add_app_type_controls(rtd);
	if (ret)
		pr_err("%s, add app type controls failed:%d\n", __func__, ret);

	ret = msm_lsm_add_afe_data_controls(rtd);
	if (ret)
		pr_err("%s, add afe data controls failed:%d\n", __func__, ret);

#ifdef ENABLE_SVA_MIXER_CTL
	ret = msm_lsm_add_va_controls(rtd);
	if (ret)
		pr_err("%s, add va controls failed: %d\n", __func__, ret);
#endif
	return ret;
}


static int msm_asoc_lsm_new(struct snd_soc_component *component, struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	int ret = 0;

	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	ret = msm_lsm_add_controls(rtd);
	if (ret)
		pr_err("%s, kctl add failed:%d\n", __func__, ret);

	return ret;
}

static int msm_asoc_lsm_probe(struct snd_soc_component *component)
{
	pr_debug("enter %s\n", __func__);

	return 0;
}

static struct snd_soc_component_driver msm_soc_component = {
	.name			= DRV_NAME,
	.open			= msm_lsm_open,
	.close          	= msm_lsm_close,
	.ioctl          	= msm_lsm_ioctl,
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
	.compat_ioctl		= msm_lsm_ioctl_compat,
#endif
	.prepare		= msm_lsm_prepare,
	.hw_params      	= msm_lsm_hw_params,
	.copy_user      	= msm_lsm_pcm_copy,
	.pointer        	= msm_lsm_pcm_pointer,
	.pcm_construct	= msm_asoc_lsm_new,
	.probe		= msm_asoc_lsm_probe,
};

#ifdef ENABLE_SVA_MIXER_CTL
static int msm_lsm_cdev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int msm_lsm_cdev_close(struct inode *inode, struct file *filp)
{
	return 0;
}

static long msm_lsm_cdev_ioctl(struct file *file,
			unsigned int ioctl, unsigned long arg)
{
	int rc = 0;
	int xchg = 0;
	unsigned long flags = 0;
	struct lsm_priv *prtd = NULL;
	struct lsm_cdev_info lsm_info;
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream substream;

	switch (ioctl) {
	case SNDRV_LSM_GENERIC_DET_EVENT:
		if (copy_from_user(&lsm_info, (void *)arg,sizeof(lsm_info)))
			goto done;
		/*Using substream.number to pass the device number*/
		substream.number = lsm_info.dev_num;
		if (msm_lsm_cdev_session_lut(&substream, GET_INFO))
		{
			pr_err(" %s Failed to get the required data", __func__);
			return -EFAULT;
		}
		runtime = substream.runtime;
		prtd = runtime->private_data;
		rtd = substream.private_data;

		atomic_set(&prtd->event_wait_stop, 0);

		rc = wait_event_freezable(prtd->event_wait,
				(cmpxchg(&prtd->event_avail, 1, 0) ||
				 (xchg = atomic_cmpxchg(&prtd->event_wait_stop,
							1, 0))));
		mutex_lock(&prtd->lsm_api_lock);

		dev_dbg(rtd->dev, "%s: wait_event_freezable %d event_wait_stop %d\n",
				__func__, rc, xchg);

		if (!rc && !xchg) {
			dev_dbg(rtd->dev, "%s: %s: New event available %ld\n",
					__func__, "SNDRV_LSM_GENERIC_DET_EVENT",
					prtd->event_avail);

			spin_lock_irqsave(&prtd->event_lock, flags);


			if (prtd->det_event) {
				lsm_info.det_status = prtd->det_event->status;
				spin_unlock_irqrestore(&prtd->event_lock,
						flags);
			} else {
				spin_unlock_irqrestore(&prtd->event_lock,
						flags);
				dev_dbg(rtd->dev,
					"%s: %s: prtd->event_status is NULL\n",
						__func__,
						"SNDRV_LSM_GENERIC_DET_EVENT");
				rc = -EINVAL;
				break;
			}

		} else if (xchg) {
			lsm_info.det_status = 0;
			dev_dbg(rtd->dev, "%s: %s: Wait aborted\n",
					__func__, "SNDRV_LSM_GENERIC_DET_EVENT");
			rc = 0;
		}
		mutex_unlock(&prtd->lsm_api_lock);

		if (copy_to_user((void *)arg, &lsm_info, sizeof(lsm_info)))
		{
			pr_err("%s: copy to user failed", __func__);
		}
		break;
		case LSM_LUT_CLEAR_INFO:
		{
			struct snd_pcm_substream substream;
			substream.number = (int)arg;
			if (msm_lsm_cdev_session_lut(&substream, CLEAR_INFO))
				pr_err("%s: LUT clear info failed", __func__);
		}
		break;
		default:
		pr_err("Received cmd : 0x%x", ioctl);
	}
	return 0;
done:
	return -EFAULT;
}

static const struct file_operations lsm_cdev_fops = {
	.owner          =       THIS_MODULE,
	.unlocked_ioctl =       msm_lsm_cdev_ioctl,
	.compat_ioctl   =       msm_lsm_cdev_ioctl,
	.open           =       msm_lsm_cdev_open,
	.release        =       msm_lsm_cdev_close,
};

static int msm_lsm_create_char_dev(struct platform_device *pdev)
{
	int ret = 0;
	struct lsm_char_dev *lsm_dev;
	char *cdev_name = "msm_lsm_cdev";


	lsm_dev = devm_kzalloc(&pdev->dev, sizeof(*lsm_dev), GFP_KERNEL);
	if (!lsm_dev) {
		ret = -ENOMEM;
		return ret;
	}

	ret = alloc_chrdev_region(&lsm_dev->dev_num, 0, MINOR_NUMBER_COUNT,
			cdev_name);
	if (ret < 0) {
		pr_err("%s: Failed to alloc char dev %s, err %d\n",
				__func__, cdev_name, ret);
		return ret;
	}

	lsm_dev->cls = class_create(THIS_MODULE, cdev_name);
	if (IS_ERR(lsm_dev->cls)) {
		ret = PTR_ERR(lsm_dev->cls);
		pr_err("%s: Failed to create class, dev %s, err %d\n",
				__func__, cdev_name, ret);
		goto unregister_chrdev;
	}

	lsm_dev->dev = device_create(lsm_dev->cls,
				NULL, lsm_dev->dev_num, NULL, cdev_name);
	if (IS_ERR(lsm_dev->dev)) {
		ret = PTR_ERR(lsm_dev->dev);
		pr_err("%s: Failed to create device, dev %s, err %d\n",
				__func__, cdev_name, ret);
		goto destroy_class;
	}

	cdev_init(&lsm_dev->cdev, &lsm_cdev_fops);
	ret = cdev_add(&lsm_dev->cdev, lsm_dev->dev_num, MINOR_NUMBER_COUNT);
	if (ret < 0) {
		pr_err("%s: Failed to register char dev %s, err %d\n",
				__func__, cdev_name, ret);
		goto destroy_device;
	}

	mutex_init(&lsm_dev->lock);
	dev_set_drvdata(&pdev->dev, lsm_dev);

	return 0;

destroy_device:
	device_destroy(lsm_dev->cls, lsm_dev->dev_num);
destroy_class:
	class_destroy(lsm_dev->cls);
unregister_chrdev:
	unregister_chrdev_region(0, 1);
	return ret;
}

static int msm_lsm_destroy_char_dev(struct lsm_char_dev *lsm_dev)
{
	cdev_del(&lsm_dev->cdev);
	device_destroy(lsm_dev->cls, lsm_dev->dev_num);
	class_destroy(lsm_dev->cls);
	unregister_chrdev_region(0, 1);

	return 0;
}
#endif

static int msm_lsm_probe(struct platform_device *pdev)
{
#ifdef ENABLE_SVA_MIXER_CTL
	msm_lsm_create_char_dev(pdev);
#endif
	return snd_soc_register_component(&pdev->dev, &msm_soc_component,
					  NULL, 0);
}

static int msm_lsm_remove(struct platform_device *pdev)
{
#ifdef ENABLE_SVA_MIXER_CTL
	struct lsm_char_dev *lsm_dev;
	lsm_dev = dev_get_drvdata(&pdev->dev);
	mutex_destroy(&lsm_dev->lock);

	msm_lsm_destroy_char_dev(lsm_dev);
#endif
	snd_soc_unregister_component(&pdev->dev);

	return 0;
}

static const struct of_device_id msm_lsm_client_dt_match[] = {
	{.compatible = "qcom,msm-lsm-client" },
	{ }
};

static struct platform_driver msm_lsm_driver = {
	.driver = {
		.name = "msm-lsm-client",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(msm_lsm_client_dt_match),
		.suppress_bind_attrs = true,
	},
	.probe = msm_lsm_probe,
	.remove = msm_lsm_remove,
};

int __init msm_lsm_client_init(void)
{
	return platform_driver_register(&msm_lsm_driver);
}

void msm_lsm_client_exit(void)
{
	platform_driver_unregister(&msm_lsm_driver);
}

MODULE_DESCRIPTION("LSM client platform driver");
MODULE_DEVICE_TABLE(of, msm_lsm_client_dt_match);
MODULE_LICENSE("GPL v2");
