/*
 *  stk831x.c - Linux kernel modules for sensortek stk8311/stk8312/stk8313
 *  accelerometer
 *
 *  Copyright (C) 2011~2013 Lex Hsieh / sensortek <lex_hsieh@sensortek.com.tw>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/pm_runtime.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/fcntl.h>
#include <linux/syscalls.h>
#include <linux/amlogic/sensor/sensor_common.h>
/*#include <linux/time.h>*/

#if 1
#define debugprintk(x...) printk(x)
#else
#define debugprintk(x...)
#endif


/*#define CONFIG_GRAVITY_STK8312*/

/*#define STK_ALLWINNER_PLATFORM*/
/*#define STK_ALLWINNER_A13*/
/*#define STK_ALLWINNER_A20_A31*/
/*#define STK_ROCKCHIP_PLATFORM*/

#define STK_ACC_DRIVER_VERSION	"1.8.0"
/*choose polling or interrupt mode*/
#define STK_ACC_POLLING_MODE	1
#if (!STK_ACC_POLLING_MODE)
#define ADDITIONAL_GPIO_CFG 1
#define STK_INT_PIN	39
#endif
/*#define STK_PERMISSION_THREAD*/
#define STK_RESUME_RE_INIT
/*#define STK_DEBUG_PRINT*/
/*#define STK_DEBUG_RAWDATA*/
/*#define STK_LOWPASS*/
#define STK_FIR_LEN	4

#define STK_ZG_FILTER
#ifdef CONFIG_GRAVITY_STK8312
#define STK_ZG_COUNT	1
#endif

#define STK_TUNE
#ifdef CONFIG_GRAVITY_STK8312
#define STK_TUNE_XYOFFSET 4
#define STK_TUNE_ZOFFSET 11
#define STK_TUNE_NOISE 5
#endif
#define STK_TUNE_NUM 125
#define STK_TUNE_DELAY 125

#ifndef STK_ALLWINNER_PLATFORM
#ifdef CONFIG_STK8313
#include <linux/amlogic/sensor/stk8313.h>
#elif defined CONFIG_GRAVITY_STK8312
#include <linux/amlogic/sensor/stk8312.h>
#else
#error "What's your stk accelerometer?"
#endif
#else

#include "stk8312.h"

#endif	/* #ifndef STK_ALLWINNER_PLATFORM */

#ifdef STK_ALLWINNER_PLATFORM
#include <mach/system.h>
#include <mach/hardware.h>
#include <mach/sys_config.h>
#endif

/*SYSCALL_DEFINE3(fchmodat, int, dfd, const char __user *,
	filename, mode_t, mode);*/

static struct i2c_client *this_client;

#if defined(STK_LOWPASS)
#define MAX_FIR_LEN 32
struct data_filter {
	s16 raw[MAX_FIR_LEN][3];
	int sum[3];
	int num;
	int idx;
};
#endif

struct stk831x_data {
	struct input_dev *input_dev;
	struct work_struct stk_work;
	int irq;
	int raw_data[3];
	atomic_t enabled;
	unsigned char delay;
	struct mutex write_lock;
	bool first_enable;
	bool re_enable;
	char recv_reg;
#if STK_ACC_POLLING_MODE
	struct hrtimer acc_timer;
	struct work_struct stk_acc_work;
	struct workqueue_struct *stk_acc_wq;
	ktime_t acc_poll_delay;
#endif	/* #if STK_ACC_POLLING_MODE */
	atomic_t cali_status;
#if defined(STK_LOWPASS)
	atomic_t                firlength;
	atomic_t                fir_en;
	struct data_filter      fir;
#endif
};


struct stk831x_range {
	char rng;
	int range;
	int resolution;
} stk8312_range[3] = {
	{0,  3,       6},      /* 1.5*2 */
	{1,  6*2,    8},
	{2,  16*2,  8},
};

#define STK831X_HOLD_ODR
/*2:100Hz, 3:50Hz, 4:25Hz*/
#define STK831X_INIT_ODR		2
#define STK831X_SAMPLE_TIME_MIN_NO		2
#define STK831X_SAMPLE_TIME_NO		5
static const int STK831X_SAMPLE_TIME[STK831X_SAMPLE_TIME_NO] = {
	2500, 5000, 10000, 20000, 40000};
static struct stk831x_data *stk831x_data_ptr;
static int event_since_en;
static int event_since_en_limit = 20;
#if (!STK_ACC_POLLING_MODE)
static struct workqueue_struct *stk_mems_work_queue;
#endif

#define STK_DEBUG_CALI
#define STK_SAMPLE_NO				10
#define STK_ACC_CALI_VER0			0x3D
#define STK_ACC_CALI_VER1			0x01
#define STK_ACC_CALI_FILE			"/data/misc/stkacccali.conf"
#define STK_ACC_CALI_FILE_SIZE			10

#define STK_K_SUCCESS_TUNE			0x04
#define STK_K_SUCCESS_FT2			0x03
#define STK_K_SUCCESS_FT1			0x02
#define STK_K_SUCCESS_FILE			0x01
#define STK_K_NO_CALI				0xFF
#define STK_K_RUNNING				0xFE
#define STK_K_FAIL_LRG_DIFF			0xFD
#define STK_K_FAIL_OPEN_FILE			0xFC
#define STK_K_FAIL_W_FILE			0xFB
#define STK_K_FAIL_R_BACK			0xFA
#define STK_K_FAIL_R_BACK_COMP			0xF9
#define STK_K_FAIL_I2C				0xF8
#define STK_K_FAIL_K_PARA			0xF7
#define STK_K_FAIL_OTP_OUT_RG			0xF6
#define STK_K_FAIL_ENG_I2C			0xF5
#define STK_K_FAIL_FT1_USD			0xF4
#define STK_K_FAIL_FT2_USD			0xF3
#define STK_K_FAIL_WRITE_NOFST			0xF2
#define STK_K_FAIL_OTP_5T			0xF1
#define STK_K_FAIL_PLACEMENT			0xF0


#define POSITIVE_Z_UP		0
#define NEGATIVE_Z_UP		1
#define POSITIVE_X_UP		2
#define NEGATIVE_X_UP		3
#define POSITIVE_Y_UP		4
#define NEGATIVE_Y_UP		5
static unsigned char stk831x_placement = POSITIVE_Z_UP;
#ifdef STK_TUNE
static char stk_tune_offset_record[3] = {0};
static int stk_tune_offset[3] = {0};
static int stk_tune_sum[3] = {0};
static int stk_tune_max[3] = {0};
static int stk_tune_min[3] = {0};
static int stk_tune_index;
static int stk_tune_done;
#endif

static int stk_store_in_ic(struct stk831x_data *stk, char otp_offset[],
	char FT_index, uint32_t delay_ms);
static int32_t stk_get_file_content(char *r_buf, int8_t buf_size);
static int stk_store_in_file(char offset[], char mode);
static int STK831x_ReadByteOTP(char rReg, char *value);
static int STK831x_SetEnable(struct stk831x_data *stk, char en);
static int STK831x_SetCali(struct stk831x_data *stk, char sstate);
static int32_t stk_get_ic_content(struct stk831x_data *stk);
static int STK831x_SetOffset(char buf[]);
static void stk_handle_first_en(struct stk831x_data *stk);
static int STK831x_GetDelay(struct stk831x_data *stk, uint32_t *gdelay_ns);
static int STK831x_SetDelay(struct stk831x_data *stk, uint32_t sdelay_ns);

#ifdef STK_ALLWINNER_PLATFORM
static int gsensor_direct_x;
static int gsensor_direct_y;
static int gsensor_direct_z;
static int gsensor_xy_revert;

enum {
	DEBUG_INIT              = 1U << 0,
	DEBUG_CONTROL_INFO      = 1U << 1,
	DEBUG_DATA_INFO         = 1U << 2,
	DEBUG_SUSPEND           = 1U << 3,
};
static u32 debug_mask;
#define dprintk(level_mask, fmt, arg...) \
do { \
	if (unlikely(debug_mask & level_mask)) \
		debugprintk(KERN_DEBUG fmt , ## arg) \
} while (0)

module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
#endif /* #ifdef STK_ALLWINNER_PLATFORM */

#ifdef STK_ALLWINNER_PLATFORM
/* Addresses to scan */
static union
{
	unsigned short dirty_addr_buf[2];
	const unsigned short normal_i2c[2];
} u_i2c_addr = {{0x00},};
static __u32 twi_id = 1;
#endif

/**
 * gsensor_fetch_sysconfig_para - get config info from sysconfig.fex file.
 * return value:
 *                    = 0; success;
 *                    < 0; err
 */
#ifdef STK_ALLWINNER_A20_A31
static int gsensor_fetch_sysconfig_para(void)
{
	int ret = -1;
	int device_used = -1;
	script_item_u	val;
	script_item_value_type_e  type;

	dprintk(DEBUG_INIT, "========%s===================\n", __func__);

	type = script_get_item("gsensor_para", "gsensor_used", &val);

	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("%s: type err  device_used = %d.\n", __func__, val.val);
		goto script_get_err;
	}
	device_used = val.val;

	if (1 == device_used) {
		type = script_get_item("gsensor_para", "gsensor_twi_id", &val);
		if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
			pr_err("%s: type err twi_id = %d.\n",
				__func__, val.val);
			goto script_get_err;
		}
		twi_id = val.val;

		dprintk(DEBUG_INIT, "%s: twi_id is %d.\n", __func__, twi_id);

		if (SCIRPT_ITEM_VALUE_TYPE_INT !=
			script_get_item("gsensor_para",
			"gsensor_direct_x", &val)){
			pr_err("%s: line: %d: script_get_item err.\n",
				__FILE__, __LINE__);
			goto script_get_err;
		}
		gsensor_direct_x = val.val;

		if (SCIRPT_ITEM_VALUE_TYPE_INT !=
			script_get_item("gsensor_para",
			"gsensor_direct_y", &val)){
			pr_err("%s: line: %d: script_get_item err.\n",
				__FILE__, __LINE__);
			goto script_get_err;
		}
		gsensor_direct_y = val.val;
		if (SCIRPT_ITEM_VALUE_TYPE_INT !=
			script_get_item("gsensor_para",
			"gsensor_direct_z", &val)){
			pr_err("%s: line: %d: script_get_item err.\n",
				__FILE__, __LINE__);
			goto script_get_err;
		}
		gsensor_direct_z = val.val;

		if (SCIRPT_ITEM_VALUE_TYPE_INT !=
			script_get_item("gsensor_para",
			"gsensor_xy_revert", &val)){
			pr_err("%s: line: %d: script_get_item err.\n",
				__FILE__, __LINE__);
			goto script_get_err;
		}
		gsensor_xy_revert = val.val;

		ret = 0;

	} else {
		pr_err("%s: gsensor_unused.\n",  __func__);
		ret = -1;
	}

	return ret;

script_get_err:
	pr_notice("=========script_get_err============\n");
	return ret;
}
#endif /* #ifdef STK_ALLWINNER_A20_A31 */

#ifdef STK_ALLWINNER_A13
static int gsensor_fetch_sysconfig_para(void)
{
	int ret = -1;
	int device_used = -1;

	printk("========%s===================\n", __func__);
	ret = script_parser_fetch("gsensor_para",
		"gsensor_used", &device_used, 1);
	if (SCRIPT_PARSER_OK != ret) {
		pr_err("%s: script_parser_fetch err.ret = %d.\n",
			__func__, ret);
		goto script_parser_fetch_err;
	}
	if (1 == device_used) {
		if (SCRIPT_PARSER_OK != script_parser_fetch("gsensor_para",
			"gsensor_twi_id", &twi_id, 1)){
			pr_err("%s: script_parser_fetch err.\n", __func__);
			goto script_parser_fetch_err;
		}
		debugprintk("%s: twi_id is %d.\n", __func__, twi_id);

		stk8313_pin_hd = gpio_request_ex("gsensor_para", NULL);
		if (stk8313_pin_hd ==  -1)
			debugprintk("stk8313_pin_hd pin request error!\n");
		ret = 0;

	} else {
		pr_err("%s: gsensor_unused.\n",  __func__);
		ret = -1;
	}

	return ret;

script_parser_fetch_err:
	pr_notice("=========script_parser_fetch_err============\n");
	return ret;
}
#endif	/* #ifdef STK_ALLWINNER_A13 */



