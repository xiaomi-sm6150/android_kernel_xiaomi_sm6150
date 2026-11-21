/*******************************************************************************
 * Copyright (C) 2015 Maxim Integrated Products, Inc., All Rights Reserved.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 *******************************************************************************
 *
 *  DS28E16.c - DS28E16 device module. Requires low level 1-Wire connection.
 */
#define pr_fmt(fmt)	"[ds28e16] %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power/xiaomi_batt_verify.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include "sha384_software.h"
#include "ds28e16.h"
#include "onewire_gpio.h"

#define ds_info	pr_debug
#define ds_dbg	pr_debug
#define ds_err	pr_debug
#define ds_log	pr_info



static unsigned char session_seed[32] = {
0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
static unsigned char S_secret[32] = {
0x0C, 0x99, 0x2B, 0xD3, 0x95, 0xDB, 0xA0, 0xB4,
0xEF, 0x07, 0xB3, 0xD8, 0x75, 0xF3, 0xC7, 0xAE,
0xDA, 0xC4, 0x41, 0x2F, 0x48, 0x93, 0xB5, 0xD9,
0xE1, 0xE5, 0x4B, 0x20, 0x9B, 0xF3, 0x77, 0x39};
static unsigned char challenge[32] = {0x00};
static int auth_ANON = 1;
static int auth_BDCONST = 1;
static int pagenumber = 0;

// maxim define
static int tm = 1;
static unsigned short CRC16;
static const short oddparity[16] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };
static unsigned char last_result_byte = RESULT_SUCCESS;

static unsigned char MANID[2] = {0x00};

// mi add
static unsigned char flag_mi_romid = 0;
static unsigned char flag_mi_status = 0;
static unsigned char flag_mi_page0_data = 0;
static unsigned char flag_mi_page1_data = 0;
static unsigned char flag_mi_counter = 0;
static unsigned char flag_mi_auth_result = 0;
static unsigned char mi_romid[8] = {0x00};
static unsigned char mi_status[8] = {0x00};
static unsigned char mi_page0_data[16] = {0x00};
static unsigned char mi_page1_data[16] = {0x00};
static unsigned char mi_counter[16] = {0x00};
static int mi_auth_result = 0x00;
static bool batt_verified_result_from_uefi;
static bool batt_chip_ok_result_from_uefi;


static unsigned char crc_low_first(unsigned char *ptr, unsigned char len)
{
	unsigned char i;
	unsigned char crc = 0x00;

	while (len--) {
		crc ^= *ptr++;
		for (i = 0; i < 8; ++i) {
			if (crc & 0x01)
				crc = (crc >> 1) ^ 0x8c;
			else
				crc = (crc >> 1);
		}
	}

	return (crc);
}

static short Read_RomID(unsigned char *RomID)
{
	unsigned char i;
	unsigned char crc = 0x00;

	ds_info("flag_mi_status = %02x, flag_mi_romid = %02x\n", flag_mi_status, flag_mi_romid);

	if (flag_mi_romid == 2) {
		memcpy(RomID, mi_romid, 8);
		return DS_TRUE;
	}

	if ((xm_ow_reset()) != 0) {
		ds_err("Failed to reset ds28e16!\n");
		xm_ow_reset();
		return ERROR_NO_DEVICE;
	}

	ds_dbg("Ready to write 0x33 to maxim IC!\n");
	xm_ow_write_byte(CMD_READ_ROM);
	udelay(10);
	for (i = 0; i < 8; i++)
		RomID[i] = xm_ow_read_byte();

	ds_info("RomID = %02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
	RomID[0], RomID[1], RomID[2], RomID[3],
	RomID[4], RomID[5], RomID[6], RomID[7]);

	crc = crc_low_first(RomID, 7);
	ds_info("crc_low_first = %02x\n", crc);

	if (crc == RomID[7]) {
		memcpy(mi_romid, RomID, 8);
		if ((mi_romid[0] == FAMILY_CODE) && (mi_romid[6] == CUSTOM_ID_MSB) && ((mi_romid[5] & 0xf0) == CUSTOM_ID_LSB))
			flag_mi_romid = 2;
		else
			flag_mi_romid = 1;
		return DS_TRUE;
	} else {
		xm_ow_reset();
		return DS_FALSE;
	}
}

static unsigned short docrc16(unsigned short data)
{
	data = (data ^ (CRC16 & 0xff)) & 0xff;
	CRC16 >>= 8;

	if (oddparity[data & 0xf] ^ oddparity[data >> 4])
		CRC16 ^= 0xc001;

	data <<= 6;
	CRC16  ^= data;
	data <<= 1;
	CRC16   ^= data;

	return CRC16;
}

