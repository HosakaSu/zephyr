/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/timing/timing.h>
#include <zephyr/ztest.h>

static bool released;
static bool stuck_low;
static bool stuck_high;
static int read_error;
static int release_error;
static int discharge_error;
static int report_error;
static int configure_error;
static gpio_flags_t configured_flags;
static uint32_t counter;
static uint32_t report_count;
static int32_t last_value;

static int test_get(const struct gpio_dt_spec *spec)
{
	ARG_UNUSED(spec);
	if (read_error != 0) {
		return read_error;
	}
	return stuck_high || (released && !stuck_low);
}

static int test_set(const struct gpio_dt_spec *spec, int value)
{
	ARG_UNUSED(spec);
	if (value != 0) {
		if (release_error != 0) {
			return release_error;
		}
		released = true;
	} else {
		if (discharge_error != 0) {
			return discharge_error;
		}
		released = false;
	}
	return 0;
}

static int test_configure(const struct gpio_dt_spec *spec, gpio_flags_t flags)
{
	ARG_UNUSED(spec);
	configured_flags = flags;
	return configure_error;
}

static timing_t test_counter(void)
{
	counter += 10U;
	return counter;
}

static uint64_t test_cycles(volatile timing_t *start, volatile timing_t *end)
{
	return (uint32_t)*end - (uint32_t)*start;
}

static int test_report(const struct device *dev, uint16_t code, int32_t value, bool sync,
		       k_timeout_t timeout)
{
	if (report_error != 0) {
		return report_error;
	}
	return input_report_key(dev, code, value, sync, timeout);
}

#define gpio_pin_configure_dt test_configure
#define gpio_pin_get_dt       test_get
#define gpio_pin_set_dt       test_set
#define timing_counter_get    test_counter
#define timing_cycles_get     test_cycles
#define input_report_key      test_report
#include "../../../../../drivers/input/input_gpio_rc_touch.c"
#undef gpio_pin_configure_dt
#undef gpio_pin_get_dt
#undef gpio_pin_set_dt
#undef timing_counter_get
#undef timing_cycles_get
#undef input_report_key

static struct rc_touch_data data;
static const struct rc_touch_config cfg = {
	.pad = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	.poll_interval_ms = 10U,
	.press_percent = 125U,
	.release_percent = 115U,
	.code = INPUT_KEY_4,
};
static const struct device pad = {
	.name = "test-pad",
	.config = &cfg,
	.data = &data,
};

static void event_cb(struct input_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	zassert_equal(event->dev, &pad);
	zassert_equal(event->type, INPUT_EV_KEY);
	zassert_equal(event->code, INPUT_KEY_4);
	zassert_true(event->sync);
	last_value = event->value;
	report_count++;
}

INPUT_CALLBACK_DEFINE(NULL, event_cb, NULL);

static void calibrate(void)
{
	for (uint32_t i = 0U; i < 32U; i++) {
		rc_touch_process(&pad, 4000U);
	}
	zassert_equal(data.baseline, 4000U);
	zassert_equal(report_count, 0U);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&data, 0, sizeof(data));
	data.timeout_cycles = 100U;
	released = false;
	stuck_low = false;
	stuck_high = false;
	read_error = 0;
	release_error = 0;
	discharge_error = 0;
	report_error = 0;
	configure_error = 0;
	configured_flags = 0U;
	counter = 0U;
	report_count = 0U;
	last_value = -1;
}

ZTEST(gpio_rc_touch, test_calibration)
{
	for (uint32_t i = 0U; i < 31U; i++) {
		rc_touch_process(&pad, 0U);
	}
	zassert_equal(report_count, 0U);
	rc_touch_process(&pad, 0U);
	zassert_equal(data.calibration_count, 0U);
	rc_touch_process(&pad, 9000U);
	rc_touch_reset_debounce(&data);
	zassert_equal(data.calibration_sum, 0U);
	calibrate();
}

ZTEST(gpio_rc_touch, test_hysteresis_and_debounce)
{
	calibrate();
	rc_touch_process(&pad, 5000U);
	rc_touch_process(&pad, 5000U);
	zassert_equal(report_count, 0U);
	rc_touch_process(&pad, 4999U);
	for (uint32_t i = 0U; i < 3U; i++) {
		rc_touch_process(&pad, 5000U);
	}
	zassert_equal(report_count, 1U);
	zassert_equal(last_value, 1);
	for (uint32_t i = 0U; i < 8U; i++) {
		rc_touch_process(&pad, 4700U);
	}
	zassert_equal(report_count, 1U);
	for (uint32_t i = 0U; i < 3U; i++) {
		rc_touch_process(&pad, 4600U);
	}
	zassert_equal(report_count, 2U);
	zassert_equal(last_value, 0);
	zassert_equal(data.baseline, 4000U);
}

ZTEST(gpio_rc_touch, test_error_breaks_debounce)
{
	calibrate();
	rc_touch_process(&pad, 6600U);
	rc_touch_process(&pad, 6600U);
	rc_touch_reset_debounce(&data);
	rc_touch_process(&pad, 6600U);
	zassert_equal(report_count, 0U);
	rc_touch_process(&pad, 6600U);
	rc_touch_process(&pad, 6600U);
	zassert_equal(report_count, 1U);
}

ZTEST(gpio_rc_touch, test_queue_full_retry)
{
	calibrate();
	report_error = -ENOMSG;
	for (uint32_t i = 0U; i < 5U; i++) {
		rc_touch_process(&pad, 6600U);
	}
	zassert_false(data.pressed);
	zassert_equal(report_count, 0U);
	report_error = 0;
	rc_touch_process(&pad, 6600U);
	zassert_equal(report_count, 1U);
	zassert_true(data.pressed);
}

ZTEST(gpio_rc_touch, test_measurement_and_timeout)
{
	uint64_t cycles;

	counter = UINT32_MAX - 15U;
	zassert_ok(rc_touch_measure(&pad, &cycles));
	zassert_equal(cycles, 10U);
	zassert_false(released);
	stuck_low = true;
	zassert_equal(rc_touch_measure(&pad, &cycles), -ETIMEDOUT);
	zassert_false(released);
	zassert_true(cycles >= 100U);
	stuck_low = false;
	stuck_high = true;
	zassert_equal(rc_touch_measure(&pad, &cycles), -EIO);
	zassert_false(released);
}

ZTEST(gpio_rc_touch, test_gpio_errors)
{
	uint64_t cycles;

	read_error = -EIO;
	zassert_equal(rc_touch_measure(&pad, &cycles), -EIO);
	zassert_false(released);
	read_error = 0;
	release_error = -EACCES;
	zassert_equal(rc_touch_measure(&pad, &cycles), -EACCES);
	zassert_false(released);
	release_error = 0;
	discharge_error = -EIO;
	zassert_equal(rc_touch_measure(&pad, &cycles), -EIO);
}

ZTEST(gpio_rc_touch, test_initialization)
{
	struct k_work_sync sync;

	configure_error = -ENOTSUP;
	zassert_equal(rc_touch_init(&pad), -ENOTSUP);
	configure_error = 0;
	zassert_ok(rc_touch_init(&pad));
	zassert_equal(configured_flags, GPIO_INPUT | GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN);
	k_work_cancel_delayable_sync(&data.work, &sync);
	zassert_equal(data.dev, &pad);
	zassert_true(data.timeout_cycles > 0U);
}

ZTEST_SUITE(gpio_rc_touch, NULL, NULL, before, NULL, NULL);