static int STK_i2c_Rx(char *rxData, int length)
{
	uint8_t retry;
#ifdef STK_ROCKCHIP_PLATFORM
	int scl_clk_rate = 100 * 1000;
#endif
	struct i2c_msg msgs[] = {
		{
			.addr = this_client->addr,
			.flags = 0,
			.len = 1,
			.buf = rxData,
#ifdef STK_ROCKCHIP_PLATFORM
			.scl_rate = scl_clk_rate,
#endif
		},
		{
			.addr = this_client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = rxData,
#ifdef STK_ROCKCHIP_PLATFORM
			.scl_rate = scl_clk_rate,
#endif
		},
	};

	for (retry = 0; retry <= 3; retry++) {
		if (i2c_transfer(this_client->adapter, msgs, 2) > 0)
			break;
		else
			mdelay(10);
	}

	if (retry > 3) {
		debugprintk(KERN_ERR "%s: i2c error, retry over 3\n", __func__);
		return -EIO;
	} else
		return 0;
}

static int STK_i2c_Tx(char *txData, int length)
{
	int retry;
#ifdef STK_ROCKCHIP_PLATFORM
	int scl_clk_rate = 100 * 1000;
#endif
	struct i2c_msg msg[] = {
		{
			.addr = this_client->addr,
			.flags = 0,
			.len = length,
			.buf = txData,
#ifdef STK_ROCKCHIP_PLATFORM
			.scl_rate = scl_clk_rate,
#endif
		},
	};

	for (retry = 0; retry <= 3; retry++) {
		if (i2c_transfer(this_client->adapter, msg, 1) > 0)
			break;
		else
			mdelay(10);
	}

	if (*txData >= 0x21 && *txData <= 0x3E) {
		for (retry = 0; retry <= 3; retry++) {
			if (i2c_transfer(this_client->adapter, msg, 1) > 0)
				break;
			else
				mdelay(20);
		}
	}

	if (retry > 3) {
		debugprintk(KERN_ERR "%s: i2c error, retry over 3\n", __func__);
		return -EIO;
	} else
		return 0;
}


static int STK831X_SetVD(struct stk831x_data *stk)
{
	int result;
	char buffer[2] = "";

	char reg24;

	msleep(20);
	result = STK831x_ReadByteOTP(0x70, &reg24);

	if (result < 0) {
		debugprintk(KERN_ERR "%s: read back error, result=%d\n",
			__func__, result);
		return result;
	}

	if (reg24 != 0) {
		buffer[0] = 0x24;
		buffer[1] = reg24;
		debugprintk(KERN_INFO "%s:write 0x%x to 0x24\n",
			__func__, buffer[1]);
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed\n", __func__);
			return result;
		}
	} else {
		debugprintk(KERN_INFO "%s: reg24=0, do nothing\n", __func__);
		return 0;
	}

	buffer[0] = 0x24;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	if (buffer[0] != reg24) {
		debugprintk(KERN_ERR "%s: error, reg24=0x%x, read=0x%x\n",
			__func__, reg24, buffer[0]);
		return -1;
	}

	debugprintk(KERN_INFO "%s: successfully", __func__);
	return 0;
}

#ifdef STK_TUNE
static void STK831x_ResetPara(void)
{
	int ii;
	for (ii = 0; ii < 3; ii++) {
		stk_tune_sum[ii] = 0;
		stk_tune_min[ii] = 4096;
		stk_tune_max[ii] = -4096;
	}
	return;
}

static void STK831x_Tune(struct stk831x_data *stk, int acc[])
{
	int ii;
	char offset[3];
	char mode_reg;
	int result;
	char buffer[2] = "";

	if (stk_tune_done == 0) {
		if (event_since_en >= STK_TUNE_DELAY) {
			if ((abs(acc[0]) <= STK_TUNE_XYOFFSET) && (abs(acc[1])
				<= STK_TUNE_XYOFFSET)
				&& (abs(abs(acc[2])-STK_LSB_1G)
				<= STK_TUNE_ZOFFSET))
				stk_tune_index++;
			else
				stk_tune_index = 0;

			if (stk_tune_index == 0)
				STK831x_ResetPara();
			else {
				for (ii = 0; ii < 3; ii++) {
					stk_tune_sum[ii] += acc[ii];
					if (acc[ii] > stk_tune_max[ii])
						stk_tune_max[ii] = acc[ii];
					if (acc[ii] < stk_tune_min[ii])
						stk_tune_min[ii] = acc[ii];
				}
			}

			if (stk_tune_index == STK_TUNE_NUM) {
				for (ii = 0; ii < 3; ii++) {
					if ((stk_tune_max[ii] -
						stk_tune_min[ii])
						> STK_TUNE_NOISE) {
						stk_tune_index = 0;
						STK831x_ResetPara();
						return;
					}
				}
				buffer[0] = STK831X_MODE;
				result = STK_i2c_Rx(buffer, 1);
				if (result < 0) {
					debugprintk(KERN_ERR
						"%s:failed, result=0x%x\n",
						__func__, result);
					return;
				}
				mode_reg = buffer[0];
				buffer[1] = mode_reg & 0xF8;
				buffer[0] = STK831X_MODE;
				result = STK_i2c_Tx(buffer, 2);
				if (result < 0) {
					debugprintk(KERN_ERR
						"%s:failed, result=0x%x\n",
						__func__, result);
					return;
				}

				stk_tune_offset[0] =
					stk_tune_sum[0]/STK_TUNE_NUM;
				stk_tune_offset[1] =
					stk_tune_sum[1]/STK_TUNE_NUM;
				if (acc[2] > 0)
					stk_tune_offset[2] =
						stk_tune_sum[2]/STK_TUNE_NUM -
						STK_LSB_1G;
				else
					stk_tune_offset[2] =
						stk_tune_sum[2]/STK_TUNE_NUM -
						(-STK_LSB_1G);

				offset[0] = (char) (-stk_tune_offset[0]);
				offset[1] = (char) (-stk_tune_offset[1]);
				offset[2] = (char) (-stk_tune_offset[2]);
				STK831x_SetOffset(offset);
				stk_tune_offset_record[0] = offset[0];
				stk_tune_offset_record[1] = offset[1];
				stk_tune_offset_record[2] = offset[2];

				buffer[1] = mode_reg | 0x1;
				buffer[0] = STK831X_MODE;
				result = STK_i2c_Tx(buffer, 2);
				if (result < 0) {
					debugprintk(KERN_ERR
						"%s:failed, result=0x%x\n",
						__func__, result);
					return;
				}

				STK831X_SetVD(stk);
				stk_store_in_file(offset, STK_K_SUCCESS_TUNE);
				stk_tune_done = 1;
				atomic_set(&stk->cali_status,
					STK_K_SUCCESS_TUNE);
				event_since_en = 0;
				debugprintk(KERN_INFO
					"%s:TUNE done, %d,%d,%d\n", __func__,
					offset[0], offset[1], offset[2]);
			}
		}
	}

	return;
}
#endif

#ifdef CONFIG_GRAVITY_STK8312
static int STK831x_CheckReading(int acc[], bool clear)
{
	static int check_result;

	if (acc[0] == 127 || acc[0] == -128 || acc[1] == 127 || acc[1] == -128
		|| acc[2] == 127 || acc[2] == -128) {
		debugprintk(KERN_INFO "%s: acc:%o,%o,%o\n", __func__,
			acc[0], acc[1], acc[2]);
		check_result++;
	}
	if (clear) {
		if (check_result == 3) {
			event_since_en_limit = 10000;
			debugprintk(KERN_INFO "%s: incorrect reading\n",
				__func__);
			check_result = 0;
			return 1;
		}
		check_result = 0;
	}
	return 0;
}

static int STK831x_ReadSensorData(struct stk831x_data *stk)
{
	int result;
	char buffer[3] = "";
	int acc_xyz[3] = {0};
#ifdef STK_ZG_FILTER
	s16 zero_fir = 0;
#endif
#ifdef STK_LOWPASS
	int idx, firlength = atomic_read(&stk->firlength);
#endif
	int k_status = atomic_read(&stk->cali_status);
	memset(buffer, 0, 3);

	buffer[0] = STK831X_XOUT;
	result = STK_i2c_Rx(buffer, 3);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:i2c transfer error\n", __func__);
		return result;
	}

	if (buffer[0] & 0x80)
		acc_xyz[0] = buffer[0] - 256;
	else
		acc_xyz[0] = buffer[0];
	if (buffer[1] & 0x80)
		acc_xyz[1] = buffer[1] - 256;
	else
		acc_xyz[1] = buffer[1];
	if (buffer[2] & 0x80)
		acc_xyz[2] = buffer[2] - 256;
	else
		acc_xyz[2] = buffer[2];

#ifdef STK_DEBUG_RAWDATA
	debugprintk(KERN_INFO "%s:RAW  %4d,%4d,%4d\n", __func__,
		acc_xyz[0], acc_xyz[1], acc_xyz[2]);
#endif

	if (event_since_en == 16 || event_since_en == 17)
		STK831x_CheckReading(acc_xyz, false);
	else if (event_since_en == 18)
		STK831x_CheckReading(acc_xyz, true);

	if (k_status == STK_K_RUNNING) {
		stk->raw_data[0] = acc_xyz[0];
		stk->raw_data[1] = acc_xyz[1];
		stk->raw_data[2] = acc_xyz[2];
		return 0;
	}


#ifdef STK_LOWPASS
	if (atomic_read(&stk->fir_en)) {
		if (stk->fir.num < firlength) {
			stk->fir.raw[stk->fir.num][0] = acc_xyz[0];
			stk->fir.raw[stk->fir.num][1] = acc_xyz[1];
			stk->fir.raw[stk->fir.num][2] = acc_xyz[2];
			stk->fir.sum[0] += acc_xyz[0];
			stk->fir.sum[1] += acc_xyz[1];
			stk->fir.sum[2] += acc_xyz[2];
			stk->fir.num++;
			stk->fir.idx++;
		} else {
			idx = stk->fir.idx % firlength;
			stk->fir.sum[0] -= stk->fir.raw[idx][0];
			stk->fir.sum[1] -= stk->fir.raw[idx][1];
			stk->fir.sum[2] -= stk->fir.raw[idx][2];
			stk->fir.raw[idx][0] = acc_xyz[0];
			stk->fir.raw[idx][1] = acc_xyz[1];
			stk->fir.raw[idx][2] = acc_xyz[2];
			stk->fir.sum[0] += acc_xyz[0];
			stk->fir.sum[1] += acc_xyz[1];
			stk->fir.sum[2] += acc_xyz[2];
			stk->fir.idx++;
			acc_xyz[0] = stk->fir.sum[0]/firlength;
			acc_xyz[1] = stk->fir.sum[1]/firlength;
			acc_xyz[2] = stk->fir.sum[2]/firlength;
		}
	}
#ifdef STK_DEBUG_RAWDATA
	debugprintk(KERN_INFO "%s:After FIR  %4d,%4d,%4d\n",
		__func__, acc_xyz[0],
		acc_xyz[1], acc_xyz[2]);
#endif

#endif	/* #ifdef STK_LOWPASS */

#ifdef STK_TUNE
	if ((k_status&0xF0) != 0)
		STK831x_Tune(stk, acc_xyz);
#endif

#ifdef STK_ZG_FILTER
	if (abs(acc_xyz[0]) <= STK_ZG_COUNT)
		acc_xyz[0] = (acc_xyz[0]*zero_fir);
	if (abs(acc_xyz[1]) <= STK_ZG_COUNT)
		acc_xyz[1] = (acc_xyz[1]*zero_fir);
	if (abs(acc_xyz[2]) <= STK_ZG_COUNT)
		acc_xyz[2] = (acc_xyz[2]*zero_fir);
#endif	/* #ifdef STK_ZG_FILTER */

	stk->raw_data[0] = acc_xyz[0];
	stk->raw_data[1] = acc_xyz[1];
	stk->raw_data[2] = acc_xyz[2];

	return 0;
}

#elif defined CONFIG_STK8313
static int STK831x_CheckReading(int acc[], bool clear)
{
	static int check_result;

	if (acc[0] == 2047 || acc[0] == -2048 || acc[1] == 2047 ||
		acc[1] == -2048 ||
		acc[2] == 2047 || acc[2] == -2048) {
		debugprintk(KERN_INFO "%s: acc:%o,%o,%o\n", __func__,
			acc[0], acc[1], acc[2]);
		check_result++;
	}
	if (clear) {
		if (check_result == 3) {
			event_since_en_limit = 10000;
			debugprintk(KERN_INFO "%s: incorrect reading\n",
				__func__);
			check_result = 0;
			return 1;
		}
		check_result = 0;
	}
	return 0;
}