static int DS28E16_standard_cmd_flow(unsigned char *write_buf, int delay_ms,
unsigned char *read_buf, int *read_len, int write_len)
{
	unsigned char buf[128];
	int i;
	int buf_len = 0;
	int expected_read_len = *read_len;

	//NEW FLOW
	/*'1 Wire
	'''''''''''''''''''''''
	?<Reset/Presence>
	?<ROM level command Sequence>
	?TX: Start Command
	?TX: Length Byte
	?TX: Memory Command
	?TX: Parameter, TX: Data
	?RX: CRC16
	?TX: Release Byte
	?<Strong pull-up Delay>
	?RX: Dummy Byte
	?RX: Length Byte
	?RX: Result byte
	?RX: CRC16
	?< wait for reset>
	'''''''''''''''''''''''*/
	ds_dbg("DS28E16_standard_cmd_flow start\n");

	if ((xm_ow_reset()) != 0) {
		ds_err("Failed to reset ds28e16!\n");
		xm_ow_reset();
		return ERROR_NO_DEVICE;
	}

	xm_ow_write_byte(CMD_SKIP_ROM);

	buf[buf_len++] = CMD_START;
	memcpy(&buf[buf_len], write_buf, write_len);
	buf_len += write_len;
	for (i = 0; i < buf_len; i++)
		xm_ow_write_byte(buf[i]);

	buf[buf_len++] = xm_ow_read_byte();
	buf[buf_len++] = xm_ow_read_byte();

	ds_err("DS28E16_standard_cmd_flow: crc is :\n");
	ds_err("%02x %02x \n",buf[buf_len-1], buf[buf_len]);

	CRC16 = 0;
	for (i = 0; i < buf_len; i++)
		docrc16(buf[i]);

	if (CRC16 != 0xB001) {
		xm_ow_reset();
		ds_err("DS28E16_standard_cmd_flow: crc error!\n");
		return DS_FALSE;
	}

	// check for strong pull-up
	if (delay_ms > 0) {
		xm_ow_write_byte(CMD_RELEASE_BYTE);
		msleep(delay_ms);
	}

	xm_ow_read_byte();
	*read_len = xm_ow_read_byte();

	if (expected_read_len != *read_len) {
		xm_ow_reset();
		ds_err("DS28E16_standard_cmd_flow: len error!\n");
		return DS_FALSE;
	}

	buf_len = *read_len + 2;
	for (i = 0; i < buf_len; i++) {
		buf[i] = xm_ow_read_byte();
	//	ds_err("buf[%d] = 0x%02x %s, len = %d\n", i, buf[i], buf[i], buf_len);
	}

	CRC16 = 0;
	docrc16(*read_len);
	for (i = 0; i < buf_len; i++)
		docrc16(buf[i]);

	if (CRC16 != 0xB001) {
		xm_ow_reset();
		ds_err("DS28E16_standard_cmd_flow: crc2 error!\n");
		return DS_FALSE;
	}

	memcpy(read_buf, buf, *read_len);
	ds_info("DS28E16_standard_cmd_flow success!\n");
	return DS_TRUE;
}

//--------------------------------------------------------------------------
/// 'Read Status' command
///
/// @param[out] data
/// pointer to unsigned char (buffer of length 1) for page protection data
///
///  @return
///  DS_TRUE - command successful @n
///  DS_FALSE - command failed
///
static int DS28E16_cmd_readStatus(unsigned char *data)
{
	unsigned char write_buf[255];
	unsigned char read_buf[255];
	int write_len = 0;
	int len_byte = 1;
	int read_len = 7;

	if (flag_mi_status) {
		memcpy(data, mi_status, 8);
		return DS_TRUE;
	}

	last_result_byte = RESULT_FAIL_NONE;
	/*
	?<Start, device address write>
	?TX: Read Page Protection command
	?TX: Length (SMBus) [always 1]
	?TX: Parameter
	?<Stop>
	?<Delay>
	?<Start, device address read>
	?RX: Length (SMBus) [always 1]
	?RX: Protection Data
	?<Stop>
	*/

	write_buf[write_len++] = len_byte;
	write_buf[write_len++] = CMD_READ_STATUS;

	if (DS28E16_standard_cmd_flow(write_buf, DELAY_DS28E16_EE_READ*tm,
		read_buf, &read_len, write_len)) {
		if (read_buf[0] == RESULT_SUCCESS) {
			last_result_byte = read_buf[0];
			memcpy(data, &read_buf[1], 8);
			flag_mi_status = 1;
			memcpy(mi_status, data, 8);
			MANID[0] = data[4];
			return DS_TRUE;
		}
	}

	xm_ow_reset();
	return DS_FALSE;
}

