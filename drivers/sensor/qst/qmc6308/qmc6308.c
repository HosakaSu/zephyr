/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qst_qmc6308

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#define QMC6308_ID           0x00U
#define QMC6308_ID_VALUE     0x80U
#define QMC6308_DATA         0x01U
#define QMC6308_STATUS       0x09U
#define QMC6308_CONTROL1     0x0aU
#define QMC6308_CONTROL2     0x0bU
#define QMC6308_CONTROL3     0x0dU
#define QMC6308_DRDY         BIT(0)
#define QMC6308_OVERFLOW     BIT(1)
#define QMC6308_NVM_READY    (BIT(3) | BIT(4))
#define QMC6308_RESET        BIT(7)
#define QMC6308_SR_INCREASED BIT(6)
#define QMC6308_NORMAL_OSR8  0xc1U

struct qmc6308_config {
	struct i2c_dt_spec bus;
	uint8_t range_index;
	uint8_t odr_index;
};

struct qmc6308_data {
	int16_t xyz[3];
	bool fetched;
};

static bool qmc6308_channel_supported(enum sensor_channel chan)
{
	return (chan == SENSOR_CHAN_MAGN_X) || (chan == SENSOR_CHAN_MAGN_Y) ||
	       (chan == SENSOR_CHAN_MAGN_Z) || (chan == SENSOR_CHAN_MAGN_XYZ);
}

static int qmc6308_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct qmc6308_config *config = dev->config;
	struct qmc6308_data *data = dev->data;
	uint8_t raw[6];
	uint8_t status;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && !qmc6308_channel_supported(chan)) {
		return -ENOTSUP;
	}
	/* Reading status clears DRDY and overflow; inspect both in the same read. */
	ret = i2c_reg_read_byte_dt(&config->bus, QMC6308_STATUS, &status);
	if (ret != 0) {
		return ret;
	}
	if ((status & QMC6308_OVERFLOW) != 0U) {
		return -ERANGE;
	}
	if ((status & QMC6308_DRDY) == 0U) {
		return -EAGAIN;
	}
	ret = i2c_burst_read_dt(&config->bus, QMC6308_DATA, raw, sizeof(raw));
	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(data->xyz); i++) {
		data->xyz[i] = (int16_t)sys_get_le16(&raw[i * 2U]);
	}
	data->fetched = true;
	return 0;
}

static int qmc6308_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	static const uint16_t sensitivity[] = {1000U, 2500U, 3750U, 15000U};
	const struct qmc6308_config *config = dev->config;
	const struct qmc6308_data *data = dev->data;
	uint8_t start;
	uint8_t count;

	if (!qmc6308_channel_supported(chan)) {
		return -ENOTSUP;
	}
	if (!data->fetched) {
		return -ENODATA;
	}
	start = (chan == SENSOR_CHAN_MAGN_XYZ) ? 0U : chan - SENSOR_CHAN_MAGN_X;
	count = (chan == SENSOR_CHAN_MAGN_XYZ) ? 3U : 1U;
	for (uint8_t i = 0U; i < count; i++) {
		int64_t microgauss =
			(int64_t)data->xyz[start + i] * 1000000 / sensitivity[config->range_index];

		val[i].val1 = microgauss / 1000000;
		val[i].val2 = microgauss % 1000000;
	}
	return 0;
}

static int qmc6308_init(const struct device *dev)
{
	const struct qmc6308_config *config = dev->config;
	uint8_t value;
	int ret;

	if (!i2c_is_ready_dt(&config->bus)) {
		return -ENODEV;
	}
	ret = i2c_reg_read_byte_dt(&config->bus, QMC6308_ID, &value);
	if (ret != 0) {
		return ret;
	}
	if (value != QMC6308_ID_VALUE) {
		return -ENODEV;
	}
	ret = i2c_reg_write_byte_dt(&config->bus, QMC6308_CONTROL2, QMC6308_RESET);
	if (ret != 0) {
		return ret;
	}
	k_msleep(1);
	/* SOFT_RST does not clear itself. Release it before configuring the chip. */
	ret = i2c_reg_write_byte_dt(&config->bus, QMC6308_CONTROL2, 0U);
	if (ret != 0) {
		return ret;
	}
	for (uint8_t attempt = 0U; attempt < 20U; attempt++) {
		ret = i2c_reg_read_byte_dt(&config->bus, QMC6308_STATUS, &value);
		if (ret != 0) {
			return ret;
		}
		if ((value & QMC6308_NVM_READY) == QMC6308_NVM_READY) {
			break;
		}
		k_msleep(1);
	}
	if ((value & QMC6308_NVM_READY) != QMC6308_NVM_READY) {
		return -ETIMEDOUT;
	}
	ret = i2c_reg_write_byte_dt(&config->bus, QMC6308_CONTROL3, QMC6308_SR_INCREASED);
	if (ret != 0) {
		return ret;
	}
	ret = i2c_reg_write_byte_dt(&config->bus, QMC6308_CONTROL2, config->range_index << 2);
	if (ret != 0) {
		return ret;
	}
	return i2c_reg_write_byte_dt(&config->bus, QMC6308_CONTROL1,
				     QMC6308_NORMAL_OSR8 | (config->odr_index << 2));
}

static DEVICE_API(sensor, qmc6308_api) = {
	.sample_fetch = qmc6308_sample_fetch,
	.channel_get = qmc6308_channel_get,
};

#define QMC6308_DEFINE(inst)                                                                       \
	static struct qmc6308_data qmc6308_data_##inst;                                            \
	static const struct qmc6308_config qmc6308_config_##inst = {                               \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.range_index = DT_INST_ENUM_IDX(inst, full_scale),                                 \
		.odr_index = DT_INST_ENUM_IDX(inst, sampling_frequency),                           \
	};                                                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, qmc6308_init, NULL, &qmc6308_data_##inst,               \
				     &qmc6308_config_##inst, POST_KERNEL,                          \
				     CONFIG_SENSOR_INIT_PRIORITY, &qmc6308_api);

DT_INST_FOREACH_STATUS_OKAY(QMC6308_DEFINE)