static int STK831x_ReadSensorData(struct stk831x_data *stk)
{
	int result;
	char buffer[6] = "";
	int acc_xyz[3] = {0};
#ifdef STK_ZG_FILTER
	s16 zero_fir = 0;
#endif
#ifdef STK_LOWPASS
	int idx, firlength = atomic_read(&stk->firlength);
#endif
	int k_status = atomic_read(&stk->cali_status);

	memset(buffer, 0, 6);
	buffer[0] = STK831X_XOUT;
	result = STK_i2c_Rx(buffer, 6);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:i2c transfer error\n", __func__);
		return result;
	}

	if (buffer[0] & 0x80)
		acc_xyz[0] = ((int)buffer[0]<<4) + (buffer[1]>>4) - 4096;
	else
		acc_xyz[0] = ((int)buffer[0]<<4) + (buffer[1]>>4);
	if (buffer[2] & 0x80)
		acc_xyz[1] = ((int)buffer[2]<<4) + (buffer[3]>>4) - 4096;
	else
		acc_xyz[1] = ((int)buffer[2]<<4) + (buffer[3]>>4);
	if (buffer[4] & 0x80)
		acc_xyz[2] = ((int)buffer[4]<<4) + (buffer[5]>>4) - 4096;
	else
		acc_xyz[2] = ((int)buffer[4]<<4) + (buffer[5]>>4);

#ifdef STK_DEBUG_RAWDATA
	debugprintk(KERN_INFO "%s:RAW  %4d,%4d,%4d\n", __func__, acc_xyz[0],
		acc_xyz[1], acc_xyz[2]);
#endif

	if (event_since_en == 16 || event_since_en == 17)
		STK831x_CheckReading(acc_xyz, false);
	else if (event_since_en == 18)
		STK831x_CheckReading(acc_xyz, true);
	if (k_status == STK_K_RUNNING) {
		stk->raw_data[0] = acc_xyz[0];
		stk->raw_data[1] = acc_xyz[1];
		stk->raw_data[2] = acc_xyz[2];
		return 0;
	}


#ifdef STK_LOWPASS
	if (atomic_read(&stk->fir_en)) {
		if (stk->fir.num < firlength) {
			stk->fir.raw[stk->fir.num][0] = acc_xyz[0];
			stk->fir.raw[stk->fir.num][1] = acc_xyz[1];
			stk->fir.raw[stk->fir.num][2] = acc_xyz[2];
			stk->fir.sum[0] += acc_xyz[0];
			stk->fir.sum[1] += acc_xyz[1];
			stk->fir.sum[2] += acc_xyz[2];
			stk->fir.num++;
			stk->fir.idx++;
		} else {
			idx = stk->fir.idx % firlength;
			stk->fir.sum[0] -= stk->fir.raw[idx][0];
			stk->fir.sum[1] -= stk->fir.raw[idx][1];
			stk->fir.sum[2] -= stk->fir.raw[idx][2];
			stk->fir.raw[idx][0] = acc_xyz[0];
			stk->fir.raw[idx][1] = acc_xyz[1];
			stk->fir.raw[idx][2] = acc_xyz[2];
			stk->fir.sum[0] += acc_xyz[0];
			stk->fir.sum[1] += acc_xyz[1];
			stk->fir.sum[2] += acc_xyz[2];
			stk->fir.idx++;
			acc_xyz[0] = stk->fir.sum[0]/firlength;
			acc_xyz[1] = stk->fir.sum[1]/firlength;
			acc_xyz[2] = stk->fir.sum[2]/firlength;
		}
	}
#ifdef STK_DEBUG_RAWDATA
	debugprintk(KERN_INFO "%s:After FIR  %4d,%4d,%4d\n",
		__func__, acc_xyz[0],
		acc_xyz[1], acc_xyz[2]);
#endif

#endif	/* #ifdef STK_LOWPASS */


#ifdef STK_TUNE
	if ((k_status&0xF0) != 0)
		STK831x_Tune(stk, acc_xyz);
#endif

#ifdef STK_ZG_FILTER
	if (abs(acc_xyz[0]) <= STK_ZG_COUNT)
		acc_xyz[0] = (acc_xyz[0]*zero_fir);
	if (abs(acc_xyz[1]) <= STK_ZG_COUNT)
		acc_xyz[1] = (acc_xyz[1]*zero_fir);
	if (abs(acc_xyz[2]) <= STK_ZG_COUNT)
		acc_xyz[2] = (acc_xyz[2]*zero_fir);
#endif	/* #ifdef STK_ZG_FILTER */

	stk->raw_data[0] = acc_xyz[0];
	stk->raw_data[1] = acc_xyz[1];
	stk->raw_data[2] = acc_xyz[2];

	return 0;
}
#endif

static int STK831x_ReportValue(struct stk831x_data *stk)
{
#ifdef STK_ALLWINNER_PLATFORM
	int tmp;
#endif
	if (event_since_en < 1200)
		event_since_en++;

	if (event_since_en < event_since_en_limit)
		return 0;
#ifdef STK_ALLWINNER_PLATFORM
	/*gsensor_direct_x = 0;*/
	if (gsensor_direct_x == 1)
		stk->raw_data[0] = -stk->raw_data[0];

	/*gsensor_direct_y = 1;*/
	if (gsensor_direct_y == 1)
		stk->raw_data[1] = -stk->raw_data[1];

	gsensor_direct_z = 1;
	if (gsensor_direct_z == 1)
		stk->raw_data[2] = -stk->raw_data[2];

	if (gsensor_xy_revert == 1) {
		tmp = stk->raw_data[0];
		stk->raw_data[0] = stk->raw_data[1];
		stk->raw_data[1] = tmp;
	}
#endif /* #ifdef STK_ALLWINNER_PLATFORM */

#ifdef STK_DEBUG_PRINT
	debugprintk(KERN_INFO "%s:%4d,%4d,%4d\n", __func__, stk->raw_data[0],
		stk->raw_data[1], stk->raw_data[2]);
#endif
	aml_sensor_report_acc(this_client, stk->input_dev, stk->raw_data[0],
		stk->raw_data[1], stk->raw_data[2]);
	return 0;
}

static int STK831x_SetOffset(char buf[])
{
	int result;
	char buffer[4] = "";

	buffer[0] = STK831X_OFSX;
	buffer[1] = buf[0];
	buffer[2] = buf[1];
	buffer[3] = buf[2];
	result = STK_i2c_Tx(buffer, 4);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	return 0;
}

static int STK831x_GetOffset(char buf[])
{
	int result;
	char buffer[3] = "";

	buffer[0] = STK831X_OFSX;
	result = STK_i2c_Rx(buffer, 3);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	buf[0] = buffer[0];
	buf[1] = buffer[1];
	buf[2] = buffer[2];
	return 0;
}

static int STK831x_SetEnable(struct stk831x_data *stk, char en)
{
	int result;
	char buffer[2] = "";
	int new_enabled = (en)?1:0;
	int k_status = atomic_read(&stk->cali_status);

	if (new_enabled == atomic_read(&stk->enabled))
		return 0;
	debugprintk(KERN_INFO "%s:%x\n", __func__, en);

	if (stk->first_enable && k_status != STK_K_RUNNING)
		stk_handle_first_en(stk);

	mutex_lock(&stk->write_lock);
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto e_err_i2c;
	}
	if (en) {
		buffer[1] = (buffer[0] & 0xF8) | 0x01;
		event_since_en = 0;
#ifdef STK_TUNE
		if ((k_status&0xF0) != 0 && stk_tune_done == 0) {
			stk_tune_index = 0;
			STK831x_ResetPara();
		}
#endif
	} else
		buffer[1] = (buffer[0] & 0xF8);

	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto e_err_i2c;
	}
	mutex_unlock(&stk->write_lock);

	if (stk->first_enable && k_status != STK_K_RUNNING) {
		stk->first_enable = false;
		msleep(20);
		result = stk_get_ic_content(stk);
	}
	if (en) {
		STK831X_SetVD(stk);
#if STK_ACC_POLLING_MODE
		hrtimer_start(&stk->acc_timer, stk->acc_poll_delay,
			HRTIMER_MODE_REL);
#else
		enable_irq((unsigned int)stk->irq);
#endif
	} else {
#if STK_ACC_POLLING_MODE
		hrtimer_cancel(&stk->acc_timer);
		cancel_work_sync(&stk->stk_acc_work);
#else
		disable_irq((unsigned int)stk->irq);
#endif
	}
	atomic_set(&stk->enabled, new_enabled);
	return 0;

e_err_i2c:
	mutex_unlock(&stk->write_lock);
	return result;
}

static int STK831x_GetEnable(struct stk831x_data *stk, char *gState)
{
	*gState = atomic_read(&stk->enabled);
	return 0;
}

static int STK831x_SetDelay(struct stk831x_data *stk, uint32_t sdelay_ns)
{
	unsigned char sr_no;
	int result;
	char buffer[2] = "";
	uint32_t sdelay_us = sdelay_ns / 1000;

	for (sr_no = (STK831X_SAMPLE_TIME_NO-1); sr_no > 0; sr_no--) {
		if (sdelay_us >= STK831X_SAMPLE_TIME[sr_no])
			break;
	}
	if (sr_no < STK831X_SAMPLE_TIME_MIN_NO)
		sr_no = STK831X_SAMPLE_TIME_MIN_NO;

#ifdef STK831X_HOLD_ODR
	sr_no = STK831X_INIT_ODR;
#endif

#ifdef STK_DEBUG_PRINT
#ifdef STK831X_HOLD_ODR
	debugprintk(KERN_INFO "%s:sdelay_us=%d, Hold delay = %d\n",
		__func__, sdelay_us, STK831X_SAMPLE_TIME[STK831X_INIT_ODR]);
#else
	debugprintk(KERN_INFO "%s:sdelay_us=%d\n", __func__, sdelay_us);
#endif
#endif
	mutex_lock(&stk->write_lock);
	if (stk->delay == sr_no) {
		mutex_unlock(&stk->write_lock);
		return 0;
	}
	buffer[0] = STK831X_SR;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto d_err_i2c;
	}

	buffer[1] = (buffer[0] & 0xF8) | ((sr_no & 0x07));
	buffer[0] = STK831X_SR;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto d_err_i2c;
	}
	stk->delay = sr_no;
#if STK_ACC_POLLING_MODE
	stk->acc_poll_delay =
		ns_to_ktime(STK831X_SAMPLE_TIME[sr_no]*USEC_PER_MSEC);
#endif

#if defined(STK_LOWPASS)
	stk->fir.num = 0;
	stk->fir.idx = 0;
	stk->fir.sum[0] = 0;
	stk->fir.sum[1] = 0;
	stk->fir.sum[2] = 0;
#endif
	mutex_unlock(&stk->write_lock);

	return 0;
d_err_i2c:
	mutex_unlock(&stk->write_lock);
	return result;
}

static int STK831x_GetDelay(struct stk831x_data *stk, uint32_t *gdelay_ns)
{
	int result;
	char buffer[2] = "";

	mutex_lock(&stk->write_lock);
	buffer[0] = STK831X_SR;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		mutex_unlock(&stk->write_lock);
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	mutex_unlock(&stk->write_lock);
	*gdelay_ns = (uint32_t) STK831X_SAMPLE_TIME[(int)buffer[0]] * 1000;
	return 0;
}


static int STK831x_SetRange(char srange)
{
	int result;
	char buffer[2] = "";
#ifdef STK_DEBUG_PRINT
	debugprintk(KERN_INFO "%s:range=0x%x\n", __func__, srange);
#endif

	if (srange >= 3) {
		debugprintk(KERN_ERR "%s:parameter out of range\n", __func__);
		return -1;
	}

	buffer[0] = STK831X_STH;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}

	buffer[1] = (buffer[0] & 0x3F) | srange<<6;
	buffer[0] = STK831X_STH;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	return 0;
}

static int STK831x_GetRange(char *grange)
{
	int result;
	char buffer = 0;

	buffer = STK831X_STH;
	result = STK_i2c_Rx(&buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}
	*grange = buffer >> 6;
	return 0;
}

static int STK831x_ReadByteOTP(char rReg, char *value)
{
	int redo = 0;
	int result;
	char buffer[2] = "";
	*value = 0;

	buffer[0] = 0x3D;
	buffer[1] = rReg;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto eng_i2c_r_err;
	}
	buffer[0] = 0x3F;
	buffer[1] = 0x02;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto eng_i2c_r_err;
	}

	do {
		msleep(20);
		buffer[0] = 0x3F;
		result = STK_i2c_Rx(buffer, 1);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed\n", __func__);
			goto eng_i2c_r_err;
		}
		if (buffer[0] & 0x80)
			break;
		redo++;
	} while (redo < 10);

	if (redo == 10) {
		debugprintk(KERN_ERR
			"%s:OTP read repeat read 10 times! Failed!\n",
			__func__);
		return -STK_K_FAIL_OTP_5T;
	}
	buffer[0] = 0x3E;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		goto eng_i2c_r_err;
	}
	*value = buffer[0];
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s: read 0x%x=0x%x\n", __func__, rReg, *value);
#endif
	return 0;

eng_i2c_r_err:
	return -STK_K_FAIL_ENG_I2C;
}