//--------------------------------------------------------------------------
/// 'Read Memory' command
///
/// @param[in] pg
/// page number to read
/// @param[out] data
/// buffer length must be at least 32 bytes to hold memory read
///
///  @return
///  DS_TRUE - command successful @n
///  DS_FALSE - command failed
///
static int DS28E16_cmd_readMemory(int pg, unsigned char *data)
{
	unsigned char write_buf[3];
	unsigned char read_buf[255];
	int write_len = 0;
	int read_len = 33;
	int length_byte = 2;
	unsigned char pagenum = (unsigned char)pg & 0x03;

	switch (pagenum)
	{
		case 0x00:
			if (flag_mi_page0_data) {
				memcpy(data, mi_page0_data, 16);
				return DS_TRUE;
			}
			break;
		case 0x01:
			if (flag_mi_page1_data) {
				memcpy(data, mi_page1_data, 16);
				return DS_TRUE;
			}
			break;
		case 0x02:
			if (flag_mi_counter) {
				memcpy(data, mi_counter, 16);
				return DS_TRUE;
			}
			break;
		default:
			break;
	}

	last_result_byte = RESULT_FAIL_NONE;

	/*
	?<Start, device address write>
	?TX: Read Memory Command
	?TX: Length (SMBus)
	?TX: Parameter
	?<Stop>
	?<Delay>
	?<Start, device address read>
	?RX: Length (SMBus)
	?RX: Result byte
	?RX: Data
	?<Stop>
	*/

	if (pg >= MAX_PAGENUM) {
		ds_err("page(%d) data should not be set.\n", pg);
		return DS_FALSE;
	}

	write_buf[write_len++] = length_byte;
	write_buf[write_len++] = CMD_READ_MEM;
	write_buf[write_len++] = pagenum;

	ds_dbg("DS28E16_cmd_readMemory write_buf:\n");
	ds_info("%02x %02x %02x", write_buf[0], write_buf[1], write_buf[2]);

	if (DS28E16_standard_cmd_flow(write_buf, DELAY_DS28E16_EE_READ*tm,
		read_buf, &read_len, write_len)) {
		if (read_len == 33) {
			last_result_byte = read_buf[0];
			if (read_buf[0] == RESULT_SUCCESS) {
				memcpy(data, &read_buf[1], 16);
				if (pagenum == 0x00) {
					flag_mi_page0_data = 1;
					memcpy(mi_page0_data, data, 16);
				}
				if (pagenum == 0x01) {
					flag_mi_page1_data = 1;
					memcpy(mi_page1_data, data, 16);
				}
				return DS_TRUE;
			} else {
				if (read_buf[0] == 0x55 && pagenum == 2) {
					memcpy(data, &read_buf[1], 16);
					flag_mi_counter = 1;
					memcpy(mi_counter, data, 16);
					return DS_TRUE;
				}
			}
		}
	}

	xm_ow_reset();
	return DS_FALSE;
}

// --------------------------------------------------------------------------
/// 'Compute and Read Page Authentication' command
///
/// @param[in] anon - boolean parameter
/// @param[in] pg - Page number   2,计数器; 0,page0; 1,page1;
/// @param[in] challenge
/// @param[out] hmac   返回的计算结果32个字节
///
/// @return
/// DS_TRUE - command successful @n
/// DS_FALSE - command failed
///
static int DS28E16_cmd_computeReadPageAuthentication(int anon, int pg,
unsigned char *challenge, unsigned char *hmac)
{
	unsigned char write_buf[255];
	unsigned char read_buf[255];
	int write_len = 0;
	int read_len = 33;
	int len_byte = 35;

	last_result_byte = RESULT_FAIL_NONE;

	write_buf[write_len++] = len_byte;
	write_buf[write_len++] = CMD_COMP_READ_AUTH;
	write_buf[write_len] = pg & 0x03;
	if (anon)
		write_buf[write_len] = (write_buf[write_len] | 0xE0);

	write_len++;
	write_buf[write_len++] = 0x02;// Fixed Parameter
	memcpy(&write_buf[write_len], challenge, 32);// Challenge
	write_len += 32;

	ds_info("computeReadPageAuthen:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			write_buf[0], write_buf[1], write_buf[2], write_buf[3],
			write_buf[4], write_buf[5], write_buf[6], write_buf[7],
			write_buf[8], write_buf[9], write_buf[10], write_buf[11],
			write_buf[12], write_buf[13], write_buf[14], write_buf[15],
			write_buf[16], write_buf[17], write_buf[18], write_buf[19],
			write_buf[20], write_buf[21], write_buf[22], write_buf[23],
			write_buf[24], write_buf[25], write_buf[26], write_buf[27],
			write_buf[28], write_buf[29], write_buf[30], write_buf[31],
			write_buf[32], write_buf[33], write_buf[34]);

	if (DS28E16_standard_cmd_flow(write_buf, DELAY_DS28E16_EE_WRITE, read_buf, &read_len, write_len)) {
		last_result_byte = read_buf[0];
		if (read_buf[0] == RESULT_SUCCESS) {
			ds_info("hmac read_buf:\n");
			ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			read_buf[0], read_buf[1], read_buf[2], read_buf[3],
			read_buf[4], read_buf[5], read_buf[6], read_buf[7],
			read_buf[8], read_buf[9], read_buf[10], read_buf[11],
			read_buf[12], read_buf[13], read_buf[14], read_buf[15],
			read_buf[16], read_buf[17], read_buf[18], read_buf[19],
			read_buf[20], read_buf[21], read_buf[22], read_buf[23],
			read_buf[24], read_buf[25], read_buf[26], read_buf[27],
			read_buf[28], read_buf[29], read_buf[30], read_buf[31],read_buf[32]);
			memcpy(hmac, &read_buf[1], 32);
			ds_info("hmac:\n");
			ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			hmac[0], hmac[1], hmac[2], hmac[3],
			hmac[4], hmac[5], hmac[6], hmac[7],
			hmac[8], hmac[9], hmac[10], hmac[11],
			hmac[12], hmac[13], hmac[14], hmac[15],
			hmac[16], hmac[17], hmac[18], hmac[19],
			hmac[20], hmac[21], hmac[22], hmac[23],
			hmac[24], hmac[25], hmac[26], hmac[27],
			hmac[28], hmac[29], hmac[30], hmac[31]);
			return DS_TRUE;
		}
	}

	xm_ow_reset();
	return DS_FALSE;
}

