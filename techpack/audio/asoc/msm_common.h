/*
* SPDX-License-Identifier: GPL-2.0-only
* Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
* Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
*/
#ifndef _MSM_COMMON_H_
#define _MSM_COMMON_H_

#include <sound/soc.h>
#include <sound/pcm.h>

struct snd_card_pdata {
	struct kobject snd_card_kobj;
	int card_status;
};

typedef enum snd_card_status_t {
	SND_CARD_STATUS_OFFLINE = 0,
	SND_CARD_STATUS_ONLINE  = 1,
} snd_card_status_t;

int snd_card_notify_user(snd_card_status_t card_status);
int snd_card_set_card_status(snd_card_status_t card_status);
#endif