static int STK831x_WriteByteOTP(char wReg, char value)
{
	int finish_w_check = 0;
	int result;
	char buffer[2] = "";
	char read_back, value_xor = value;
	int re_write = 0;

	do {
		finish_w_check = 0;
		buffer[0] = 0x3D;
		buffer[1] = wReg;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, err=0x%x\n",
				__func__, result);
			goto eng_i2c_w_err;
		}
		buffer[0] = 0x3E;
		buffer[1] = value_xor;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, err=0x%x\n",
				__func__, result);
			goto eng_i2c_w_err;
		}
		buffer[0] = 0x3F;
		buffer[1] = 0x01;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, err=0x%x\n",
				__func__, result);
			goto eng_i2c_w_err;
		}

		do {
			msleep(20);
			buffer[0] = 0x3F;
			result = STK_i2c_Rx(buffer, 1);
			if (result < 0) {
				debugprintk(KERN_ERR "%s:failed, err=0x%x\n",
					__func__, result);
				goto eng_i2c_w_err;
			}
			if (buffer[0] & 0x80) {
				result = STK831x_ReadByteOTP(wReg, &read_back);
				if (result < 0) {
					debugprintk(KERN_ERR
						"read back error, result=%d\n",
						 result);
					goto eng_i2c_w_err;
				}

				if (read_back == value) {
#ifdef STK_DEBUG_CALI
					debugprintk(KERN_INFO
						"%s: write 0x%x=0x%x success\n",
						__func__, wReg, value);
#endif
					re_write = 0xFF;
					break;
				} else {
					debugprintk(KERN_ERR
						"%s: write 0x%x=0x%x,",
						__func__, wReg, value_xor);
					debugprintk(KERN_ERR
						"read 0x%x=0x%x,try again\n",
						wReg, read_back);
					value_xor = read_back ^ value;
					re_write++;
					break;
				}
			}
			finish_w_check++;
		} while (finish_w_check < 5);
	} while (re_write < 10);

	if (re_write == 10) {
		debugprintk(KERN_ERR
			"write 0x%x fail, read=0x%x, write=0x%x, target=0x%x\n",
			wReg, read_back, value_xor, value);
		return -STK_K_FAIL_OTP_5T;
	}

	return 0;

eng_i2c_w_err:
	return -STK_K_FAIL_ENG_I2C;
}

static int STK831x_WriteOffsetOTP(struct stk831x_data *stk, int FT,
	char offsetData[])
{
	char regR[6], reg_comp[3];
	char mode;
	int result;
	char buffer[2] = "";
	int ft_pre_trim = 0;

	if (FT == 1) {
		result = STK831x_ReadByteOTP(0x7F, &regR[0]);
		if (result < 0)
			goto eng_i2c_err;

		if (regR[0]&0x10) {
			debugprintk(KERN_ERR "%s: 0x7F=0x%x\n",
				__func__, regR[0]);
			return -STK_K_FAIL_FT1_USD;
		}
	} else if (FT == 2) {
		result = STK831x_ReadByteOTP(0x7F, &regR[0]);
		if (result < 0)
			goto eng_i2c_err;

		if (regR[0]&0x20) {
			debugprintk(KERN_ERR "%s: 0x7F=0x%x\n",
				__func__, regR[0]);
			return -STK_K_FAIL_FT2_USD;
		}
	}

	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	mode = buffer[0];
	buffer[1] = (mode | 0x01);
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto common_i2c_error;
	msleep(20);


	if (FT == 1) {
		result = STK831x_ReadByteOTP(0x40, &reg_comp[0]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_ReadByteOTP(0x41, &reg_comp[1]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_ReadByteOTP(0x42, &reg_comp[2]);
		if (result < 0)
			goto eng_i2c_err;
	} else if (FT == 2) {
		result = STK831x_ReadByteOTP(0x50, &reg_comp[0]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_ReadByteOTP(0x51, &reg_comp[1]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_ReadByteOTP(0x52, &reg_comp[2]);
		if (result < 0)
			goto eng_i2c_err;
	}

	result = STK831x_ReadByteOTP(0x30, &regR[0]);
	if (result < 0)
		goto eng_i2c_err;
	result = STK831x_ReadByteOTP(0x31, &regR[1]);
	if (result < 0)
		goto eng_i2c_err;
	result = STK831x_ReadByteOTP(0x32, &regR[2]);
	if (result < 0)
		goto eng_i2c_err;

	if (reg_comp[0] == regR[0] && reg_comp[1] == regR[1] &&
		reg_comp[2] == regR[2]) {
		debugprintk(KERN_INFO "%s: ft pre-trimmed\n", __func__);
		ft_pre_trim = 1;
	}

	if (!ft_pre_trim) {
		if (FT == 1) {
			result = STK831x_WriteByteOTP(0x40, regR[0]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x41, regR[1]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x42, regR[2]);
			if (result < 0)
				goto eng_i2c_err;
		} else if (FT == 2) {
			result = STK831x_WriteByteOTP(0x50, regR[0]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x51, regR[1]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x52, regR[2]);
			if (result < 0)
				goto eng_i2c_err;
		}
	}
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s:OTP step1 Success!\n", __func__);
#endif
	buffer[0] = 0x2A;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[0] = buffer[0];
	buffer[0] = 0x2B;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[1] = buffer[0];
	buffer[0] = 0x2E;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[2] = buffer[0];
	buffer[0] = 0x2F;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[3] = buffer[0];
	buffer[0] = 0x32;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[4] = buffer[0];
	buffer[0] = 0x33;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto common_i2c_error;
	else
		regR[5] = buffer[0];

	regR[1] = offsetData[0];
	regR[3] = offsetData[2];
	regR[5] = offsetData[1];
	if (FT == 1) {
		result = STK831x_WriteByteOTP(0x44, regR[1]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_WriteByteOTP(0x46, regR[3]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_WriteByteOTP(0x48, regR[5]);
		if (result < 0)
			goto eng_i2c_err;

		if (!ft_pre_trim) {
			result = STK831x_WriteByteOTP(0x43, regR[0]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x45, regR[2]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x47, regR[4]);
			if (result < 0)
				goto eng_i2c_err;
		}
	} else if (FT == 2) {
		result = STK831x_WriteByteOTP(0x54, regR[1]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_WriteByteOTP(0x56, regR[3]);
		if (result < 0)
			goto eng_i2c_err;
		result = STK831x_WriteByteOTP(0x58, regR[5]);
		if (result < 0)
			goto eng_i2c_err;

		if (!ft_pre_trim) {
			result = STK831x_WriteByteOTP(0x53, regR[0]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x55, regR[2]);
			if (result < 0)
				goto eng_i2c_err;
			result = STK831x_WriteByteOTP(0x57, regR[4]);
			if (result < 0)
				goto eng_i2c_err;
		}
	}
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s:OTP step2 Success!\n", __func__);
#endif
	result = STK831x_ReadByteOTP(0x7F, &regR[0]);
	if (result < 0)
		goto eng_i2c_err;

	if (FT == 1)
		regR[0] = regR[0]|0x10;
	else if (FT == 2)
		regR[0] = regR[0]|0x20;

	result = STK831x_WriteByteOTP(0x7F, regR[0]);
	if (result < 0)
		goto eng_i2c_err;
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s:OTP step3 Success!\n", __func__);
#endif
	return 0;

eng_i2c_err:
	debugprintk(KERN_ERR "%s: read/write eng i2c error, result=0x%x\n",
		__func__, result);
	return result;

common_i2c_error:
	debugprintk(KERN_ERR "%s: read/write common i2c error, result=0x%x\n",
		__func__, result);
	return result;
}

static int STK831X_VerifyCali(struct stk831x_data *stk, unsigned char en_dis,
	uint32_t delay_ms)
{
	unsigned char axis, state;
	int acc_ave[3] = {0, 0, 0};
	const unsigned char verify_sample_no = 3;
	const unsigned char verify_diff = 2;
	int result;
	char buffer[2] = "";
	int ret = 0;

	if (en_dis) {
		STK831x_SetDelay(stk, 10000000);
		buffer[0] = STK831X_MODE;
		result = STK_i2c_Rx(buffer, 1);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, result=0x%x\n",
				__func__, result);
			return -STK_K_FAIL_I2C;
		}
		buffer[1] = (buffer[0] & 0xF8) | 0x01;
		buffer[0] = STK831X_MODE;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, result=0x%x\n",
				__func__, result);
			return -STK_K_FAIL_I2C;
		}
		STK831X_SetVD(stk);
		msleep(delay_ms*15);
	}

	for (state = 0; state < verify_sample_no; state++) {
		STK831x_ReadSensorData(stk);
		for (axis = 0; axis < 3; axis++)
			acc_ave[axis] += stk->raw_data[axis];
#ifdef STK_DEBUG_CALI
		debugprintk(KERN_INFO "%s: acc=%d,%d,%d\n",
			__func__, stk->raw_data[0],
			stk->raw_data[1], stk->raw_data[2]);
#endif
		msleep(delay_ms);
	}

	for (axis = 0; axis < 3; axis++)
		acc_ave[axis] /= verify_sample_no;

	switch (stk831x_placement)  {
	case POSITIVE_X_UP:
		acc_ave[0] -= STK_LSB_1G;
		break;
	case NEGATIVE_X_UP:
		acc_ave[0] += STK_LSB_1G;
		break;
	case POSITIVE_Y_UP:
		acc_ave[1] -= STK_LSB_1G;
		break;
	case NEGATIVE_Y_UP:
		acc_ave[1] += STK_LSB_1G;
		break;
	case POSITIVE_Z_UP:
		acc_ave[2] -= STK_LSB_1G;
		break;
	case NEGATIVE_Z_UP:
		acc_ave[2] += STK_LSB_1G;
		break;
	default:
		debugprintk("%s: invalid stk831x_placement=%d\n",
			__func__, stk831x_placement);
		ret = -STK_K_FAIL_PLACEMENT;
		break;
	}
	if (abs(acc_ave[0]) > verify_diff || abs(acc_ave[1]) > verify_diff ||
		abs(acc_ave[2]) > verify_diff) {
		debugprintk(KERN_INFO "%s:Check data x:%d, y:%d, z:%d\n",
			__func__, acc_ave[0], acc_ave[1], acc_ave[2]);
		debugprintk(KERN_ERR "%s:Check Fail, Calibration Fail\n",
			__func__);
		ret = -STK_K_FAIL_LRG_DIFF;
	}
#ifdef STK_DEBUG_CALI
	else
		debugprintk(KERN_INFO "%s:Check data pass\n", __func__);
#endif
	if (en_dis) {
		buffer[0] = STK831X_MODE;
		result = STK_i2c_Rx(buffer, 1);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, result=0x%x\n",
				__func__, result);
			return -STK_K_FAIL_I2C;
		}
		buffer[1] = (buffer[0] & 0xF8);
		buffer[0] = STK831X_MODE;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0) {
			debugprintk(KERN_ERR "%s:failed, result=0x%x\n",
				__func__, result);
			return -STK_K_FAIL_I2C;
		}
	}

	return ret;
}


static int STK831x_SetCali(struct stk831x_data *stk, char sstate)
{
	char org_enable;
	int acc_ave[3] = {0, 0, 0};
	int state, axis;
	int new_offset[3];
	char char_offset[3] = {0};
	int result;
	char buffer[2] = "";
	char reg_offset[3] = {0};
	char store_location = sstate;
	uint32_t gdelay_ns, real_delay_ms;
	char offset[3];

	atomic_set(&stk->cali_status, STK_K_RUNNING);
	/*sstate=1, STORE_OFFSET_IN_FILE*/
	/*sstate=2, STORE_OFFSET_IN_IC		*/
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s:store_location=%d\n",
		__func__, store_location);