// --------------------------------------------------------------------------
/// 'Compute Secret S' command
///
/// @param[in] anon - boolean parameter
/// @param[in] bdconst - boolean parameter
/// @param[in] pg - Page number   页面
/// @param[in] partial secret   32个字节的种子
///
/// @return
/// DS_TRUE - command successful @n
/// DS_FALSE - command failed
///
static int DS28E16_cmd_computeS_Secret(int anon, int bdconst,
int pg, unsigned char *partial)
{
	unsigned char write_buf[40];
	unsigned char read_buf[40];
	int write_len = 0;
	int read_len = 1;
	int len_byte = 35;
	int param = pg & 0x03;

	last_result_byte = RESULT_FAIL_NONE;

	write_buf[write_len++] = len_byte;
	write_buf[write_len++] = CMD_COMP_S_SECRET;

	if (bdconst)
		param = param | 0x04;
	if (anon)
		param = param | 0xE0;

	write_buf[write_len] = param;
	write_len++;
	write_buf[write_len++] = 0x08;// Fixed Parameter
	memcpy(&write_buf[write_len], partial, 32); // Partial Secret
	write_len += 32;

	ds_info("computeS_Secret:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			write_buf[0], write_buf[1], write_buf[2], write_buf[3],
			write_buf[4], write_buf[5], write_buf[6], write_buf[7],
			write_buf[8], write_buf[9], write_buf[10], write_buf[11],
			write_buf[12], write_buf[13], write_buf[14], write_buf[15],
			write_buf[16], write_buf[17], write_buf[18], write_buf[19],
			write_buf[20], write_buf[21], write_buf[22], write_buf[23],
			write_buf[24], write_buf[25], write_buf[26], write_buf[27],
			write_buf[28], write_buf[29], write_buf[30], write_buf[31],
			write_buf[32], write_buf[33], write_buf[34]);

	if (DS28E16_standard_cmd_flow(write_buf, DELAY_DS28E16_EE_WRITE,
	read_buf, &read_len, write_len)) {
		last_result_byte = read_buf[0];
		if (read_buf[0] == RESULT_SUCCESS)
			return DS_TRUE;
	}

	xm_ow_reset();
	return DS_FALSE;
}

