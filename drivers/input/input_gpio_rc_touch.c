/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_gpio_rc_touch

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/timing/timing.h>

LOG_MODULE_REGISTER(gpio_rc_touch, CONFIG_INPUT_LOG_LEVEL);

#define RC_CALIBRATION_SAMPLES 32U
#define RC_DEBOUNCE_SAMPLES    3U
#define RC_TIMEOUT_US          100U

struct rc_touch_config {
	struct gpio_dt_spec pad;
	uint32_t poll_interval_ms;
	uint16_t press_percent;
	uint16_t release_percent;
	uint16_t code;
};

struct rc_touch_data {
	const struct device *dev;
	struct k_work_delayable work;
	uint64_t timeout_cycles;
	uint64_t calibration_sum;
	uint64_t baseline;
	uint8_t calibration_count;
	uint8_t debounce_count;
	bool pressed;
};

static int rc_touch_measure(const struct device *dev, uint64_t *cycles)
{
	const struct rc_touch_config *cfg = dev->config;
	struct rc_touch_data *data = dev->data;
	timing_t start;
	timing_t end;
	unsigned int key;
	int level;
	int ret;
	int discharge_ret;

	*cycles = 0U;
	/* The pad has been held low since the preceding scan. */
	level = gpio_pin_get_dt(&cfg->pad);
	if (level != 0) {
		ret = level < 0 ? level : -EIO;
		goto discharge;
	}

	key = irq_lock();
	start = timing_counter_get();
	ret = gpio_pin_set_dt(&cfg->pad, 1);
	if (ret == 0) {
		do {
			level = gpio_pin_get_dt(&cfg->pad);
			end = timing_counter_get();
			*cycles = timing_cycles_get(&start, &end);
		} while ((level == 0) && (*cycles < data->timeout_cycles));

		if (level < 0) {
			ret = level;
		} else if (*cycles >= data->timeout_cycles) {
			ret = -ETIMEDOUT;
		}
	}
	irq_unlock(key);

discharge:
	/* Always restore discharge, including after a GPIO error or timeout. */
	discharge_ret = gpio_pin_set_dt(&cfg->pad, 0);
	return ret != 0 ? ret : discharge_ret;
}

static void rc_touch_reset_debounce(struct rc_touch_data *data)
{
	data->debounce_count = 0U;
	if (data->calibration_count < RC_CALIBRATION_SAMPLES) {
		data->calibration_count = 0U;
		data->calibration_sum = 0U;
	}
}

static void rc_touch_process(const struct device *dev, uint64_t cycles)
{
	const struct rc_touch_config *cfg = dev->config;
	struct rc_touch_data *data = dev->data;
	bool pressed;
	int ret;

	if (data->calibration_count < RC_CALIBRATION_SAMPLES) {
		data->calibration_sum += cycles;
		data->calibration_count++;
		if (data->calibration_count == RC_CALIBRATION_SAMPLES) {
			data->baseline = data->calibration_sum / RC_CALIBRATION_SAMPLES;
			if (data->baseline == 0U) {
				data->calibration_count = 0U;
				data->calibration_sum = 0U;
				LOG_ERR("Insufficient timing resolution");
				return;
			}
			LOG_INF("Calibration complete: %llu cycles",
				(unsigned long long)data->baseline);
		}
		return;
	}

	if (data->pressed) {
		pressed = cycles * 100U > data->baseline * cfg->release_percent;
	} else {
		pressed = cycles * 100U >= data->baseline * cfg->press_percent;
	}

	if (pressed == data->pressed) {
		data->debounce_count = 0U;
		return;
	}
	if (data->debounce_count < RC_DEBOUNCE_SAMPLES) {
		data->debounce_count++;
	}
	if (data->debounce_count == RC_DEBOUNCE_SAMPLES) {
		ret = input_report_key(dev, cfg->code, pressed, true, K_NO_WAIT);
		if (ret == 0) {
			data->pressed = pressed;
			data->debounce_count = 0U;
		}
	}
}

static void rc_touch_work(struct k_work *work)
{
	struct rc_touch_data *data =
		CONTAINER_OF(k_work_delayable_from_work(work), struct rc_touch_data, work);
	const struct rc_touch_config *cfg = data->dev->config;
	uint64_t cycles;
	int ret;

	ret = rc_touch_measure(data->dev, &cycles);
	if (ret == 0) {
		rc_touch_process(data->dev, cycles);
	} else {
		rc_touch_reset_debounce(data);
		LOG_WRN_RATELIMIT("Charge measurement failed: %d", ret);
	}
	k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

static int rc_touch_init(const struct device *dev)
{
	const struct rc_touch_config *cfg = dev->config;
	struct rc_touch_data *data = dev->data;
	int ret;

	if (!gpio_is_ready_dt(&cfg->pad)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->pad, GPIO_INPUT | GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN);
	if (ret != 0) {
		return ret;
	}

	timing_init();
	timing_start();
	if (timing_freq_get() < 1000000U) {
		timing_stop();
		return -ENOTSUP;
	}
	data->timeout_cycles = timing_freq_get() * RC_TIMEOUT_US / 1000000U;
	data->dev = dev;
	k_work_init_delayable(&data->work, rc_touch_work);
	k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
	return 0;
}

#define RC_TOUCH_DEFINE(inst)                                                                      \
	BUILD_ASSERT(DT_INST_PROP(inst, poll_interval_ms) >= 5, "Discharge needs at least 5 ms");  \
	BUILD_ASSERT(DT_INST_PROP(inst, release_threshold_percent) > 100 &&                        \
			     DT_INST_PROP(inst, press_threshold_percent) >                         \
				     DT_INST_PROP(inst, release_threshold_percent) &&              \
			     DT_INST_PROP(inst, press_threshold_percent) <= 1000,                  \
		     "Thresholds must satisfy 100 < release < press <= 1000");                     \
	BUILD_ASSERT((DT_INST_GPIO_FLAGS(inst, gpios) &                                            \
		      (GPIO_ACTIVE_LOW | GPIO_PULL_UP | GPIO_PULL_DOWN)) == 0,                     \
		     "The pad requires active-high open-drain GPIO without pulls");                \
	static struct rc_touch_data rc_touch_data_##inst;                                          \
	static const struct rc_touch_config rc_touch_config_##inst = {                             \
		.pad = GPIO_DT_SPEC_INST_GET(inst, gpios),                                         \
		.poll_interval_ms = DT_INST_PROP(inst, poll_interval_ms),                          \
		.press_percent = DT_INST_PROP(inst, press_threshold_percent),                      \
		.release_percent = DT_INST_PROP(inst, release_threshold_percent),                  \
		.code = DT_INST_PROP(inst, zephyr_code),                                           \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, rc_touch_init, NULL, &rc_touch_data_##inst,                    \
			      &rc_touch_config_##inst, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,    \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(RC_TOUCH_DEFINE)