#endif
	if ((store_location != 3 && store_location != 2 && store_location != 1)
		|| (stk831x_placement < 0 || stk831x_placement > 5)) {
		debugprintk(KERN_ERR "%s, invalid parameters\n", __func__);
		atomic_set(&stk->cali_status, STK_K_FAIL_K_PARA);
		return -STK_K_FAIL_K_PARA;
	}
	STK831x_GetDelay(stk, &gdelay_ns);
	STK831x_GetEnable(stk, &org_enable);
	if (org_enable)
		STK831x_SetEnable(stk, 0);
	STK831x_SetDelay(stk, 10000000);
	msleep(20);
	STK831x_GetDelay(stk, &real_delay_ms);
	real_delay_ms = (real_delay_ms + (NSEC_PER_MSEC / 2)) / NSEC_PER_MSEC;
	debugprintk(KERN_INFO "%s: delay =%d ms\n", __func__, real_delay_ms);

	STK831x_SetOffset(reg_offset);
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto err_i2c_rw;
	buffer[1] = (buffer[0] & 0xF8) | 0x01;
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto err_i2c_rw;

	STK831X_SetVD(stk);
	if (store_location >= 2) {
		buffer[0] = 0x2B;
		buffer[1] = 0x0;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0)
			goto err_i2c_rw;
		buffer[0] = 0x2F;
		buffer[1] = 0x0;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0)
			goto err_i2c_rw;
		buffer[0] = 0x33;
		buffer[1] = 0x0;
		result = STK_i2c_Tx(buffer, 2);
		if (result < 0)
			goto err_i2c_rw;
	}

	msleep(real_delay_ms*20);
	for (state = 0; state < STK_SAMPLE_NO; state++) {
		STK831x_ReadSensorData(stk);
		for (axis = 0; axis < 3; axis++)
			acc_ave[axis] += stk->raw_data[axis];
#ifdef STK_DEBUG_CALI
		debugprintk(KERN_INFO "%s: acc=%d,%d,%d\n",
			__func__, stk->raw_data[0],
			stk->raw_data[1], stk->raw_data[2]);
#endif
		msleep(real_delay_ms);
	}
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto err_i2c_rw;
	buffer[1] = (buffer[0] & 0xF8);
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto err_i2c_rw;

	for (axis = 0; axis < 3; axis++) {
		if (acc_ave[axis] >= 0)
			acc_ave[axis] = (acc_ave[axis] + STK_SAMPLE_NO / 2) /
				STK_SAMPLE_NO;
		else
			acc_ave[axis] = (acc_ave[axis] - STK_SAMPLE_NO / 2) /
				STK_SAMPLE_NO;

	}
	if (acc_ave[2] <= -1)
		stk831x_placement = NEGATIVE_Z_UP;
	else if ((acc_ave[2] >= 1))
		stk831x_placement = POSITIVE_Z_UP;
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s:stk831x_placement=%d\n",
		__func__, stk831x_placement);
#endif

	switch (stk831x_placement) {
	case POSITIVE_X_UP:
		acc_ave[0] -= STK_LSB_1G;
		break;
	case NEGATIVE_X_UP:
		acc_ave[0] += STK_LSB_1G;
		break;
	case POSITIVE_Y_UP:
		acc_ave[1] -= STK_LSB_1G;
		break;
	case NEGATIVE_Y_UP:
		acc_ave[1] += STK_LSB_1G;
		break;
	case POSITIVE_Z_UP:
		acc_ave[2] -= STK_LSB_1G;
		break;
	case NEGATIVE_Z_UP:
		acc_ave[2] += STK_LSB_1G;
		break;
	default:
		debugprintk("%s: invalid stk831x_placement=%d\n",
			__func__, stk831x_placement);
		atomic_set(&stk->cali_status, STK_K_FAIL_PLACEMENT);
		return -STK_K_FAIL_K_PARA;
		break;
	}

	for (axis = 0; axis < 3; axis++) {
		acc_ave[axis] = -acc_ave[axis];
		new_offset[axis] = acc_ave[axis];
		char_offset[axis] = new_offset[axis];
	}
#ifdef STK_DEBUG_CALI
	debugprintk(KERN_INFO "%s: New offset:%d,%d,%d\n",
		__func__, new_offset[0], new_offset[1], new_offset[2]);
#endif
	if (store_location == 1) {
		STK831x_SetOffset(char_offset);
		msleep(20);
		STK831x_GetOffset(reg_offset);
		for (axis = 0; axis < 3; axis++) {
			if (char_offset[axis] != reg_offset[axis]) {
				debugprintk(KERN_ERR
					"%s: set offset to register fail!,",
					__func__);
				debugprintk(KERN_ERR
					"char_ofset[%d]=%d,reg_offset[%d]=%d\n",
					axis, char_offset[axis], axis,
					reg_offset[axis]);
				atomic_set(&stk->cali_status,
					STK_K_FAIL_WRITE_NOFST);
				return -STK_K_FAIL_WRITE_NOFST;
			}
		}


		result = STK831X_VerifyCali(stk, 1, real_delay_ms);
		if (result) {
			debugprintk(KERN_ERR
				"%s: calibration check fail, result=0x%x\n",
				__func__, result);
			atomic_set(&stk->cali_status, -result);
		} else {
			result = stk_store_in_file(char_offset,
				STK_K_SUCCESS_FILE);
			if (result) {
				debugprintk(KERN_INFO
					"%s:write calibration failed\n",
					__func__);
				atomic_set(&stk->cali_status, -result);
			} else {
				debugprintk(KERN_INFO "%s successfully\n",
					__func__);
				atomic_set(&stk->cali_status,
					STK_K_SUCCESS_FILE);
			}

		}
	} else if (store_location >= 2) {
		for (axis = 0; axis < 3; axis++) {
			char_offset[axis] = (char)new_offset[axis];
			if ((char_offset[axis]>>7) == 0) {
				if (char_offset[axis] >= 0x20) {
					debugprintk(KERN_ERR
						"%s: offset[%d]=0x%x",
						__func__, axis,
						char_offset[axis]);
					debugprintk(KERN_ERR
						"is large, limit to 0x1f\n");
					/*atomic_set(&stk->cali_status,
						STK_K_FAIL_OTP_OUT_RG);	*/
					/*return -STK_K_FAIL_OTP_OUT_RG;*/
				}
			} else {
				if (char_offset[axis] <= 0xDF) {
					debugprintk(KERN_ERR
						"%s: offset[%d]=0x%x", __func__,
						axis, char_offset[axis]);
					debugprintk(KERN_ERR
						"is large, limit to 0x20\n");
						char_offset[axis] = 0x20;
					/*atomic_set(&stk->cali_status,
						STK_K_FAIL_OTP_OUT_RG);	*/
					/*return -STK_K_FAIL_OTP_OUT_RG;*/
				} else
					char_offset[axis] =
						char_offset[axis] & 0x3f;
			}
		}

		debugprintk(KERN_INFO "%s: OTP offset:0x%x,0x%x,0x%x\n",
			__func__, char_offset[0],
			char_offset[1], char_offset[2]);
		if (store_location == 2) {
			result = stk_store_in_ic(stk,
				char_offset, 1, real_delay_ms);
			if (result == 0) {
				debugprintk(KERN_INFO
					"%s successfully\n", __func__);
				atomic_set(&stk->cali_status,
					STK_K_SUCCESS_FT1);
			} else
				debugprintk(KERN_ERR "%s fail, result=%d\n",
					__func__, result);
		} else if (store_location == 3) {
			result = stk_store_in_ic(stk, char_offset, 2,
				real_delay_ms);
			if (result == 0) {
				debugprintk(KERN_INFO "%s successfully\n",
					__func__);
				atomic_set(&stk->cali_status,
					STK_K_SUCCESS_FT2);
			} else
				debugprintk(KERN_ERR "%s fail, result=%d\n",
					__func__, result);
		}
		offset[0] = offset[1] = offset[2] = 0;
		stk_store_in_file(offset, store_location);
	}
#ifdef STK_TUNE
	stk_tune_offset_record[0] = 0;
	stk_tune_offset_record[1] = 0;
	stk_tune_offset_record[2] = 0;
	stk_tune_done = 1;
#endif
	stk->first_enable = false;
	STK831x_SetDelay(stk, gdelay_ns);

	if (org_enable)
		STK831x_SetEnable(stk, 1);
	return 0;

err_i2c_rw:
	stk->first_enable = false;
	if (org_enable)
		STK831x_SetEnable(stk, 1);
	debugprintk(KERN_ERR "%s: i2c read/write error, err=0x%x\n",
		__func__, result);
	atomic_set(&stk->cali_status, STK_K_FAIL_I2C);
	return result;
}


static int STK831x_GetCali(struct stk831x_data *stk)
{
	char r_buf[STK_ACC_CALI_FILE_SIZE] = {0};
	char offset[3], mode;
	int cnt, result;
	char regR[6];

#ifdef STK_TUNE
	debugprintk(KERN_INFO
		"tune_done=%d, tune_index=%d, tune_offset=%d,%d,%d\n",
		stk_tune_done, stk_tune_index, stk_tune_offset_record[0],
		stk_tune_offset_record[1], stk_tune_offset_record[2]);
#endif
	if ((stk_get_file_content(r_buf, STK_ACC_CALI_FILE_SIZE)) == 0) {
		if (r_buf[0] == STK_ACC_CALI_VER0 && r_buf[1] ==
			STK_ACC_CALI_VER1) {
			offset[0] = r_buf[2];
			offset[1] = r_buf[3];
			offset[2] = r_buf[4];
			mode = r_buf[5];
			debugprintk(KERN_INFO
				"%s:file offset:%#02x,%#02x,%#02x, %#02x\n",
				__func__, offset[0],
				offset[1], offset[2], mode);
		} else {
			debugprintk(KERN_ERR
				"%s: cali version number error!", __func__);
			debugprintk(KERN_ERR
				"r_buf=0x%x,0x%x,0x%x,0x%x,0x%x\n",
				 r_buf[0], r_buf[1], r_buf[2],
				 r_buf[3], r_buf[4]);
		}
	} else
		debugprintk(KERN_INFO "%s: No file offset\n", __func__);

	for (cnt = 0x43; cnt < 0x49; cnt++) {
		result = STK831x_ReadByteOTP(cnt, &(regR[cnt-0x43]));
		if (result < 0)
			debugprintk(KERN_ERR
				"%s: STK831x_ReadByteOTP failed, ret=%d\n",
				__func__, result);
	}
	debugprintk(KERN_INFO
		"%s: OTP 0x43-0x49:%#02x,%#02x,%#02x,%#02x, %#02x,%#02x\n",
		__func__, regR[0], regR[1], regR[2], regR[3], regR[4], regR[5]);

	for (cnt = 0x53; cnt < 0x59; cnt++) {
		result = STK831x_ReadByteOTP(cnt, &(regR[cnt-0x53]));
		if (result < 0)
			debugprintk(KERN_ERR
				"%s: STK831x_ReadByteOTP failed, ret=%d\n",
				__func__, result);
	}
	debugprintk(KERN_INFO
		"%s: OTP 0x53-0x59:%#02x,%#02x,%#02x, %#02x,%#02x,%#02x\n",
		__func__, regR[0],
		regR[1], regR[2], regR[3], regR[4], regR[5]);

	return 0;
}

static int STK831x_Init(struct stk831x_data *stk, struct i2c_client *client)
{
	int result;
	char buffer[2] = "";

#ifdef CONFIG_GRAVITY_STK8312
	debugprintk(KERN_INFO "%s: Initialize stk8312\n", __func__);
#elif defined CONFIG_STK8313
	debugprintk(KERN_INFO "%s: Initialize stk8313\n", __func__);
#endif

	buffer[0] = STK831X_RESET;
	buffer[1] = 0x00;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}

	/* int pin is active high, psuh-pull */
	buffer[0] = STK831X_MODE;
	buffer[1] = 0xC0;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}

	/* 50 Hz ODR */
	stk->delay = STK831X_INIT_ODR;
	buffer[0] = STK831X_SR;
	buffer[1] = stk->delay;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return result;
	}

#if (!STK_ACC_POLLING_MODE)
	/* enable GINT, int after every measurement */
	buffer[0] = STK831X_INTSU;
	buffer[1] = 0x10;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:interrupt init failed\n", __func__);
		return result;
	}
#endif
	buffer[0] = STK831X_STH;
	/* +- 6g mode */
	buffer[1] = 0x42;

	result = STK_i2c_Tx(buffer, 2);
	if (result < 0) {
		debugprintk(KERN_ERR "%s:set range failed\n", __func__);
		return result;
	}

	atomic_set(&stk->enabled, 0);
	event_since_en = 0;

#ifdef STK_LOWPASS
	memset(&stk->fir, 0x00, sizeof(stk->fir));
	atomic_set(&stk->firlength, STK_FIR_LEN);
	atomic_set(&stk->fir_en, 1);
#endif

#ifdef STK_TUNE
	stk_tune_offset[0] = 0;
	stk_tune_offset[1] = 0;
	stk_tune_offset[2] = 0;
	stk_tune_done = 0;
#endif
	return 0;
}