static int AuthenticateDS28E16(int anon, int bdconst, int S_Secret_PageNum, int PageNum,
unsigned char *Challenge, unsigned char *Secret_Seeds, unsigned char *S_Secret)
{
	unsigned char PageData[32], MAC_Read_Value[32], CAL_MAC[32];
	unsigned char status_chip[16];
	unsigned char MAC_Computer_Datainput[128];
	int i = 0;
	int msg_len = 0;
	unsigned char flag = DS_FALSE;

/*
	//if (flag_mi_auth_result)
	if (mi_auth_result == DS_TRUE)
		return mi_auth_result;
*/
	if (anon != ANONYMOUS) {
		if (ds28el16_get_page_status_retry(status_chip) == DS_TRUE) {
			MANID[0] = status_chip[4];
		} else {
			xm_ow_reset();
			return ERROR_R_STATUS;
		}
	}

	if (ds28el16_Read_RomID_retry(mi_romid) != DS_TRUE) {
		xm_ow_reset();
		return ERROR_R_ROMID;
	}

	// DS28E16 calculate its session secret
	flag = DS28E16_cmd_computeS_Secret_retry(anon,
	bdconst, S_Secret_PageNum, Secret_Seeds);
	if (flag == DS_FALSE) {
		ds_err("DS28E16_cmd_computeS_Secret error");
		xm_ow_reset();
		return ERROR_S_SECRET;
	}

	// DS28E16 compute its MAC based on above sessio secret
	flag = DS28E16_cmd_computeReadPageAuthentication_retry(anon,
	PageNum, Challenge, MAC_Read_Value);
	if (flag == DS_FALSE) {
		ds_err("DS28E16_cmd_computeReadPageAuthentication error");
		xm_ow_reset();
		return ERROR_COMPUTE_MAC;
	}

	ds_info("%02x %02x %02x %02x", anon, bdconst, S_Secret_PageNum, PageNum);

	ds_dbg("Seeds:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			Secret_Seeds[0], Secret_Seeds[1], Secret_Seeds[2], Secret_Seeds[3],
			Secret_Seeds[4], Secret_Seeds[5], Secret_Seeds[6], Secret_Seeds[7],
			Secret_Seeds[8], Secret_Seeds[9], Secret_Seeds[10], Secret_Seeds[11],
			Secret_Seeds[12], Secret_Seeds[13], Secret_Seeds[14], Secret_Seeds[15],
			Secret_Seeds[16], Secret_Seeds[17], Secret_Seeds[18], Secret_Seeds[19],
			Secret_Seeds[20], Secret_Seeds[21], Secret_Seeds[22], Secret_Seeds[23],
			Secret_Seeds[24], Secret_Seeds[25], Secret_Seeds[26], Secret_Seeds[27],
			Secret_Seeds[28], Secret_Seeds[29], Secret_Seeds[30], Secret_Seeds[31]);

	ds_dbg("S_Secret:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			S_Secret[0], S_Secret[1], S_Secret[2], S_Secret[3],
			S_Secret[4], S_Secret[5], S_Secret[6], S_Secret[7],
			S_Secret[8], S_Secret[9], S_Secret[10], S_Secret[11],
			S_Secret[12], S_Secret[13], S_Secret[14], S_Secret[15],
			S_Secret[16], S_Secret[17], S_Secret[18], S_Secret[19],
			S_Secret[20], S_Secret[21], S_Secret[22], S_Secret[23],
			S_Secret[24], S_Secret[25], S_Secret[26], S_Secret[27],
			S_Secret[28], S_Secret[29], S_Secret[30], S_Secret[31]);

	ds_dbg("Challenge:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			Challenge[0], Challenge[1], Challenge[2], Challenge[3],
			Challenge[4], Challenge[5], Challenge[6], Challenge[7],
			Challenge[8], Challenge[9], Challenge[10], Challenge[11],
			Challenge[12], Challenge[13], Challenge[14], Challenge[15],
			Challenge[16], Challenge[17], Challenge[18], Challenge[19],
			Challenge[20], Challenge[21], Challenge[22], Challenge[23],
			Challenge[24], Challenge[25], Challenge[26], Challenge[27],
			Challenge[28], Challenge[29], Challenge[30], Challenge[31]);

	ds_dbg("MAC_Read_Value:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			MAC_Read_Value[0], MAC_Read_Value[1], MAC_Read_Value[2], MAC_Read_Value[3],
			MAC_Read_Value[4], MAC_Read_Value[5], MAC_Read_Value[6], MAC_Read_Value[7],
			MAC_Read_Value[8], MAC_Read_Value[9], MAC_Read_Value[10], MAC_Read_Value[11],
			MAC_Read_Value[12], MAC_Read_Value[13], MAC_Read_Value[14], MAC_Read_Value[15],
			MAC_Read_Value[16], MAC_Read_Value[17], MAC_Read_Value[18], MAC_Read_Value[19],
			MAC_Read_Value[20], MAC_Read_Value[21], MAC_Read_Value[22], MAC_Read_Value[23],
			MAC_Read_Value[24], MAC_Read_Value[25], MAC_Read_Value[26], MAC_Read_Value[27],
			MAC_Read_Value[28], MAC_Read_Value[29], MAC_Read_Value[30], MAC_Read_Value[31]);

	// read the page data
	flag = ds28el16_get_page_data_retry(PageNum, PageData);
	if (flag != DS_TRUE) {
		ds_err("DS28E16_cmd_readMemory error");
		xm_ow_reset();
		return ERROR_R_PAGEDATA;
	}

	// insert mi_romid
	if (anon != ANONYMOUS)
		memcpy(MAC_Computer_Datainput, mi_romid, 8);
	else
		memset(MAC_Computer_Datainput, 0xff, 8);
	msg_len += 8;

	// insert Page Data
	memcpy(&MAC_Computer_Datainput[msg_len], PageData, 16);
	msg_len += 16;
	memset(&MAC_Computer_Datainput[msg_len], 0x00, 16);
	msg_len += 16;

	// insert Challenge
	memcpy(&MAC_Computer_Datainput[msg_len], Challenge, 32);
	msg_len += 32;

	// insert Bind Data Page number
	MAC_Computer_Datainput[msg_len] = PageNum & 0x03;
	MAC_Computer_Datainput[msg_len] = MAC_Computer_Datainput[msg_len] & 0x03;
	msg_len += 1;

	// insert MANID
	memcpy(&MAC_Computer_Datainput[msg_len], MANID, 2);
	msg_len += 2;

	// calculate the MAC
	sha3_256_hmac(S_Secret, 32, MAC_Computer_Datainput, msg_len, CAL_MAC);

	ds_dbg("host data:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			MAC_Computer_Datainput[0], MAC_Computer_Datainput[1], MAC_Computer_Datainput[2],
			MAC_Computer_Datainput[3], MAC_Computer_Datainput[4], MAC_Computer_Datainput[5],
			MAC_Computer_Datainput[6], MAC_Computer_Datainput[7], MAC_Computer_Datainput[8],
			MAC_Computer_Datainput[9], MAC_Computer_Datainput[10], MAC_Computer_Datainput[11],
			MAC_Computer_Datainput[12], MAC_Computer_Datainput[13], MAC_Computer_Datainput[14],
			MAC_Computer_Datainput[15],	MAC_Computer_Datainput[16], MAC_Computer_Datainput[17],
			MAC_Computer_Datainput[18], MAC_Computer_Datainput[19], MAC_Computer_Datainput[20],
			MAC_Computer_Datainput[21], MAC_Computer_Datainput[22], MAC_Computer_Datainput[23],
			MAC_Computer_Datainput[24], MAC_Computer_Datainput[25], MAC_Computer_Datainput[26],
			MAC_Computer_Datainput[27], MAC_Computer_Datainput[28], MAC_Computer_Datainput[29],
			MAC_Computer_Datainput[30], MAC_Computer_Datainput[31],	MAC_Computer_Datainput[32],
			MAC_Computer_Datainput[33], MAC_Computer_Datainput[34], MAC_Computer_Datainput[35],
			MAC_Computer_Datainput[36], MAC_Computer_Datainput[37], MAC_Computer_Datainput[38],
			MAC_Computer_Datainput[39],	MAC_Computer_Datainput[40], MAC_Computer_Datainput[41],
			MAC_Computer_Datainput[42], MAC_Computer_Datainput[43], MAC_Computer_Datainput[44],
			MAC_Computer_Datainput[45], MAC_Computer_Datainput[46], MAC_Computer_Datainput[47],
			MAC_Computer_Datainput[48], MAC_Computer_Datainput[49], MAC_Computer_Datainput[50],
			MAC_Computer_Datainput[51], MAC_Computer_Datainput[52], MAC_Computer_Datainput[53],
			MAC_Computer_Datainput[54], MAC_Computer_Datainput[55], MAC_Computer_Datainput[56],
			MAC_Computer_Datainput[57], MAC_Computer_Datainput[58], MAC_Computer_Datainput[59],
			MAC_Computer_Datainput[60], MAC_Computer_Datainput[61], MAC_Computer_Datainput[62],
			MAC_Computer_Datainput[63], MAC_Computer_Datainput[64], MAC_Computer_Datainput[65],
			MAC_Computer_Datainput[66], MAC_Computer_Datainput[67], MAC_Computer_Datainput[68],
			MAC_Computer_Datainput[69], MAC_Computer_Datainput[70], MAC_Computer_Datainput[71],
			MAC_Computer_Datainput[72], MAC_Computer_Datainput[73], MAC_Computer_Datainput[74],
			MAC_Computer_Datainput[75], MAC_Computer_Datainput[76], MAC_Computer_Datainput[77],
			MAC_Computer_Datainput[78], MAC_Computer_Datainput[79]);

	ds_dbg("host mac:\n");
	ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			CAL_MAC[0], CAL_MAC[1], CAL_MAC[2], CAL_MAC[3],
			CAL_MAC[4], CAL_MAC[5], CAL_MAC[6], CAL_MAC[7],
			CAL_MAC[8], CAL_MAC[9], CAL_MAC[10], CAL_MAC[11],
			CAL_MAC[12], CAL_MAC[13], CAL_MAC[14], CAL_MAC[15],
			CAL_MAC[16], CAL_MAC[17], CAL_MAC[18], CAL_MAC[19],
			CAL_MAC[20], CAL_MAC[21], CAL_MAC[22], CAL_MAC[23],
			CAL_MAC[24], CAL_MAC[25], CAL_MAC[26], CAL_MAC[27],
			CAL_MAC[28], CAL_MAC[29], CAL_MAC[30], CAL_MAC[31]);

	for (i = 0; i < 32; i++) {
		if (CAL_MAC[i] != MAC_Read_Value[i])
			break;
	}

	if (i != 32) {
		flag_mi_auth_result = 1;
		mi_auth_result = ERROR_UNMATCH_MAC;
		// for debug
		ds_info("hmac is not match. result=ERROR_UNMATCH_MAC\n");
		flag_mi_page1_data = 0;
		return ERROR_UNMATCH_MAC;
	} else {
		flag_mi_auth_result = 1;
		ds_info("hmac is match. result=DS_TRUE\n");
		mi_auth_result = DS_TRUE;
		return DS_TRUE;
	}
}


