/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT liteon_ap3216c

#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/sensor/ap3216c.h>
#include <zephyr/ztest.h>

struct ap3216c_emul_data {
	uint8_t regs[0x30];
	int fail_reg;
};

static int emul_init(const struct emul *emul, const struct device *parent)
{
	struct ap3216c_emul_data *data = emul->data;

	ARG_UNUSED(parent);
	data->fail_reg = -1;
	return 0;
}

static int emul_transfer(const struct emul *emul, struct i2c_msg *msgs, int num_msgs, int addr)
{
	struct ap3216c_emul_data *data = emul->data;
	uint8_t reg;

	ARG_UNUSED(addr);
	if ((num_msgs < 1) || (num_msgs > 2) || (msgs[0].len < 1U) ||
	    ((msgs[0].flags & I2C_MSG_READ) != 0U)) {
		return -EIO;
	}
	reg = msgs[0].buf[0];
	if ((reg >= sizeof(data->regs)) || (reg == data->fail_reg)) {
		return -EIO;
	}
	if ((num_msgs == 1) && (msgs[0].len == 2U)) {
		if ((reg == 0U) && (msgs[0].buf[1] == 4U)) {
			memset(data->regs, 0, sizeof(data->regs));
		} else {
			data->regs[reg] = msgs[0].buf[1];
		}
		return 0;
	}
	if ((num_msgs == 2) && (msgs[0].len == 1U) && (msgs[1].len == 1U) &&
	    ((msgs[1].flags & I2C_MSG_READ) != 0U)) {
		msgs[1].buf[0] = data->regs[reg];
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api emul_api = {
	.transfer = emul_transfer,
};

#define EMUL_DEFINE(inst)                                                                          \
	static struct ap3216c_emul_data emul_data_##inst;                                          \
	EMUL_DT_INST_DEFINE(inst, emul_init, &emul_data_##inst, NULL, &emul_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(EMUL_DEFINE)

static const struct device *const sensors[] = {
	DEVICE_DT_GET(DT_NODELABEL(sensor0)),
	DEVICE_DT_GET(DT_NODELABEL(sensor1)),
	DEVICE_DT_GET(DT_NODELABEL(sensor2)),
	DEVICE_DT_GET(DT_NODELABEL(sensor3)),
};

static const struct emul *const emulators[] = {
	EMUL_DT_GET(DT_NODELABEL(sensor0)),
	EMUL_DT_GET(DT_NODELABEL(sensor1)),
	EMUL_DT_GET(DT_NODELABEL(sensor2)),
	EMUL_DT_GET(DT_NODELABEL(sensor3)),
};

static int get(const struct device *dev, int chan, struct sensor_value *value)
{
	return sensor_channel_get(dev, (enum sensor_channel)chan, value);
}

static void load(size_t index, const uint8_t sample[6])
{
	struct ap3216c_emul_data *data = emulators[index]->data;

	data->fail_reg = -1;
	memcpy(&data->regs[0x0a], sample, 6);
	zassert_ok(sensor_sample_fetch(sensors[index]));
}

ZTEST(ap3216c, test_initialization)
{
	for (size_t i = 0U; i < ARRAY_SIZE(sensors); i++) {
		struct ap3216c_emul_data *data = emulators[i]->data;

		zassert_true(device_is_ready(sensors[i]));
		zassert_equal(data->regs[0], 3U);
		zassert_equal(data->regs[0x10], i << 4);
	}
}

ZTEST(ap3216c, test_illuminance_ranges)
{
	const uint8_t sample[] = {0, 0, 0xff, 0xff, 0, 0};
	const int32_t whole[] = {22937, 5164, 1291, 321};
	const int32_t fraction[] = {250000, 158000, 39500, 121500};
	struct sensor_value value;

	for (size_t i = 0U; i < ARRAY_SIZE(sensors); i++) {
		load(i, sample);
		zassert_ok(get(sensors[i], SENSOR_CHAN_LIGHT, &value));
		zassert_equal(value.val1, whole[i]);
		zassert_equal(value.val2, fraction[i]);
	}
}

ZTEST(ap3216c, test_raw_fields_and_object_flag)
{
	const uint8_t sample[] = {0x7f, 0xff, 0x34, 0x12, 0xbf, 0xbf};
	struct sensor_value value;

	load(1, sample);
	zassert_ok(get(sensors[1], SENSOR_CHAN_AP3216C_ALS_RAW, &value));
	zassert_equal(value.val1, 0x1234);
	zassert_ok(get(sensors[1], SENSOR_CHAN_AP3216C_IR_RAW, &value));
	zassert_equal(value.val1, 1023);
	zassert_ok(get(sensors[1], SENSOR_CHAN_AP3216C_PROX_RAW, &value));
	zassert_equal(value.val1, 1023);
	zassert_ok(get(sensors[1], SENSOR_CHAN_PROX, &value));
	zassert_equal(value.val1, 1);
	zassert_equal(value.val2, 0);
}

ZTEST(ap3216c, test_overflow_flags)
{
	uint8_t sample[] = {0x80, 0, 0x10, 0, 0, 0};
	struct sensor_value value;

	load(1, sample);
	zassert_equal(get(sensors[1], SENSOR_CHAN_AP3216C_IR_RAW, &value), -ERANGE);
	zassert_equal(get(sensors[1], SENSOR_CHAN_AP3216C_PROX_RAW, &value), -ERANGE);
	zassert_equal(get(sensors[1], SENSOR_CHAN_PROX, &value), -ERANGE);
	zassert_ok(get(sensors[1], SENSOR_CHAN_LIGHT, &value));
	sample[0] = 0;
	for (size_t i = 4U; i < 6U; i++) {
		sample[i] = 0x40;
		load(1, sample);
		zassert_equal(get(sensors[1], SENSOR_CHAN_AP3216C_PROX_RAW, &value), -ERANGE);
		zassert_ok(get(sensors[1], SENSOR_CHAN_AP3216C_IR_RAW, &value));
		sample[i] = 0;
	}
}

ZTEST(ap3216c, test_read_error_preserves_sample)
{
	const uint8_t sample[] = {0, 0, 0x34, 0x12, 0, 0};
	struct ap3216c_emul_data *data = emulators[1]->data;
	struct sensor_value value;

	load(1, sample);
	data->regs[0x0c] = 0xff;
	data->fail_reg = 0x0d;
	zassert_equal(sensor_sample_fetch(sensors[1]), -EIO);
	zassert_ok(get(sensors[1], SENSOR_CHAN_AP3216C_ALS_RAW, &value));
	zassert_equal(value.val1, 0x1234);
	data->fail_reg = -1;
}

ZTEST(ap3216c, test_unsupported_channels)
{
	struct sensor_value value;

	zassert_equal(get(sensors[1], SENSOR_CHAN_IR, &value), -ENOTSUP);
	zassert_equal(get(sensors[1], SENSOR_CHAN_ALL, &value), -ENOTSUP);
	zassert_equal(sensor_sample_fetch_chan(sensors[1], SENSOR_CHAN_AMBIENT_TEMP), -ENOTSUP);
}

ZTEST_SUITE(ap3216c, NULL, NULL, NULL, NULL, NULL);