static void stk_handle_first_en(struct stk831x_data *stk)
{
	char r_buf[STK_ACC_CALI_FILE_SIZE] = {0};
	char offset[3];
	char mode;

	if ((stk_get_file_content(r_buf, STK_ACC_CALI_FILE_SIZE)) == 0) {
		if (r_buf[0] == STK_ACC_CALI_VER0 && r_buf[1] ==
			STK_ACC_CALI_VER1) {
			offset[0] = r_buf[2];
			offset[1] = r_buf[3];
			offset[2] = r_buf[4];
			mode = r_buf[5];
			STK831x_SetOffset(offset);
#ifdef STK_TUNE
			stk_tune_offset_record[0] = offset[0];
			stk_tune_offset_record[1] = offset[1];
			stk_tune_offset_record[2] = offset[2];
#endif
			debugprintk(KERN_INFO
				"%s: set offset:%d,%d,%d, mode=%d\n",
				__func__, offset[0],
				offset[1], offset[2], mode);
			atomic_set(&stk->cali_status, mode);
		} else {
			debugprintk(KERN_ERR
				"%s: cali version number error!", __func__);
			debugprintk(KERN_ERR
				"r_buf=0x%x,0x%x,0x%x,0x%x,0x%x\n",
				 r_buf[0], r_buf[1], r_buf[2],
				r_buf[3], r_buf[4]);
			/*return -EINVAL;*/
		}
	}
#ifdef STK_TUNE
	else if (stk_tune_offset_record[0] != 0 || stk_tune_offset_record[1]
		!= 0 || stk_tune_offset_record[2] != 0) {
		STK831x_SetOffset(stk_tune_offset_record);
		stk_tune_done = 1;
		atomic_set(&stk->cali_status, STK_K_SUCCESS_TUNE);
		debugprintk(KERN_INFO "%s: set offset:%d,%d,%d\n",
			__func__, stk_tune_offset_record[0],
			stk_tune_offset_record[1], stk_tune_offset_record[2]);
	}
#endif
	else {
		offset[0] = offset[1] = offset[2] = 0;
		stk_store_in_file(offset, STK_K_NO_CALI);
		atomic_set(&stk->cali_status, STK_K_NO_CALI);
	}
	debugprintk(KERN_INFO "%s: finish, cali_status = 0x%x\n",
		__func__, atomic_read(&stk->cali_status));
	return;
}

static int32_t stk_get_ic_content(struct stk831x_data *stk)
{
	int result;
	char regR;

	result = STK831x_ReadByteOTP(0x7F, &regR);
	if (result < 0) {
		debugprintk(KERN_ERR
			"%s: read/write eng i2c error, result=0x%x\n",
			__func__, result);
		return result;
	}

	if (regR&0x20) {
		atomic_set(&stk->cali_status, STK_K_SUCCESS_FT2);
		debugprintk(KERN_INFO "%s: OTP 2 used\n", __func__);
		return 2;
	}
	if (regR&0x10) {
		atomic_set(&stk->cali_status, STK_K_SUCCESS_FT1);
		debugprintk(KERN_INFO "%s: OTP 1 used\n", __func__);
		return 1;
	}
	return 0;
}

static int stk_store_in_ic(struct stk831x_data *stk, char otp_offset[],
	char FT_index, uint32_t delay_ms)
{
	int result;
	char buffer[2] = "";

	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto ic_err_i2c_rw;
	buffer[1] = (buffer[0] & 0xF8) | 0x01;
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto ic_err_i2c_rw;
	STK831X_SetVD(stk);

	buffer[0] = 0x2B;
	buffer[1] = otp_offset[0];
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto ic_err_i2c_rw;
	buffer[0] = 0x2F;
	buffer[1] = otp_offset[2];
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto ic_err_i2c_rw;
	buffer[0] = 0x33;
	buffer[1] = otp_offset[1];
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto ic_err_i2c_rw;


#ifdef STK_DEBUG_CALI
	/*debugprintk(KERN_INFO "%s:Check All OTP Data after
		write 0x2B 0x2F 0x33\n", __func__);*/
	/*STK831x_ReadAllOTP();*/
#endif

	msleep(delay_ms*15);
	result = STK831X_VerifyCali(stk, 0, 0);
	if (result) {
		debugprintk(KERN_ERR
			"%s: calibration check1 fail, FT_index=%d\n",
			__func__, FT_index);
		goto ic_err_misc;
	}
#ifdef STK_DEBUG_CALI
	/*debugprintk(KERN_INFO "\n%s:Check All OTP Data before write OTP\n",
		__func__);*/
	/*STK831x_ReadAllOTP();*/

#endif
	/*Write OTP	*/
	debugprintk(KERN_INFO "\n%s:Write offset data to FT%d OTP\n",
		__func__, FT_index);
	result = STK831x_WriteOffsetOTP(stk, FT_index, otp_offset);
	if (result < 0) {
		debugprintk(KERN_INFO "%s: write OTP%d fail\n",
			__func__, FT_index);
		goto ic_err_misc;
	}

	buffer[0] = STK831X_MODE;
	result = STK_i2c_Rx(buffer, 1);
	if (result < 0)
		goto ic_err_i2c_rw;
	buffer[1] = (buffer[0] & 0xF8);
	buffer[0] = STK831X_MODE;
	result = STK_i2c_Tx(buffer, 2);
	if (result < 0)
		goto ic_err_i2c_rw;

	msleep(20);
	STK831x_Init(stk, this_client);
#ifdef STK_DEBUG_CALI
	/*debugprintk(KERN_INFO "\n%s:Check All OTP Data after write
		OTP and reset\n", __func__);*/
	/*STK831x_ReadAllOTP();*/
#endif

	result = STK831X_VerifyCali(stk, 1, delay_ms);
	if (result) {
		debugprintk(KERN_ERR "%s: calibration check2 fail\n", __func__);
		goto ic_err_misc;
	}
	return 0;

ic_err_misc:
	STK831x_Init(stk, this_client);
	msleep(20);
	atomic_set(&stk->cali_status, -result);
	return result;

ic_err_i2c_rw:
	debugprintk(KERN_ERR "%s: i2c read/write error, err=0x%x\n",
		__func__, result);
	msleep(20);
	STK831x_Init(stk, this_client);
	atomic_set(&stk->cali_status, STK_K_FAIL_I2C);
	return result;
}

static int32_t stk_get_file_content(char *r_buf, int8_t buf_size)
{
	struct file  *cali_file;
	mm_segment_t fs;
	ssize_t ret;

	cali_file = filp_open(STK_ACC_CALI_FILE, O_RDONLY, 0);
	if (IS_ERR(cali_file)) {
		debugprintk(KERN_ERR "%s: filp_open error, no offset file!\n",
			__func__);
		return -ENOENT;
	} else {
		fs = get_fs();
		set_fs(get_ds());
		ret = cali_file->f_op->read(cali_file, r_buf,
			STK_ACC_CALI_FILE_SIZE, &cali_file->f_pos);
		if (ret < 0) {
			filp_close(cali_file, NULL);
			return -EIO;
		}
		set_fs(fs);
	}

	filp_close(cali_file, NULL);
	return 0;
}

#ifdef STK_PERMISSION_THREAD
static struct task_struct *STKPermissionThread;

static int stk_permission_thread(void *data)
{
	int ret = 0;
	int retry = 0;
	mm_segment_t fs = get_fs();
	set_fs(KERNEL_DS);
	msleep(20000);
	do {
		msleep(5000);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input0/driver/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input1/driver/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input2/driver/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input3/driver/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input4/driver/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input0/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input1/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input2/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input3/cali" , 0666);
		ret = sys_fchmodat(AT_FDCWD,
			"/sys/class/input/input4/cali" , 0666);
		ret = sys_chmod(STK_ACC_CALI_FILE , 0666);
		ret = sys_fchmodat(AT_FDCWD, STK_ACC_CALI_FILE , 0666);
		if (retry++ > 10)
			break;
	} while (ret == -ENOENT);
	set_fs(fs);
	debugprintk(KERN_INFO "%s exit, retry=%d\n", __func__, retry);
	return 0;
}
#endif	/*	#ifdef STK_PERMISSION_THREAD	*/

static int stk_store_in_file(char offset[], char mode)
{
	struct file  *cali_file;
	char r_buf[STK_ACC_CALI_FILE_SIZE] = {0};
	char w_buf[STK_ACC_CALI_FILE_SIZE] = {0};
	mm_segment_t fs;
	ssize_t ret;
	int8_t i;

	w_buf[0] = STK_ACC_CALI_VER0;
	w_buf[1] = STK_ACC_CALI_VER1;
	w_buf[2] = offset[0];
	w_buf[3] = offset[1];
	w_buf[4] = offset[2];
	w_buf[5] = mode;

	cali_file = filp_open(STK_ACC_CALI_FILE, O_CREAT | O_RDWR, 0666);

	if (IS_ERR(cali_file)) {
		debugprintk(KERN_ERR "%s: filp_open error!\n", __func__);
		return -STK_K_FAIL_OPEN_FILE;
	} else {
		fs = get_fs();
		set_fs(get_ds());

		ret = cali_file->f_op->write(cali_file, w_buf,
			STK_ACC_CALI_FILE_SIZE, &cali_file->f_pos);
		if (ret != STK_ACC_CALI_FILE_SIZE) {
			debugprintk(KERN_ERR "%s: write error!\n", __func__);
			filp_close(cali_file, NULL);
			return -STK_K_FAIL_W_FILE;
		}
		cali_file->f_pos = 0x00;
		ret = cali_file->f_op->read(cali_file, r_buf,
			STK_ACC_CALI_FILE_SIZE, &cali_file->f_pos);
		if (ret < 0) {
			debugprintk(KERN_ERR "%s: read error!\n", __func__);
			filp_close(cali_file, NULL);
			return -STK_K_FAIL_R_BACK;
		}
		set_fs(fs);

		for (i = 0; i < STK_ACC_CALI_FILE_SIZE; i++) {
			if (r_buf[i] != w_buf[i]) {
				debugprintk(KERN_ERR
					"%s: read back error,", __func__);
				debugprintk(KERN_ERR
					"r_buf[%x](0x%x) != w_buf[%x](0x%x)\n",
					i, r_buf[i], i, w_buf[i]);
				filp_close(cali_file, NULL);
				return -STK_K_FAIL_R_BACK_COMP;
			}
		}
	}
	filp_close(cali_file, NULL);

#ifdef STK_PERMISSION_THREAD
	fs = get_fs();
	set_fs(KERNEL_DS);
	ret = sys_chmod(STK_ACC_CALI_FILE , 0666);
	ret = sys_fchmodat(AT_FDCWD, STK_ACC_CALI_FILE , 0666);
	set_fs(fs);
#endif
	return 0;
}

static int stk_open(struct inode *inode, struct file *file)
{
	int ret;
	ret = nonseekable_open(inode, file);
	if (ret < 0)
		return ret;
	file->private_data = stk831x_data_ptr;
	return 0;
}

static int stk_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long stk_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int retval = 0;
	char state = 0, restore_state = 0;
	char rwbuf[8] = "";
	uint32_t delay_ns;
	char char3_buffer[3];
	int result;
	int int3_buffer[3];
	struct stk831x_data *stk = file->private_data;

	/*	printk(KERN_INFO "%s: cmd = 0x%x\n", __func__, cmd);	*/

	if (cmd == STK_IOCTL_SET_DELAY || cmd == STK_IOCTL_SET_OFFSET ||
		cmd == STK_IOCTL_SET_RANGE || cmd == STK_IOCTL_WRITE ||
		cmd == STK_IOCTL_SET_CALI) {
		STK831x_GetEnable(stk, &restore_state);
		if (restore_state)
			STK831x_SetEnable(stk, 0);
	}

	switch (cmd) {
	case STK_IOCTL_SET_OFFSET:
		if (copy_from_user(&char3_buffer, argp, sizeof(char3_buffer)))
			return -EFAULT;
		break;
	case STK_IOCTL_SET_DELAY:
		if (copy_from_user(&delay_ns, argp, sizeof(uint32_t)))
			return -EFAULT;
		break;
	case STK_IOCTL_WRITE:
	case STK_IOCTL_READ:
		if (copy_from_user(&rwbuf, argp, sizeof(rwbuf)))
			return -EFAULT;
		break;
	case STK_IOCTL_SET_ENABLE:
	case STK_IOCTL_SET_RANGE:
	case STK_IOCTL_SET_CALI:
		if (copy_from_user(&state, argp, sizeof(char)))
			return -EFAULT;
		break;
	default:
		break;
	}

	switch (cmd) {
	case STK_IOCTL_WRITE:
		if (rwbuf[0] < 2)
			return -EINVAL;
		result = STK_i2c_Tx(&rwbuf[1], rwbuf[0]);
		if (result < 0)
			return result;
		break;
	case STK_IOCTL_SET_OFFSET:
		STK831x_SetOffset(char3_buffer);
		break;
	case STK_IOCTL_SET_DELAY:
		STK831x_SetDelay(stk, delay_ns);
		break;
	case STK_IOCTL_READ:
		if (rwbuf[0] < 1)
			return -EINVAL;
		result = STK_i2c_Rx(&rwbuf[1], rwbuf[0]);
		if (result < 0)
			return result;
		break;
	case STK_IOCTL_GET_DELAY:
		STK831x_GetDelay(stk, &delay_ns);
		break;
	case STK_IOCTL_GET_OFFSET:
		STK831x_GetOffset(char3_buffer);
		break;
	case STK_IOCTL_GET_ACCELERATION:
		STK831x_ReadSensorData(stk);
		int3_buffer[0] = stk->raw_data[0];
		int3_buffer[1] = stk->raw_data[1];
		int3_buffer[2] = stk->raw_data[2];
		break;
	case STK_IOCTL_SET_ENABLE:
		STK831x_SetEnable(stk, state);
		break;
	case STK_IOCTL_GET_ENABLE:
		STK831x_GetEnable(stk, &state);
		break;
	case STK_IOCTL_SET_RANGE:
		STK831x_SetRange(state);
		break;
	case STK_IOCTL_GET_RANGE:
		STK831x_GetRange(&state);
		break;
	case STK_IOCTL_SET_CALI:
		STK831x_SetCali(stk, state);
		break;
	default:
		retval = -ENOTTY;
		break;
	}

	if (cmd == STK_IOCTL_SET_DELAY || cmd == STK_IOCTL_SET_OFFSET ||
		cmd == STK_IOCTL_SET_RANGE || cmd == STK_IOCTL_WRITE ||
		cmd == STK_IOCTL_SET_CALI) {
		if (restore_state)
			STK831x_SetEnable(stk, restore_state);
	}
	switch (cmd) {
	case STK_IOCTL_GET_ACCELERATION:
		if (copy_to_user(argp, &int3_buffer, sizeof(int3_buffer)))
			return -EFAULT;
		break;
	case STK_IOCTL_READ:
		if (copy_to_user(argp, &rwbuf, sizeof(rwbuf)))
			return -EFAULT;
		break;
	case STK_IOCTL_GET_DELAY:
		if (copy_to_user(argp, &delay_ns, sizeof(delay_ns)))
			return -EFAULT;
		break;
	case STK_IOCTL_GET_OFFSET:
		if (copy_to_user(argp, &char3_buffer, sizeof(char3_buffer)))
			return -EFAULT;
		break;
	case STK_IOCTL_GET_RANGE:
	case STK_IOCTL_GET_ENABLE:
		if (copy_to_user(argp, &state, sizeof(char)))
			return -EFAULT;
		break;
	default:
		break;
	}

	return retval;
}


