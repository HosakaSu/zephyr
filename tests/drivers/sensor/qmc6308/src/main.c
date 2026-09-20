/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qst_qmc6308

#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

struct qmc6308_emul_data {
	uint8_t regs[0x30];
	int fail_reg;
};

struct qmc6308_emul_config {
	uint16_t address;
};

static int emul_init(const struct emul *emul, const struct device *parent)
{
	struct qmc6308_emul_data *data = emul->data;
	const struct qmc6308_emul_config *config = emul->cfg;

	ARG_UNUSED(parent);
	data->fail_reg = -1;
	data->regs[0] = (config->address == 0x30U) ? 0xffU : 0x80U;
	data->regs[9] = (config->address == 0x31U) ? 0U : 0x18U;
	return 0;
}

static int emul_transfer(const struct emul *emul, struct i2c_msg *msgs, int num_msgs, int addr)
{
	struct qmc6308_emul_data *data = emul->data;
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
		if ((reg == 0x0bU) && (msgs[0].buf[1] == 0x80U)) {
			memset(data->regs, 0, sizeof(data->regs));
			emul_init(emul, NULL);
		}
		data->regs[reg] = msgs[0].buf[1];
		return 0;
	}
	if ((num_msgs == 2) && (msgs[0].len == 1U) && ((msgs[1].flags & I2C_MSG_READ) != 0U) &&
	    ((reg + msgs[1].len) <= sizeof(data->regs))) {
		memcpy(msgs[1].buf, &data->regs[reg], msgs[1].len);
		if (reg == 9U) {
			data->regs[9] &= ~0x03U;
		}
		return 0;
	}
	return -EIO;
}

static const struct i2c_emul_api emul_api = {
	.transfer = emul_transfer,
};

#define EMUL_DEFINE(inst)                                                                          \
	static struct qmc6308_emul_data emul_data_##inst;                                          \
	static const struct qmc6308_emul_config emul_config_##inst = {                             \
		.address = DT_INST_REG_ADDR(inst),                                                 \
	};                                                                                         \
	EMUL_DT_INST_DEFINE(inst, emul_init, &emul_data_##inst, &emul_config_##inst, &emul_api,    \
			    NULL);

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

static void load(size_t index, int16_t x, int16_t y, int16_t z)
{
	struct qmc6308_emul_data *data = emulators[index]->data;

	data->fail_reg = -1;
	data->regs[9] = 0x19;
	sys_put_le16((uint16_t)x, &data->regs[1]);
	sys_put_le16((uint16_t)y, &data->regs[3]);
	sys_put_le16((uint16_t)z, &data->regs[5]);
}

static int64_t microgauss(const struct sensor_value *value)
{
	return (int64_t)value->val1 * 1000000 + value->val2;
}

ZTEST(qmc6308, test_initialization)
{
	for (size_t i = 0U; i < ARRAY_SIZE(sensors); i++) {
		struct qmc6308_emul_data *data = emulators[i]->data;

		zassert_true(device_is_ready(sensors[i]));
		zassert_equal(data->regs[0x0a], 0xc1U | (i << 2));
		zassert_equal(data->regs[0x0b], i << 2);
		zassert_equal(data->regs[0x0d], 0x40U);
	}
	zassert_false(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(sensor4))));
	zassert_false(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(sensor5))));
}

ZTEST(qmc6308, test_ranges_and_signed_data)
{
	const int16_t counts[] = {1000, 2500, 3750, 15000};
	struct sensor_value value[3];

	for (size_t i = 0U; i < ARRAY_SIZE(sensors); i++) {
		load(i, counts[i], -counts[i], 0);
		zassert_ok(sensor_sample_fetch(sensors[i]));
		zassert_ok(sensor_channel_get(sensors[i], SENSOR_CHAN_MAGN_XYZ, value));
		zassert_equal(microgauss(&value[0]), 1000000);
		zassert_equal(microgauss(&value[1]), -1000000);
		zassert_equal(microgauss(&value[2]), 0);
	}
}

ZTEST(qmc6308, test_limits_and_fraction)
{
	struct sensor_value value[3];

	load(0, -32768, 32767, -1);
	zassert_ok(sensor_sample_fetch(sensors[0]));
	zassert_ok(sensor_channel_get(sensors[0], SENSOR_CHAN_MAGN_XYZ, value));
	zassert_equal(microgauss(&value[0]), -32768000);
	zassert_equal(microgauss(&value[1]), 32767000);
	zassert_equal(microgauss(&value[2]), -1000);
	zassert_ok(sensor_channel_get(sensors[0], SENSOR_CHAN_MAGN_Z, value));
	zassert_equal(microgauss(&value[0]), -1000);
}

ZTEST(qmc6308, test_status_read_to_clear)
{
	struct qmc6308_emul_data *data = emulators[0]->data;

	load(0, 100, 200, 300);
	zassert_ok(sensor_sample_fetch(sensors[0]));
	zassert_equal(sensor_sample_fetch(sensors[0]), -EAGAIN);
	data->regs[9] = 0x1b;
	zassert_equal(sensor_sample_fetch(sensors[0]), -ERANGE);
	zassert_equal(sensor_sample_fetch(sensors[0]), -EAGAIN);
}

ZTEST(qmc6308, test_i2c_errors_preserve_sample)
{
	struct qmc6308_emul_data *data = emulators[0]->data;
	struct sensor_value value;

	load(0, 1000, 2000, 3000);
	zassert_ok(sensor_sample_fetch(sensors[0]));
	load(0, 1, 2, 3);
	data->fail_reg = 9;
	zassert_equal(sensor_sample_fetch(sensors[0]), -EIO);
	data->fail_reg = 1;
	zassert_equal(sensor_sample_fetch(sensors[0]), -EIO);
	zassert_ok(sensor_channel_get(sensors[0], SENSOR_CHAN_MAGN_X, &value));
	zassert_equal(microgauss(&value), 1000000);
	data->fail_reg = -1;
}

ZTEST(qmc6308, test_unsupported_channels)
{
	struct sensor_value value;

	zassert_equal(sensor_sample_fetch_chan(sensors[0], SENSOR_CHAN_LIGHT), -ENOTSUP);
	zassert_equal(sensor_channel_get(sensors[0], SENSOR_CHAN_LIGHT, &value), -ENOTSUP);
}

ZTEST_SUITE(qmc6308, NULL, NULL, NULL, NULL, NULL);
