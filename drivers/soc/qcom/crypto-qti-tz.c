// SPDX-License-Identifier: GPL-2.0-only
/*
 * Crypto TZ library for storage encryption.
 *
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/cacheflush.h>
#include <linux/qcom_scm.h>
#include <linux/qtee_shmbridge.h>
#include <linux/crypto-qti-common.h>
#include <linux/module.h>
#include "crypto-qti-platform.h"

#define ICE_CIPHER_MODE_XTS_256 3
#define UFS_CARD_CE 30

/*
 * TZs without the storage type (CE) variants of the ICE key calls, such as
 * sm6150's, take the legacy CONFIG_SET_ICE_KEY / INVALIDATE_ICE_KEY calls,
 * as the 4.14 sm6150 kernels used. Decided once, at the first key call.
 */
static int crypto_qti_tz_legacy = -1;

static bool crypto_qti_use_legacy(void)
{
	if (crypto_qti_tz_legacy < 0) {
		crypto_qti_tz_legacy = !qcom_scm_ice_v2_available() &&
				       qcom_scm_ice_available();
		pr_info("%s: %s ICE key calls\n", __func__,
			crypto_qti_tz_legacy ? "legacy" : "storage type");
	}

	return crypto_qti_tz_legacy;
}

static int crypto_qti_program_key_legacy(const struct blk_crypto_key *key,
					 unsigned int slot,
					 unsigned int data_unit_mask)
{
	union {
		u8 bytes[BLK_CRYPTO_MAX_ANY_KEY_SIZE];
		u32 words[BLK_CRYPTO_MAX_ANY_KEY_SIZE / sizeof(u32)];
	} k;
	int i, err;

	if (key->size > sizeof(k.bytes))
		return -EINVAL;

	memcpy(k.bytes, key->raw, key->size);
	/* the legacy call takes a standard key as big endian words (4.14) */
	if (key->crypto_cfg.key_type != BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)
		for (i = 0; i < DIV_ROUND_UP(key->size, sizeof(u32)); i++)
			__cpu_to_be32s(&k.words[i]);

	err = qcom_scm_ice_set_key(slot, k.bytes, key->size,
				   QCOM_SCM_ICE_CIPHER_AES_256_XTS,
				   data_unit_mask);
	memzero_explicit(&k, sizeof(k));
	if (err)
		pr_err("%s:SCM call Error: 0x%x slot %d\n", __func__, err, slot);

	return err;
}

int crypto_qti_program_key(const struct ice_mmio_data *mmio_data,
			   const struct blk_crypto_key *key, unsigned int slot,
			   unsigned int data_unit_mask, int capid, int storage_type)
{
	int err = 0;
	struct qtee_shm shm;

	if (crypto_qti_use_legacy())
		return crypto_qti_program_key_legacy(key, slot, data_unit_mask);

	err = qtee_shmbridge_allocate_shm(key->size, &shm);
	if (err)
		return -ENOMEM;

	memcpy(shm.vaddr, key->raw, key->size);
	qtee_shmbridge_flush_shm_buf(&shm);

	err = qcom_scm_config_set_ice_key(slot, shm.paddr, key->size,
					ICE_CIPHER_MODE_XTS_256,
					data_unit_mask, storage_type);
	if (err)
		pr_err("%s:SCM call Error: 0x%x slot %d\n",
				__func__, err, slot);

	qtee_shmbridge_inv_shm_buf(&shm);
	qtee_shmbridge_free_shm(&shm);

	return err;
}
EXPORT_SYMBOL(crypto_qti_program_key);

int crypto_qti_invalidate_key(const struct ice_mmio_data *mmio_data,
			      unsigned int slot, int storage_type)
{
	int err = 0;

	if (crypto_qti_use_legacy())
		err = qcom_scm_ice_invalidate_key(slot);
	else
		err = qcom_scm_clear_ice_key(slot, storage_type);
	if (err)
		pr_err("%s:SCM call Error: 0x%x\n", __func__, err);

	return err;
}
EXPORT_SYMBOL(crypto_qti_invalidate_key);

int crypto_qti_derive_raw_secret_platform(const struct ice_mmio_data *mmio_data,
				const u8 *wrapped_key,
				unsigned int wrapped_key_size, u8 *secret,
				unsigned int secret_size)
{
	int err = 0;
	struct qtee_shm shm_key, shm_secret;

	err = qtee_shmbridge_allocate_shm(wrapped_key_size, &shm_key);
	if (err)
		return -ENOMEM;

	err = qtee_shmbridge_allocate_shm(secret_size, &shm_secret);
	if (err)
		return -ENOMEM;

	memcpy(shm_key.vaddr, wrapped_key, wrapped_key_size);
	qtee_shmbridge_flush_shm_buf(&shm_key);

	memset(shm_secret.vaddr, 0, secret_size);
	qtee_shmbridge_flush_shm_buf(&shm_secret);

	err = qcom_scm_derive_raw_secret(shm_key.paddr, wrapped_key_size,
					shm_secret.paddr, secret_size);
	if (err) {
		pr_err("%s:SCM call Error for derive raw secret: 0x%x\n",
				__func__, err);
	}

	qtee_shmbridge_inv_shm_buf(&shm_secret);
	memcpy(secret, shm_secret.vaddr, secret_size);

	qtee_shmbridge_inv_shm_buf(&shm_key);
	qtee_shmbridge_free_shm(&shm_key);
	qtee_shmbridge_free_shm(&shm_secret);
	return err;
}
EXPORT_SYMBOL(crypto_qti_derive_raw_secret_platform);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Crypto TZ library for storage encryption");