static const struct file_operations stk_fops = {
	.owner = THIS_MODULE,
	.open = stk_open,
	.release = stk_release,
	.unlocked_ioctl = stk_ioctl,
};

static struct miscdevice stk_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "stk831x",
	.fops = &stk_fops,
};


#if STK_ACC_POLLING_MODE
static enum hrtimer_restart stk_acc_timer_func(struct hrtimer *timer)
{
	struct stk831x_data *stk = container_of(timer,
		struct stk831x_data, acc_timer);
	queue_work(stk->stk_acc_wq, &stk->stk_acc_work);
	hrtimer_forward_now(&stk->acc_timer, stk->acc_poll_delay);
	return HRTIMER_RESTART;
}

static void stk_acc_poll_work_func(struct work_struct *work)
{
	struct stk831x_data *stk = container_of(work,
		struct stk831x_data, stk_acc_work);
	STK831x_ReadSensorData(stk);
	STK831x_ReportValue(stk);
	return;
}

#else

static irqreturn_t stk_mems_irq_handler(int irq, void *data)
{
	struct stk831x_data *pData = data;
	disable_irq_nosync(pData->irq);
	queue_work(stk_mems_work_queue, &pData->stk_work);
	return IRQ_HANDLED;
}


static void stk_mems_wq_function(struct work_struct *work)
{
	struct stk831x_data *stk = container_of(work,
		struct stk831x_data, stk_work);
	STK831x_ReadSensorData(stk);
	STK831x_ReportValue(stk);
	enable_irq(stk->irq);
}

static int stk831x_irq_setup(struct i2c_client *client,
	struct stk831x_data *stk_int)
{
	int error;
	int irq = -1;
#if ADDITIONAL_GPIO_CFG
	if (gpio_request(STK_INT_PIN, "EINT")) {
		debugprintk(KERN_ERR "%s:gpio_request() failed\n", __func__);
		return -1;
	}
	gpio_direction_input(STK_INT_PIN);

	irq = gpio_to_irq(STK_INT_PIN);
	if (irq < 0) {
		debugprintk(KERN_ERR "%s:gpio_to_irq() failed\n", __func__);
		return -1;
	}
	client->irq = irq;
	stk_int->irq = irq;
#endif
	debugprintk(KERN_INFO "%s: irq # = %d\n", __func__, irq);
	if (irq < 0)
		debugprintk(KERN_ERR "%s: irq number was not specified!\n",
			__func__);
	error = request_irq(client->irq, stk_mems_irq_handler,
		IRQF_TRIGGER_RISING , "stk-mems", stk_int);
	if (error < 0) {
		debugprintk(KERN_ERR "%s: request_irq(%d) failed for (%d)\n",
			__func__, client->irq, error);
		return -1;
	}
	disable_irq(irq);
	return irq;
}

#endif

static ssize_t stk831x_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	return scnprintf(buf, PAGE_SIZE,  "%d\n", atomic_read(&stk->enabled));
}

static ssize_t stk831x_enable_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;

	struct stk831x_data *stk = i2c_get_clientdata(this_client);

	error = kstrtoul(buf, 10, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: strict_strtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}
	if ((data == 0) || (data == 1))
		STK831x_SetEnable(stk, data);
	else
		debugprintk(KERN_ERR "%s: invalud argument, data=%ld\n",
			__func__, data);
	return count;
}

static ssize_t stk831x_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	int ddata[3];

	debugprintk(KERN_INFO "driver version:%s\n", STK_ACC_DRIVER_VERSION);
	STK831x_ReadSensorData(stk);
	ddata[0] = stk->raw_data[0];
	ddata[1] = stk->raw_data[1];
	ddata[2] = stk->raw_data[2];
	return scnprintf(buf, PAGE_SIZE,  "%d %d %d\n",
		ddata[0], ddata[1], ddata[2]);
}

static ssize_t stk831x_delay_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	uint32_t gdelay_ns;

	STK831x_GetDelay(stk, &gdelay_ns);
	return scnprintf(buf, PAGE_SIZE,  "%d\n", gdelay_ns/1000000);
}

static ssize_t stk831x_delay_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	char restore_state = 0;

	error = kstrtoul(buf, 10, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: strict_strtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}

	STK831x_GetEnable(stk, &restore_state);
	if (restore_state)
		STK831x_SetEnable(stk, 0);

	STK831x_SetDelay(stk, data*1000000);	/* ms to ns */

	if (restore_state)
		STK831x_SetEnable(stk, restore_state);
	return count;
}

static ssize_t stk831x_cali_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	int status = atomic_read(&stk->cali_status);

	if (status != STK_K_RUNNING)
		STK831x_GetCali(stk);
	return scnprintf(buf, PAGE_SIZE,  "%02x\n", status);
}

static ssize_t stk831x_cali_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	error = kstrtoul(buf, 10, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}
	STK831x_SetCali(stk, data);
	return count;
}

static ssize_t stk831x_send_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int error, i;
	char *token[2];
	int w_reg[2];
	char buffer[2] = "";

	for (i = 0; i < 2; i++)
		token[i] = strsep((char **)&buf, " ");

	error = kstrtoul(token[0], 16, (unsigned long *)&(w_reg[0]));

	if (error < 0) {
		debugprintk(KERN_ERR "%s:kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}
	error = kstrtoul(token[1], 16, (unsigned long *)&(w_reg[1]));
	if (error < 0) {
		debugprintk(KERN_ERR "%s:kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}
	debugprintk(KERN_INFO "%s: reg[0x%x]=0x%x\n", __func__,
		w_reg[0], w_reg[1]);
	buffer[0] = w_reg[0];
	buffer[1] = w_reg[1];
	error = STK_i2c_Tx(buffer, 2);
	if (error < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return error;
	}
	return count;
}

static ssize_t stk831x_recv_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	return scnprintf(buf, PAGE_SIZE,  "%02x\n", stk->recv_reg);
}

static ssize_t stk831x_recv_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	char buffer[2] = "";
	unsigned long data;
	int error;
	struct stk831x_data *stk = i2c_get_clientdata(this_client);

	error = kstrtoul(buf, 16, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}

	buffer[0] = data;
	error = STK_i2c_Rx(buffer, 2);
	if (error < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return error;
	}
	stk->recv_reg = buffer[0];
	debugprintk(KERN_INFO "%s: reg[0x%x]=0x%x\n",
		__func__, (int)data , (int)buffer[0]);
	return count;
}

static ssize_t stk831x_allreg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int error;
	char buffer[16] = "";
	char show_buffer[14] = "";
	int aa, bb, no, show_no = 0;
	ssize_t num = 0;
	for (bb = 0; bb < 4; bb++) {
		buffer[0] = bb * 0x10;
		error = STK_i2c_Rx(buffer, 16);
		if (error < 0) {
			debugprintk(KERN_ERR "%s:failed\n", __func__);
			return error;
		}
		for (aa = 0; aa < 16; aa++) {
			no = bb*0x10+aa;
			debugprintk(KERN_INFO "stk reg[0x%x]=0x%x\n",
				no, buffer[aa]);
			switch (no) {
			case 0x0:
			case 0x1:
			case 0x2:
			case 0x3:
			case 0x4:
			case 0x5:
			case STK831X_INTSU:
			case STK831X_MODE:
			case STK831X_SR:
			case STK831X_OFSX:
			case STK831X_OFSY:
			case STK831X_OFSZ:
			case STK831X_STH:
			case 0x24:
				show_buffer[show_no] = buffer[aa];
				show_no++;
				break;
			default:
				break;
			}
		}
	}
	num = scnprintf(buf, PAGE_SIZE,
		"0x0=%02x,0x1=%02x,0x2=%02x,0x3=%02x,0x4=%02x,0x5=%02x,",
		show_buffer[0], show_buffer[1], show_buffer[2],
		show_buffer[3], show_buffer[4], show_buffer[5]);
	num += scnprintf(buf+num, PAGE_SIZE,
		"INTSU=%02x,MODE=%02x,SR=%02x,",
		show_buffer[6], show_buffer[7], show_buffer[8]);
	num += scnprintf(buf+num, PAGE_SIZE,
		"OFSX=%02x,OFSY=%02x,OFSZ=%02x,STH=%02x,0x24=%02x\n",
		show_buffer[9], show_buffer[10], show_buffer[11],
		show_buffer[12], show_buffer[13]);
	return num;
}

static ssize_t stk831x_sendo_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	int error, i;
	char *token[2];
	int w_reg[2];
	char buffer[2] = "";

	for (i = 0; i < 2; i++)
		token[i] = strsep((char **)&buf, " ");

	error = kstrtoul(token[0], 16, (unsigned long *)&(w_reg[0]));
	if (error < 0) {
		debugprintk(KERN_ERR "%s:kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}

	error = kstrtoul(token[1], 16, (unsigned long *)&(w_reg[1]));
	if (error < 0) {
		debugprintk(KERN_ERR "%s:kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}
	debugprintk(KERN_INFO "%s: reg[0x%x]=0x%x\n", __func__,
		w_reg[0], w_reg[1]);

	buffer[0] = w_reg[0];
	buffer[1] = w_reg[1];
	error = STK831x_WriteByteOTP(buffer[0], buffer[1]);
	if (error < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return error;
	}
	return count;
}


static ssize_t stk831x_recvo_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	char buffer[2] = "";
	unsigned long data;
	int error;

	error = kstrtoul(buf, 16, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: kstrtoul failed, error=0x%x\n",
			__func__, error);
		return error;
	}

	buffer[0] = data;
	error = STK831x_ReadByteOTP(buffer[0], &buffer[1]);
	if (error < 0) {
		debugprintk(KERN_ERR "%s:failed\n", __func__);
		return error;
	}
	debugprintk(KERN_INFO "%s: reg[0x%x]=0x%x\n",
		__func__, buffer[0] , buffer[1]);
	return count;
}

static ssize_t stk831x_firlen_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
#ifdef STK_LOWPASS
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	int len = atomic_read(&stk->firlength);

	if (atomic_read(&stk->firlength)) {
		debugprintk(KERN_INFO "len = %2d, idx = %2d\n", stk->fir.num,
			stk->fir.idx);
		debugprintk(KERN_INFO "sum = [%5d %5d %5d]\n", stk->fir.sum[0],
			stk->fir.sum[1], stk->fir.sum[2]);
		debugprintk(KERN_INFO "avg = [%5d %5d %5d]\n",
			stk->fir.sum[0]/len, stk->fir.sum[1]/len,
			stk->fir.sum[2]/len);
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&stk->firlength));
#else
	return snprintf(buf, PAGE_SIZE, "not support\n");
#endif
}