// retry interface start //
static int ds28el16_Read_RomID_retry(unsigned char *RomID)
{
	int i;
	unsigned char data[50];

	for (i = 0; i < GET_ROM_ID_RETRY; i++) {
		ds_info("read rom id communication start %d...\n", i);
		if (DS28E16_cmd_readStatus(data) == DS_TRUE) {
			if (Read_RomID(RomID) == DS_TRUE) {
				return DS_TRUE;
			}
		}
	}
	return DS_FALSE;
}

static int ds28el16_get_page_status_retry(unsigned char *data)
{
	int i;

	for (i = 0; i < GET_BLOCK_STATUS_RETRY; i++) {
		ds_info("read page status communication start... %d\n", i);

		if (DS28E16_cmd_readStatus(data) == DS_TRUE) {
			return DS_TRUE;
		}
	}

	return DS_FALSE;
}

/*
static int ds28el16_set_page_status_retry(unsigned char page, unsigned char status)
{
	int i;

	for (i = 0; i < SET_BLOCK_STATUS_RETRY; i++) {
		ds_info("set page status communication start... %d\n", i);

		if (DS28E16_cmd_setPageProtection(page, status) == TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}
*/

static int ds28el16_get_page_data_retry(int page, unsigned char *data)
{
	int i;

/*
	if (page >= MAX_PAGENUM)
		return DS_FALSE;
*/
	for (i = 0; i < GET_USER_MEMORY_RETRY; i++) {
		ds_info("read page data communication start... %d\n", i);

		if (DS28E16_cmd_readMemory(page, data) == DS_TRUE) {
			ds_dbg("page number is %d\n", page);
			ds_dbg("page data:\n");
			ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			data[0], data[1], data[2], data[3],
			data[4], data[5], data[6], data[7],
			data[8], data[9], data[10], data[11],
			data[12], data[13], data[14], data[15]);
			ds_dbg("flag_mi_page0_data is %d\n", flag_mi_page0_data);
			ds_dbg("mi_page0_data data:\n");
			ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			mi_page0_data[0], mi_page0_data[1], mi_page0_data[2], mi_page0_data[3],
			mi_page0_data[4], mi_page0_data[5], mi_page0_data[6], mi_page0_data[7],
			mi_page0_data[8], mi_page0_data[9], mi_page0_data[10], mi_page0_data[11],
			mi_page0_data[12], mi_page0_data[13], mi_page0_data[14], mi_page0_data[15]);
			ds_dbg("flag_mi_page1_data is %d\n", flag_mi_page1_data);
			ds_dbg("mi_page1_data data:\n");
			ds_dbg("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			mi_page1_data[0], mi_page1_data[1], mi_page1_data[2], mi_page1_data[3],
			mi_page1_data[4], mi_page1_data[5], mi_page1_data[6], mi_page1_data[7],
			mi_page1_data[8], mi_page1_data[9], mi_page1_data[10], mi_page1_data[11],
			mi_page1_data[12], mi_page1_data[13], mi_page1_data[14], mi_page1_data[15]);
			ds_dbg("flag_mi_counter is %d\n", flag_mi_counter);
			return DS_TRUE;
		}
	}

	return DS_FALSE;
}

