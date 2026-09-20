/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/pulse_io.h>
#include <zephyr/ztest.h>

#define NODE     DT_COMPAT_GET_ANY_STATUS_OKAY(st_stm32_timer_pulse_io)
#define CAPACITY DT_PROP(NODE, zephyr_max_cells)

static const struct device *const dev = DEVICE_DT_GET(NODE);
static struct pulse_io_channel *channel;
static struct pulse_cell cells[CAPACITY];
static struct k_thread worker;
static K_THREAD_STACK_DEFINE(worker_stack, 2048);
static int worker_result;

static struct pulse_io_config normal_config(void)
{
	return (struct pulse_io_config){
		.mode = PULSE_IO_MODE_CELL,
		.dir = PULSE_IO_DIR_TX,
		.resolution_hz = 10000000U,
		.cell_period_ticks = 13U,
	};
}

static struct pulse_io_tx_req request(void)
{
	return (struct pulse_io_tx_req){
		.cells = cells,
		.count = ARRAY_SIZE(cells),
	};
}

static void before(void *fixture)
{
	struct pulse_io_config cfg = normal_config();

	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(dev));
	memset(cells, 0, sizeof(cells));
	zassert_ok(pulse_io_channel_get(dev, 0U, &channel));
	zassert_ok(pulse_io_channel_configure(dev, channel, &cfg));
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(pulse_io_channel_release(dev, channel));
}

ZTEST(stm32_timer, test_capabilities_and_reservation)
{
	struct pulse_io_caps caps;
	struct pulse_io_channel *other;

	zassert_ok(pulse_io_get_capabilities(dev, &caps));
	zassert_equal(caps.modes, PULSE_IO_MODE_CELL);
	zassert_true(caps.supports_tx);
	zassert_false(caps.supports_rx);
	zassert_equal(caps.num_channels, 1U);
	zassert_equal(caps.cell_duty_bits, 16U);
	zassert_equal(caps.tx_max_streaming, CAPACITY);
	zassert_equal(pulse_io_channel_get(dev, 0U, &other), -EBUSY);
	zassert_equal(pulse_io_channel_get(dev, 1U, &other), -ENODEV);
	zassert_equal(pulse_io_stop(dev, NULL), -EINVAL);
	zassert_equal(pulse_io_channel_release(dev, NULL), -EINVAL);
}

ZTEST(stm32_timer, test_configuration_limits)
{
	struct pulse_io_config cfg = normal_config();

	cfg.mode = PULSE_IO_MODE_SYMBOL;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -ENOTSUP);
	cfg = normal_config();
	cfg.dir = PULSE_IO_DIR_RX;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -ENOTSUP);
	cfg = normal_config();
	cfg.idle_high = true;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -ENOTSUP);
	cfg = normal_config();
	cfg.carrier_en = true;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -ENOTSUP);
	cfg = normal_config();
	cfg.resolution_hz = 0U;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -EINVAL);
	cfg = normal_config();
	cfg.cell_period_ticks = 0U;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -EINVAL);
	cfg.cell_period_ticks = 65536U;
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -EINVAL);
}

ZTEST(stm32_timer, test_request_limits)
{
	struct pulse_io_tx_req req = request();

	req.count = CAPACITY + 1U;
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
	req = request();
	req.cells = NULL;
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
	req.count = 0U;
	zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
	req = request();
	req.loop_count = 1U;
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -ENOTSUP);
	req.loop_count = 0U;
	cells[0].duty = 14U;
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
	cells[0].duty = 0U;
	cells[0].rsvd = 1U;
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
}

ZTEST(stm32_timer, test_dma_repeated_and_single_cell)
{
	struct pulse_io_tx_req req = request();

	/* Alternate DMA buffer contents while the data cache is enabled. */
	for (uint32_t run = 0U; run < 20U; run++) {
		for (size_t i = 0U; i < ARRAY_SIZE(cells); i++) {
			cells[i].duty = (run & 1U) != 0U ? 4U : 0U;
		}
		zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
	}
	req.count = 1U;
	cells[0].duty = 0U;
	zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
	cells[0].duty = 13U;
	zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
}

ZTEST(stm32_timer, test_timeout_and_recovery)
{
	struct pulse_io_tx_req req = request();
	struct pulse_io_config cfg = normal_config();

	cfg.resolution_hz = 1000000U;
	cfg.cell_period_ticks = 60000U;
	zassert_ok(pulse_io_channel_configure(dev, channel, &cfg));
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(1)), -ETIMEDOUT);
	cfg = normal_config();
	zassert_ok(pulse_io_channel_configure(dev, channel, &cfg));
	zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
}

static void transmit_worker(void *arg1, void *arg2, void *arg3)
{
	struct pulse_io_tx_req req = request();

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	worker_result = pulse_io_transmit_sync(dev, channel, &req, K_SECONDS(2));
}

ZTEST(stm32_timer, test_cancel_and_recovery)
{
	struct pulse_io_config cfg = normal_config();
	struct pulse_io_tx_req req = request();
	k_tid_t tid;

	cfg.resolution_hz = 1000000U;
	cfg.cell_period_ticks = 60000U;
	zassert_ok(pulse_io_channel_configure(dev, channel, &cfg));
	tid = k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
			      transmit_worker, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_sleep(K_MSEC(10));
	zassert_equal(pulse_io_channel_release(dev, channel), -EBUSY);
	zassert_equal(pulse_io_channel_configure(dev, channel, &cfg), -EBUSY);
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_NO_WAIT), -EBUSY);
	zassert_ok(pulse_io_stop(dev, channel));
	zassert_ok(k_thread_join(tid, K_SECONDS(1)));
	zassert_equal(worker_result, -ECANCELED);
	cfg = normal_config();
	zassert_ok(pulse_io_channel_configure(dev, channel, &cfg));
	zassert_ok(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)));
}

ZTEST(stm32_timer, test_release_requires_reconfigure)
{
	struct pulse_io_tx_req req = request();
	struct pulse_io_rx_req rx = {0};
	size_t count;

	zassert_ok(pulse_io_channel_release(dev, channel));
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
	zassert_ok(pulse_io_channel_get(dev, 0U, &channel));
	zassert_equal(pulse_io_transmit_sync(dev, channel, &req, K_MSEC(100)), -EINVAL);
	zassert_equal(pulse_io_receive_sync(dev, channel, &rx, &count, K_NO_WAIT), -ENOTSUP);
}

ZTEST_SUITE(stm32_timer, NULL, NULL, before, after, NULL);
