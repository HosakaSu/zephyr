/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT liteon_ap3216c

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor/ap3216c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ap3216c, CONFIG_SENSOR_LOG_LEVEL);

#define AP3216C_SYSTEM_CONFIG 0x00U
#define AP3216C_IR_DATA_LOW   0x0aU
#define AP3216C_ALS_CONFIG    0x10U
#define AP3216C_MODE_RESET    0x04U
#define AP3216C_MODE_ALS_PS   0x03U
#define AP3216C_IR_OVERFLOW   BIT(7)
#define AP3216C_PS_OVERFLOW   BIT(6)
#define AP3216C_OBJECT_NEAR   BIT(7)

struct ap3216c_config {
	struct i2c_dt_spec bus;
	uint8_t gain_index;
};

struct ap3216c_data {
	uint8_t sample[6];
	bool fetched;
};

static bool ap3216c_channel_supported(enum sensor_channel chan)
{
	switch ((int)chan) {
	case SENSOR_CHAN_ALL:
	case SENSOR_CHAN_LIGHT:
	case SENSOR_CHAN_PROX:
	case SENSOR_CHAN_AP3216C_ALS_RAW:
	case SENSOR_CHAN_AP3216C_IR_RAW:
	case SENSOR_CHAN_AP3216C_PROX_RAW:
		return true;
	default:
		return false;
	}
}

static int ap3216c_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct ap3216c_config *config = dev->config;
	struct ap3216c_data *data = dev->data;
	uint8_t sample[sizeof(data->sample)];
	int ret;

	if (!ap3216c_channel_supported(chan)) {
		return -ENOTSUP;
	}

	/* Reading each low byte latches its high byte. Do not assume that the
	 * device advances the register address during multi-byte reads.
	 */
	for (size_t i = 0U; i < sizeof(sample); i++) {
		ret = i2c_reg_read_byte_dt(&config->bus, AP3216C_IR_DATA_LOW + i, &sample[i]);
		if (ret != 0) {
			return ret;
		}
	}

	memcpy(data->sample, sample, sizeof(sample));
	data->fetched = true;
	return 0;
}

static int ap3216c_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	/* Nominal microlux per count from the AP3216C Rev. 0.86 ALS table. */
	static const uint32_t microlux_per_count[] = {350000U, 78800U, 19700U, 4900U};
	const struct ap3216c_config *config = dev->config;
	const struct ap3216c_data *data = dev->data;
	const uint8_t *sample = data->sample;
	uint64_t microlux;
	bool ir_overflow;
	bool ps_overflow;

	if (!ap3216c_channel_supported(chan) || (chan == SENSOR_CHAN_ALL)) {
		return -ENOTSUP;
	}
	if (!data->fetched) {
		return -ENODATA;
	}

	ir_overflow = (sample[0] & AP3216C_IR_OVERFLOW) != 0U;
	ps_overflow = ir_overflow || (((sample[4] | sample[5]) & AP3216C_PS_OVERFLOW) != 0U);
	val->val2 = 0;

	switch ((int)chan) {
	case SENSOR_CHAN_LIGHT:
		microlux =
			(uint64_t)sys_get_le16(&sample[2]) * microlux_per_count[config->gain_index];
		val->val1 = microlux / 1000000U;
		val->val2 = microlux % 1000000U;
		return 0;
	case SENSOR_CHAN_AP3216C_ALS_RAW:
		val->val1 = sys_get_le16(&sample[2]);
		return 0;
	case SENSOR_CHAN_AP3216C_IR_RAW:
		if (ir_overflow) {
			return -ERANGE;
		}
		val->val1 = ((uint16_t)sample[1] << 2) | (sample[0] & 0x03U);
		return 0;
	case SENSOR_CHAN_AP3216C_PROX_RAW:
	case SENSOR_CHAN_PROX:
		if (ps_overflow) {
			return -ERANGE;
		}
		if (chan == SENSOR_CHAN_PROX) {
			val->val1 = (sample[5] & AP3216C_OBJECT_NEAR) != 0U;
		} else {
			val->val1 = ((uint16_t)(sample[5] & 0x3fU) << 4) | (sample[4] & 0x0fU);
		}
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int ap3216c_init(const struct device *dev)
{
	const struct ap3216c_config *config = dev->config;
	uint8_t mode;
	int ret;

	if (!i2c_is_ready_dt(&config->bus)) {
		return -ENODEV;
	}

	ret = i2c_reg_write_byte_dt(&config->bus, AP3216C_SYSTEM_CONFIG, AP3216C_MODE_RESET);
	if (ret != 0) {
		return ret;
	}
	k_msleep(10);

	ret = i2c_reg_write_byte_dt(&config->bus, AP3216C_ALS_CONFIG, config->gain_index << 4);
	if (ret != 0) {
		return ret;
	}
	ret = i2c_reg_write_byte_dt(&config->bus, AP3216C_SYSTEM_CONFIG, AP3216C_MODE_ALS_PS);
	if (ret != 0) {
		return ret;
	}
	ret = i2c_reg_read_byte_dt(&config->bus, AP3216C_SYSTEM_CONFIG, &mode);
	if (ret != 0) {
		return ret;
	}
	if (mode != AP3216C_MODE_ALS_PS) {
		return -EIO;
	}

	/* Allow the initial ALS and PS/IR conversions to finish. */
	k_msleep(300);
	return 0;
}

static DEVICE_API(sensor, ap3216c_api) = {
	.sample_fetch = ap3216c_sample_fetch,
	.channel_get = ap3216c_channel_get,
};

#define AP3216C_DEFINE(inst)                                                                       \
	static struct ap3216c_data ap3216c_data_##inst;                                            \
	static const struct ap3216c_config ap3216c_config_##inst = {                               \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.gain_index = DT_INST_ENUM_IDX(inst, als_gain),                                    \
	};                                                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, ap3216c_init, NULL, &ap3216c_data_##inst,               \
				     &ap3216c_config_##inst, POST_KERNEL,                          \
				     CONFIG_SENSOR_INIT_PRIORITY, &ap3216c_api);

DT_INST_FOREACH_STATUS_OKAY(AP3216C_DEFINE)