static ssize_t stk831x_firlen_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
#ifdef STK_LOWPASS
	struct stk831x_data *stk = i2c_get_clientdata(this_client);
	int error;
	unsigned long data;

	error = kstrtoul(buf, 10, &data);
	if (error) {
		debugprintk(KERN_ERR "%s: kstrtoul failed, error=%d\n",
			__func__, error);
		return error;
	}

	if (data > MAX_FIR_LEN)
		debugprintk(KERN_ERR
			"%s: firlen exceed maximum filter length\n", __func__);
	else if (data < 1) {
		atomic_set(&stk->firlength, 1);
		atomic_set(&stk->fir_en, 0);
		memset(&stk->fir, 0x00, sizeof(stk->fir));
	} else {
		atomic_set(&stk->firlength, data);
		memset(&stk->fir, 0x00, sizeof(stk->fir));
		atomic_set(&stk->fir_en, 1);
	}
#else
	debugprintk(KERN_ERR "%s: firlen is not supported\n", __func__);
#endif
	return count;
}

static ssize_t stk831x_range_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	char grange = 0;
	int range = 0;

	range = stk8312_range[1].range;
	if (!STK831x_GetRange(&grange)) {
		if (grange <= 0x2)
			range = stk8312_range[(int)grange].range;
	}
	return sprintf(buf, "%d\n", range);
}

static ssize_t stk831x_resolution_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	char grange = 0;
	int resolution = 0;


	resolution = stk8312_range[1].resolution;
	if (!STK831x_GetRange(&grange)) {
		if (grange <= 0x2)
			resolution = stk8312_range[(int)grange].resolution;
	}

	return sprintf(buf, "%d\n", resolution);
}

static DEVICE_ATTR(enable, 0666, stk831x_enable_show, stk831x_enable_store);
static DEVICE_ATTR(value, 0444, stk831x_value_show, NULL);
static DEVICE_ATTR(delay, 0666, stk831x_delay_show, stk831x_delay_store);
static DEVICE_ATTR(cali, 0666, stk831x_cali_show, stk831x_cali_store);
static DEVICE_ATTR(send, 0222, NULL, stk831x_send_store);
static DEVICE_ATTR(recv, 0666, stk831x_recv_show, stk831x_recv_store);
static DEVICE_ATTR(allreg, 0444, stk831x_allreg_show, NULL);
static DEVICE_ATTR(sendo, 0222, NULL, stk831x_sendo_store);
static DEVICE_ATTR(recvo, 0222, NULL, stk831x_recvo_store);
static DEVICE_ATTR(firlen, 0666, stk831x_firlen_show, stk831x_firlen_store);
static DEVICE_ATTR(range, 0444, stk831x_range_show, NULL);
static DEVICE_ATTR(resolution, 0444, stk831x_resolution_show, NULL);

static struct attribute *stk831x_attributes[] = {
	&dev_attr_enable.attr,
	&dev_attr_value.attr,
	&dev_attr_delay.attr,
	&dev_attr_cali.attr,
	&dev_attr_send.attr,
	&dev_attr_recv.attr,
	&dev_attr_allreg.attr,
	&dev_attr_sendo.attr,
	&dev_attr_recvo.attr,
	&dev_attr_firlen.attr,
	&dev_attr_range.attr,
	&dev_attr_resolution.attr,
	NULL
};

static struct attribute_group stk831x_attribute_group = {
	/*#ifndef STK_ALLWINNER_PLATFORM	*/
		/*.name = "driver",*/
	/*#endif	*/
	.attrs = stk831x_attributes,
};
static int stk831x_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int error;
	struct stk831x_data *stk;

	debugprintk(KERN_INFO "stk831x_probe: driver version:%s\n",
		STK_ACC_DRIVER_VERSION);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		debugprintk(KERN_ERR "%s:i2c_check_functionality error\n",
			__func__);
		error = -ENODEV;
		goto exit_i2c_check_functionality_error;
	}

	stk = kzalloc(sizeof(struct stk831x_data), GFP_KERNEL);
	if (!stk) {
		debugprintk(KERN_ERR "%s:memory allocation error\n",
			__func__);
		error = -ENOMEM;
		goto exit_kzalloc_error;
	}
	stk831x_data_ptr = stk;
	mutex_init(&stk->write_lock);

#if (STK_ACC_POLLING_MODE)
	stk->stk_acc_wq = create_singlethread_workqueue("stk_acc_wq");
	INIT_WORK(&stk->stk_acc_work, stk_acc_poll_work_func);
	hrtimer_init(&stk->acc_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	stk->acc_poll_delay = ns_to_ktime(STK831X_SAMPLE_TIME[STK831X_INIT_ODR]
		*USEC_PER_MSEC);
	stk->acc_timer.function = stk_acc_timer_func;
#else
	stk_mems_work_queue = create_workqueue("stk_mems_wq");
	if (stk_mems_work_queue)
		INIT_WORK(&stk->stk_work, stk_mems_wq_function);
	else {
		debugprintk(KERN_ERR "%s:create_workqueue error\n", __func__);
		error = -EPERM;
		goto exit_create_workqueue_error;
	}

	error = stk831x_irq_setup(client, stk);
	if (!error)
		goto exit_irq_setup_error;
#endif

	i2c_set_clientdata(client, stk);
	this_client = client;

	error = STK831x_Init(stk, client);
	if (error) {
		debugprintk(KERN_ERR "%s:stk831x initialization failed\n",
			__func__);
		goto exit_stk_init_error;
	}
	atomic_set(&stk->cali_status, STK_K_NO_CALI);
	stk->first_enable = true;
	stk->re_enable = false;
	event_since_en_limit = 20;

	stk->input_dev = input_allocate_device();
	if (!stk->input_dev) {
		error = -ENOMEM;
		debugprintk(KERN_ERR "%s:input_allocate_device failed\n",
			__func__);
		goto exit_input_dev_alloc_error;
	}

	stk->input_dev->name = STK831X_I2C_NAME;
	set_bit(EV_ABS, stk->input_dev->evbit);
	input_set_abs_params(stk->input_dev, ABS_X, -128, 127, 0, 0);
	input_set_abs_params(stk->input_dev, ABS_Y, -128, 127, 0, 0);
	input_set_abs_params(stk->input_dev, ABS_Z, -128, 127, 0, 0);


	error = input_register_device(stk->input_dev);
	if (error) {
		debugprintk(KERN_ERR
			"%s:Unable to register input device: %s\n",
			__func__, stk->input_dev->name);
		goto exit_input_register_device_error;
	}

	error = misc_register(&stk_device);
	if (error) {
		debugprintk(KERN_ERR "%s: misc_register failed\n", __func__);
		goto exit_misc_device_register_error;
	}
	error = sysfs_create_group(&stk->input_dev->dev.kobj,
		&stk831x_attribute_group);
	if (error) {
		debugprintk(KERN_ERR "%s: sysfs_create_group failed\n",
			__func__);
		goto exit_sysfs_create_group_error;
	}

	debugprintk(KERN_INFO "%s successfully\n", __func__);
	return 0;
exit_sysfs_create_group_error:
	sysfs_remove_group(&stk->input_dev->dev.kobj, &stk831x_attribute_group);
exit_misc_device_register_error:
	misc_deregister(&stk_device);
exit_input_register_device_error:
	input_unregister_device(stk->input_dev);
exit_input_dev_alloc_error:
exit_stk_init_error:
#if (STK_ACC_POLLING_MODE)
	hrtimer_try_to_cancel(&stk->acc_timer);
	destroy_workqueue(stk->stk_acc_wq);
#else
	free_irq(client->irq, stk);
#if ADDITIONAL_GPIO_CFG
exit_irq_setup_error:
	gpio_free(STK_INT_PIN);
#endif
	destroy_workqueue(stk_mems_work_queue);
exit_create_workqueue_error:
#endif
	mutex_destroy(&stk->write_lock);
	kfree(stk);
	stk = NULL;
exit_kzalloc_error:
exit_i2c_check_functionality_error:
	return error;
}

static int stk831x_remove(struct i2c_client *client)
{
	struct stk831x_data *stk = i2c_get_clientdata(client);

	sysfs_remove_group(&stk->input_dev->dev.kobj, &stk831x_attribute_group);
	misc_deregister(&stk_device);
	input_unregister_device(stk->input_dev);
	cancel_work_sync(&stk->stk_work);
#if (STK_ACC_POLLING_MODE)
	hrtimer_try_to_cancel(&stk->acc_timer);
	destroy_workqueue(stk->stk_acc_wq);
#else
	free_irq(client->irq, stk);
#if ADDITIONAL_GPIO_CFG
	gpio_free(STK_INT_PIN);
#endif
	if (stk_mems_work_queue)
		destroy_workqueue(stk_mems_work_queue);
#endif
	mutex_destroy(&stk->write_lock);
	kfree(stk);
	stk = NULL;
	return 0;
}

static const struct i2c_device_id stk831x[] = {
	{ STK831X_I2C_NAME, 0 },
	{ }
};

#ifdef CONFIG_PM_SLEEP
static int stk831x_suspend(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct stk831x_data *stk = i2c_get_clientdata(client);
	debugprintk(KERN_INFO "%s\n", __func__);
	if (atomic_read(&stk->enabled)) {
		STK831x_SetEnable(stk, 0);
		stk->re_enable = true;
	}
	return 0;
}


static int stk831x_resume(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct stk831x_data *stk = i2c_get_clientdata(client);
#ifdef STK_RESUME_RE_INIT
	int error;
#endif

	debugprintk(KERN_INFO "%s\n", __func__);
#ifdef STK_RESUME_RE_INIT
	error = STK831x_Init(stk, this_client);
	if (error) {
		debugprintk(KERN_ERR "%s:stk831x initialization failed\n",
			__func__);
		return error;
	}
	stk->first_enable = true;
#endif
	if (stk->re_enable) {
		stk->re_enable = false;
		STK831x_SetEnable(stk, 1);
	}
	return 0;
}
#endif


#ifdef CONFIG_PM_RUNTIME
static int stk831x_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct stk831x_data *stk = i2c_get_clientdata(client);
	debugprintk(KERN_INFO "%s\n", __func__);
	if (atomic_read(&stk->enabled)) {
		STK831x_SetEnable(stk, 0);
		stk->re_enable = true;
	}
	return 0;
}


static int stk831x_runtime_resume(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct stk831x_data *stk = i2c_get_clientdata(client);
	debugprintk(KERN_INFO "%s\n", __func__);
	stk->first_enable = true;
	if (stk->re_enable) {
		stk->re_enable = false;
		STK831x_SetEnable(stk, 1);
	}
	return 0;
}
#endif

static const struct dev_pm_ops stk831x_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(stk831x_suspend, stk831x_resume)
	SET_RUNTIME_PM_OPS(stk831x_runtime_suspend,
		stk831x_runtime_resume, NULL)
};

#ifdef STK_ALLWINNER_PLATFORM
static int gsensor_detect(struct i2c_client *client,
	struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	debugprintk("======stk831x init detect======\n");
	if (twi_id == adapter->nr) {
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			__func__, STK831X_I2C_NAME, i2c_adapter_id(adapter),
			client->addr);

		strlcpy(info->type, STK831X_I2C_NAME,
			sizeof(STK831X_I2C_NAME)+1);
		return 0;
	} else
		return -ENODEV;
}
#endif


static struct i2c_driver stk831x_driver = {
#ifdef STK_ALLWINNER_PLATFORM
	.class = I2C_CLASS_HWMON,
#endif
	.probe = stk831x_probe,
	.remove = stk831x_remove,
	.id_table	= stk831x,
	/*.suspend = stk831x_suspend,*/
	/*.resume = stk831x_resume,*/
	.driver = {
		.name = STK831X_I2C_NAME,
		.pm = &stk831x_pm_ops,
	},
#ifdef STK_ALLWINNER_PLATFORM
	.address_list	= u_i2c_addr.normal_i2c,
#endif
};

static int __init stk831x_init(void)
{
	int ret;

	debugprintk("Sensor: %s\n", __func__);

	ret = i2c_add_driver(&stk831x_driver);
	if (ret != 0) {
		debugprintk("stk831x init fail, ret=0x%x\n", ret);
		i2c_del_driver(&stk831x_driver);
		return ret;
	}
#ifdef STK_PERMISSION_THREAD
	STKPermissionThread = kthread_run(stk_permission_thread,
		"stk", "Permissionthread");
	if (IS_ERR(STKPermissionThread))
		STKPermissionThread = NULL;
#endif
	return ret;
}

static void __exit stk831x_exit(void)
{
	i2c_del_driver(&stk831x_driver);
#ifdef STK_PERMISSION_THREAD
	if (STKPermissionThread)
		STKPermissionThread = NULL;
#endif
}

module_init(stk831x_init);
module_exit(stk831x_exit);

MODULE_AUTHOR("Lex Hsieh / Sensortek");
MODULE_DESCRIPTION("stk831x 3-Axis accelerometer driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(STK_ACC_DRIVER_VERSION);