static int DS28E16_cmd_computeS_Secret_retry(int anon, int bdconst,
				int pg, unsigned char *partial)
{
	int i;

	if (pg >= MAX_PAGENUM)
		return DS_FALSE;

	for (i = 0; i < GET_S_SECRET_RETRY; i++) {
		if (DS28E16_cmd_computeS_Secret(anon, bdconst,
				pg, partial) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

static int DS28E16_cmd_computeReadPageAuthentication_retry(int anon, int pg,
				unsigned char *challenge, unsigned char *hmac)
{
	int i;

	if (pg >= MAX_PAGENUM)
		return DS_FALSE;

	for (i = 0; i < GET_MAC_RETRY; i++) {
		if (DS28E16_cmd_computeReadPageAuthentication(anon, pg,
					challenge, hmac) == DS_TRUE)
			return DS_TRUE;
	}

	return DS_FALSE;
}

// retry interface end //

/*
 * 6.1 driver glue. 4.14 exposed the IC as a "batt_verify" power supply
 * with Xiaomi-only properties (POWER_SUPPLY_PROP_AUTHENTIC, PAGE0_DATA,
 * ...) that the fuel gauge relayed to the charger. 6.1 has none of those
 * properties and no userspace reads them, so the result is handed to the
 * charger and fuel gauge through <linux/power/xiaomi_batt_verify.h>.
 * Only the read/authenticate commands are kept: the memory write, counter
 * decrement, page protection and device disable commands are dropped.
 */
struct ds28e16_data {
	struct device *dev;
	struct delayed_work verify_work;
	struct mutex lock;	/* one 1-Wire transaction at a time */
	int result;		/* -EAGAIN until final, then 1 or 0 */
	int count;
};

static struct ds28e16_data *g_ds;
static BLOCKING_NOTIFIER_HEAD(xm_batt_verify_chain);

int xm_batt_verify_authentic(void)
{
	struct ds28e16_data *ds = READ_ONCE(g_ds);

	return ds ? READ_ONCE(ds->result) : -ENODEV;
}
EXPORT_SYMBOL_GPL(xm_batt_verify_authentic);

int xm_batt_verify_page0(u8 *buf)
{
	struct ds28e16_data *ds = READ_ONCE(g_ds);
	int rc;

	if (!ds)
		return -ENODEV;

	mutex_lock(&ds->lock);
	rc = ds28el16_get_page_data_retry(0, buf) == DS_TRUE ? 0 : -EIO;
	mutex_unlock(&ds->lock);

	return rc;
}
EXPORT_SYMBOL_GPL(xm_batt_verify_page0);

int xm_batt_verify_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&xm_batt_verify_chain, nb);
}
EXPORT_SYMBOL_GPL(xm_batt_verify_register_notifier);

int xm_batt_verify_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&xm_batt_verify_chain, nb);
}
EXPORT_SYMBOL_GPL(xm_batt_verify_unregister_notifier);

