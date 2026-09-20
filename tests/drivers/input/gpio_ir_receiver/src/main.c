/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const ir = DEVICE_DT_GET(DT_NODELABEL(ir));
static const struct device *const legacy_ir = DEVICE_DT_GET(DT_NODELABEL(legacy_ir));
static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static uint32_t event_count;
static int32_t last_value;
static const struct device *last_device;

static void event_cb(struct input_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	last_device = event->dev;
	last_value = event->value;
	event_count++;
}

INPUT_CALLBACK_DEFINE(NULL, event_cb, NULL);

static void edge(uint32_t pin, uint32_t level, uint32_t delay_us)
{
	k_busy_wait(delay_us);
	zassert_ok(gpio_emul_input_set(gpio, pin, level));
}

static void frame(uint32_t pin, uint8_t address, uint8_t inverse_address, uint8_t command,
		  uint8_t inverse_command, uint32_t bits)
{
	uint32_t data = address | ((uint32_t)inverse_address << 8) | ((uint32_t)command << 16) |
			((uint32_t)inverse_command << 24);

	edge(pin, 1U, 0U);
	edge(pin, 0U, 20000U);
	edge(pin, 1U, 9000U);
	edge(pin, 0U, 4500U);
	for (uint32_t i = 0U; i < bits; i++) {
		edge(pin, 1U, 563U);
		edge(pin, 0U, (data & BIT(i)) != 0U ? 1687U : 562U);
	}
	edge(pin, 1U, 563U);
	k_msleep(60);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(ir));
	zassert_true(device_is_ready(legacy_ir));
	event_count = 0U;
}

ZTEST(gpio_ir_receiver, test_standard_frame)
{
	frame(0U, 0x00U, 0xffU, 0x45U, 0xbaU, 32U);
	zassert_equal(event_count, 1U);
	zassert_equal(last_value, 0x45);
	zassert_equal(last_device, ir);
}

ZTEST(gpio_ir_receiver, test_legacy_binding)
{
	frame(1U, 0x34U, 0xcbU, 0xa5U, 0x5aU, 32U);
	zassert_equal(event_count, 1U);
	zassert_equal(last_value, 0x34a5);
	zassert_equal(last_device, legacy_ir);
}

ZTEST(gpio_ir_receiver, test_bad_checksums)
{
	frame(0U, 0x00U, 0xfeU, 0x45U, 0xbaU, 32U);
	frame(0U, 0x00U, 0xffU, 0x45U, 0xbbU, 32U);
	zassert_equal(event_count, 0U);
	frame(0U, 0x00U, 0xffU, 0x46U, 0xb9U, 32U);
	zassert_equal(event_count, 1U);
	zassert_equal(last_value, 0x46);
}

ZTEST(gpio_ir_receiver, test_truncated_frame_recovery)
{
	frame(0U, 0x00U, 0xffU, 0x45U, 0xbaU, 10U);
	zassert_equal(event_count, 0U);
	frame(0U, 0x00U, 0xffU, 0x47U, 0xb8U, 32U);
	zassert_equal(event_count, 1U);
	zassert_equal(last_value, 0x47);
}

ZTEST(gpio_ir_receiver, test_noise_recovery)
{
	edge(0U, 0U, 20000U);
	edge(0U, 1U, 2000U);
	edge(0U, 0U, 1000U);
	edge(0U, 1U, 563U);
	k_msleep(60);
	zassert_equal(event_count, 0U);
	frame(0U, 0x00U, 0xffU, 0x44U, 0xbbU, 32U);
	zassert_equal(event_count, 1U);
	zassert_equal(last_value, 0x44);
}

ZTEST(gpio_ir_receiver, test_repeat_frame_ignored)
{
	frame(0U, 0x00U, 0xffU, 0x40U, 0xbfU, 32U);
	edge(0U, 0U, 20000U);
	edge(0U, 1U, 9000U);
	edge(0U, 0U, 2250U);
	edge(0U, 1U, 563U);
	k_msleep(60);
	zassert_equal(event_count, 1U);
	frame(0U, 0x00U, 0xffU, 0x43U, 0xbcU, 32U);
	zassert_equal(event_count, 2U);
	zassert_equal(last_value, 0x43);
}

ZTEST_SUITE(gpio_ir_receiver, NULL, NULL, before, NULL, NULL);