static void ds28e16_set_result(struct ds28e16_data *ds, bool ok)
{
	WRITE_ONCE(ds->result, ok ? 1 : 0);
	dev_info(ds->dev, "battery %s\n", ok ? "authenticated" :
		 "NOT authenticated");
	blocking_notifier_call_chain(&xm_batt_verify_chain, ok, NULL);
}

#define VERIFY_PERIOD_MS	5000
#define VERIFY_MAX_COUNT	5
static void ds28e16_verify_work(struct work_struct *work)
{
	struct ds28e16_data *ds = container_of(work, struct ds28e16_data,
					       verify_work.work);
	int i, result = DS_FALSE;

	mutex_lock(&ds->lock);
	for (i = 0; i < GET_VERIFY_RETRY; i++) {
		result = AuthenticateDS28E16(auth_ANON, auth_BDCONST, 0,
				pagenumber, challenge, session_seed, S_secret);
		if (result == DS_TRUE)
			break;
	}
	mutex_unlock(&ds->lock);

	if (result != DS_TRUE && ds->count < VERIFY_MAX_COUNT) {
		dev_info(ds->dev, "authentication round %d failed (%d), retrying\n",
			 ds->count, result);
		ds->count++;
		schedule_delayed_work(&ds->verify_work,
				      msecs_to_jiffies(VERIFY_PERIOD_MS));
		return;
	}

	ds28e16_set_result(ds, result == DS_TRUE);
}

static ssize_t authentic_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", xm_batt_verify_authentic());
}
static DEVICE_ATTR_RO(authentic);

static ssize_t romid_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	if (!flag_mi_romid)
		return -ENODATA;
	return sysfs_emit(buf, "%8phN\n", mi_romid);
}
static DEVICE_ATTR_RO(romid);

static struct attribute *ds28e16_attrs[] = {
	&dev_attr_authentic.attr,
	&dev_attr_romid.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ds28e16);

static int ds28e16_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ds28e16_data *ds;

	if (!xm_ow_ready())
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for onewire\n");

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	ds->dev = dev;
	ds->result = -EAGAIN;
	mutex_init(&ds->lock);
	INIT_DELAYED_WORK(&ds->verify_work, ds28e16_verify_work);
	platform_set_drvdata(pdev, ds);
	WRITE_ONCE(g_ds, ds);

	/* ABL already authenticated the battery: don't redo it */
	if (batt_verified_result_from_uefi) {
		dev_info(dev, "result from bootloader\n");
		ds28e16_set_result(ds, true);
		return 0;
	}

	schedule_delayed_work(&ds->verify_work, 0);
	return 0;
}

static int ds28e16_remove(struct platform_device *pdev)
{
	struct ds28e16_data *ds = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ds->verify_work);
	WRITE_ONCE(g_ds, NULL);
	return 0;
}

static const struct of_device_id ds28e16_dt_match[] = {
	{ .compatible = "maxim,ds28e16" },
	{ }
};
MODULE_DEVICE_TABLE(of, ds28e16_dt_match);

static struct platform_driver ds28e16_driver = {
	.driver = {
		.name = "maxim_ds28e16",
		.of_match_table = ds28e16_dt_match,
		.dev_groups = ds28e16_groups,
	},
	.probe = ds28e16_probe,
	.remove = ds28e16_remove,
};
module_platform_driver(ds28e16_driver);

/* androidboot.batt_verified_result=<verified><chip_ok> from Xiaomi ABL */
static int __init early_parse_batt_verified_result(char *p)
{
	if (p) {
		if (!strcmp(p, "11")) {
			batt_verified_result_from_uefi = true;
			batt_chip_ok_result_from_uefi = true;
		} else if (!strcmp(p, "10")) {
			batt_verified_result_from_uefi = true;
			batt_chip_ok_result_from_uefi = false;
		} else if (!strcmp(p, "01")) {
			batt_verified_result_from_uefi = false;
			batt_chip_ok_result_from_uefi = true;
		}
	}

	return 0;
}
early_param("androidboot.batt_verified_result", early_parse_batt_verified_result);

MODULE_AUTHOR("xiaomi Inc.");
MODULE_DESCRIPTION("ds28e16 driver");
MODULE_LICENSE("GPL");
