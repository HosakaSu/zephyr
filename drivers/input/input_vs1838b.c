/*
 * Copyright (c) 2025 Bastien Jauny <bastien.jauny@smile.fr>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(input_vs1838b, CONFIG_INPUT_LOG_LEVEL);

/* A NEC packet is defined by:
 * - a lead burst (2 edges)
 * - an 8-bit address followed by its logical inverse
 * - an 8-bit command followed by its logical inverse
 * - a trailing burst
 */

/* Constants used for parsing the edges buffer for NEC protocol */
#define NEC_LEAD_PULSE_EDGE_OFFSET 0
#define NEC_LEAD_PULSE_EDGE_WIDTH  2

#define NEC_ADDRESS_BYTE_EDGE_OFFSET (NEC_LEAD_PULSE_EDGE_OFFSET + NEC_LEAD_PULSE_EDGE_WIDTH)
#define NEC_ADDRESS_BYTE_EDGE_WIDTH  (2 * BITS_PER_BYTE)

#define NEC_REVERSE_ADDRESS_BYTE_EDGE_OFFSET                                                       \
	(NEC_ADDRESS_BYTE_EDGE_OFFSET + NEC_ADDRESS_BYTE_EDGE_WIDTH)
#define NEC_REVERSE_ADDRESS_BYTE_EDGE_WIDTH (2 * BITS_PER_BYTE)

#define NEC_COMMAND_BYTE_EDGE_OFFSET                                                               \
	(NEC_REVERSE_ADDRESS_BYTE_EDGE_OFFSET + NEC_REVERSE_ADDRESS_BYTE_EDGE_WIDTH)
#define NEC_COMMAND_BYTE_EDGE_WIDTH (2 * BITS_PER_BYTE)

#define NEC_REVERSE_COMMAND_BYTE_EDGE_OFFSET                                                       \
	(NEC_COMMAND_BYTE_EDGE_OFFSET + NEC_COMMAND_BYTE_EDGE_WIDTH)
#define NEC_REVERSE_COMMAND_BYTE_EDGE_WIDTH (2 * BITS_PER_BYTE)

#define NEC_SINGLE_COMMAND_EDGES_COUNT                                                             \
	(NEC_REVERSE_COMMAND_BYTE_EDGE_OFFSET + NEC_REVERSE_COMMAND_BYTE_EDGE_WIDTH + 2)

/* NEC protocol values */
#define NEC_LEAD_PULSE_PERIOD_ON_USEC  9000
#define NEC_LEAD_PULSE_PERIOD_OFF_USEC 4500
#define NEC_BIT_DETECT_PERIOD_NSEC     562500
#define NEC_BIT_DETECT_PERIOD_USEC     (NEC_BIT_DETECT_PERIOD_NSEC / NSEC_PER_USEC)
#define NEC_BIT_0_TOTAL_PERIOD_USEC    1125
#define NEC_BIT_1_TOTAL_PERIOD_USEC    2250
/* The longest interval within a frame is the 9 ms leader pulse. */
#define NEC_FRAME_GAP_USEC             15000U

/* Timing limits in microseconds. */
#define VS1838B_MIN_US(usec, tol) ((usec) - (tol))
#define VS1838B_MAX_US(usec, tol) ((usec) + (tol))

/* Empiric tolerance values. Might be a good idea to put them in the Kconfig? */
#define VS1838B_NEC_LEAD_PULSE_PERIOD_TOLERANCE_USEC 400
#define VS1838B_NEC_BIT_DETECT_PERIOD_TOLERANCE_USEC 150
#define VS1838B_NEC_BIT_0_TOTAL_TOLERANCE_USEC       200
#define VS1838B_NEC_BIT_1_TOTAL_TOLERANCE_USEC       200

/* Pulse duration ranges for the NEC elements. */
#define VS1838B_NEC_LEAD_PULSE_ON_MIN_US                                                           \
	VS1838B_MIN_US(NEC_LEAD_PULSE_PERIOD_ON_USEC, VS1838B_NEC_LEAD_PULSE_PERIOD_TOLERANCE_USEC)
#define VS1838B_NEC_LEAD_PULSE_ON_MAX_US                                                           \
	VS1838B_MAX_US(NEC_LEAD_PULSE_PERIOD_ON_USEC, VS1838B_NEC_LEAD_PULSE_PERIOD_TOLERANCE_USEC)

#define VS1838B_NEC_LEAD_PULSE_OFF_MIN_US                                                          \
	VS1838B_MIN_US(NEC_LEAD_PULSE_PERIOD_OFF_USEC, VS1838B_NEC_LEAD_PULSE_PERIOD_TOLERANCE_USEC)
#define VS1838B_NEC_LEAD_PULSE_OFF_MAX_US                                                          \
	VS1838B_MAX_US(NEC_LEAD_PULSE_PERIOD_OFF_USEC, VS1838B_NEC_LEAD_PULSE_PERIOD_TOLERANCE_USEC)

#define VS1838B_NEC_BIT_DETECT_MIN_US                                                              \
	VS1838B_MIN_US(NEC_BIT_DETECT_PERIOD_USEC, VS1838B_NEC_BIT_DETECT_PERIOD_TOLERANCE_USEC)
#define VS1838B_NEC_BIT_DETECT_MAX_US                                                              \
	VS1838B_MAX_US(NEC_BIT_DETECT_PERIOD_USEC, VS1838B_NEC_BIT_DETECT_PERIOD_TOLERANCE_USEC)

#define VS1838B_NEC_BIT_0_TOTAL_MIN_US                                                             \
	VS1838B_MIN_US(NEC_BIT_0_TOTAL_PERIOD_USEC, VS1838B_NEC_BIT_0_TOTAL_TOLERANCE_USEC)
#define VS1838B_NEC_BIT_0_TOTAL_MAX_US                                                             \
	VS1838B_MAX_US(NEC_BIT_0_TOTAL_PERIOD_USEC, VS1838B_NEC_BIT_0_TOTAL_TOLERANCE_USEC)

#define VS1838B_NEC_BIT_1_TOTAL_MIN_US                                                             \
	VS1838B_MIN_US(NEC_BIT_1_TOTAL_PERIOD_USEC, VS1838B_NEC_BIT_1_TOTAL_TOLERANCE_USEC)
#define VS1838B_NEC_BIT_1_TOTAL_MAX_US                                                             \
	VS1838B_MAX_US(NEC_BIT_1_TOTAL_PERIOD_USEC, VS1838B_NEC_BIT_1_TOTAL_TOLERANCE_USEC)

struct vs1838b_data {
	struct device const *dev;
	struct gpio_callback input_cb;
	struct k_work_delayable decode_work;
	uint32_t edges_cycles[NEC_SINGLE_COMMAND_EDGES_COUNT];
	uint8_t edges_count;
	struct k_sem decode_sem;
};

struct vs1838b_config {
	struct gpio_dt_spec input;
};

static inline bool is_within_range(uint32_t const duration, uint32_t const min, uint32_t const max)
{
	return (duration <= max) && (duration >= min);
}

static bool read_byte_from(uint32_t *const edges_cycles, uint8_t const offset, uint8_t *byte)
{
	/* Make sure we add bits from 0 */
	uint8_t temp_byte = 0;
	uint32_t us_on;
	uint32_t us_total;

	/* Bytes are transmitted LSB first */
	for (uint8_t i = 0; i < BITS_PER_BYTE; ++i) {
		/*
		 * To detect bits and their values we analyze:
		 * - the initial pulse width
		 * - the total period
		 */
		us_on = k_cyc_to_us_floor32(edges_cycles[(2 * i) + offset + 1] -
					    edges_cycles[(2 * i) + offset]);
		us_total = k_cyc_to_us_floor32(edges_cycles[(2 * i) + offset + 2] -
					       edges_cycles[(2 * i) + offset]);

		LOG_DBG("pulse %u us", us_on);
		LOG_DBG("period %u us", us_total);
		if (is_within_range(us_on, VS1838B_NEC_BIT_DETECT_MIN_US,
				    VS1838B_NEC_BIT_DETECT_MAX_US)) {
			if (is_within_range(us_total, VS1838B_NEC_BIT_0_TOTAL_MIN_US,
					    VS1838B_NEC_BIT_0_TOTAL_MAX_US)) {
				/* 0 detected */
			} else if (is_within_range(us_total, VS1838B_NEC_BIT_1_TOTAL_MIN_US,
						   VS1838B_NEC_BIT_1_TOTAL_MAX_US)) {
				/* 1 detected */
				temp_byte += BIT(i);
			} else {
				LOG_WRN("Failed to identify detected bit at position %u", i);
				return false;
			}
		} else {
			LOG_WRN("Failed to detect a valid bit at position %u", i);
			return false;
		}
	}

	*byte = temp_byte;
	return true;
}

static bool detect_leading_burst(uint32_t *const edges_cycles)
{
	uint32_t lead_us_on = k_cyc_to_us_floor32(edges_cycles[1] - edges_cycles[0]);
	uint32_t lead_us_off = k_cyc_to_us_floor32(edges_cycles[2] - edges_cycles[1]);

	return is_within_range(lead_us_on, VS1838B_NEC_LEAD_PULSE_ON_MIN_US,
			       VS1838B_NEC_LEAD_PULSE_ON_MAX_US) &&
	       is_within_range(lead_us_off, VS1838B_NEC_LEAD_PULSE_OFF_MIN_US,
			       VS1838B_NEC_LEAD_PULSE_OFF_MAX_US);
}

static bool read_redundant_byte(uint32_t *const edges_cycles, uint8_t *const byte,
				uint32_t const offset)
{
	uint8_t temp_byte;
	uint8_t reverse_byte;

	if (read_byte_from(edges_cycles, offset, &temp_byte) &&
	    read_byte_from(edges_cycles, offset + (2 * BITS_PER_BYTE), &reverse_byte)) {
		if (temp_byte == (uint8_t)(~reverse_byte)) {
			*byte = temp_byte;
		} else {
			LOG_ERR("Error while decoding byte");
			return false;
		}
	} else {
		LOG_ERR("Error while reading bytes");
		return false;
	}

	return true;
}

static bool read_address_byte(uint32_t *const edges_cycles, uint8_t *const address)
{
	return read_redundant_byte(edges_cycles, address, NEC_ADDRESS_BYTE_EDGE_OFFSET);
}

static bool read_command_byte(uint32_t *const edges_cycles, uint8_t *const command)
{
	return read_redundant_byte(edges_cycles, command, NEC_COMMAND_BYTE_EDGE_OFFSET);
}

static bool detect_last_burst(uint32_t *const edges_cycles)
{
	uint32_t burst_length =
		k_cyc_to_us_floor32(edges_cycles[NEC_SINGLE_COMMAND_EDGES_COUNT - 1] -
				    edges_cycles[NEC_SINGLE_COMMAND_EDGES_COUNT - 2]);

	return is_within_range(burst_length, VS1838B_NEC_BIT_DETECT_MIN_US,
			       VS1838B_NEC_BIT_DETECT_MAX_US);
}

static bool get_address_and_command(uint32_t *const edges_cycles, uint8_t *const address,
				    uint8_t *const command)
{
	if (!detect_leading_burst(edges_cycles)) {
		LOG_DBG("No lead detected");
		return false;
	}

	if (!read_address_byte(edges_cycles, address)) {
		LOG_DBG("No address decoded");
		return false;
	}

	if (!read_command_byte(edges_cycles, command)) {
		LOG_DBG("No command decoded");
		return false;
	}
	if (!detect_last_burst(edges_cycles)) {
		LOG_DBG("No trailing edge detected");
		return false;
	}

	return true;
}

/*
 * Management of the decoding
 */
static void vs1838b_decode_work_handler(struct k_work *item)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(item);
	struct vs1838b_data *data = CONTAINER_OF(dwork, struct vs1838b_data, decode_work);

	if (k_sem_take(&data->decode_sem, K_FOREVER) == 0) {
		uint8_t address_byte;
		uint8_t command_byte;

		if (get_address_and_command(data->edges_cycles, &address_byte, &command_byte)) {
			LOG_DBG("Address: [0x%X] | Command: [0x%X]", address_byte, command_byte);
			if (input_report(data->dev, INPUT_EV_DEVICE, INPUT_MSC_SCAN,
					 (address_byte << 8) | command_byte, true, K_NO_WAIT) < 0) {
				LOG_ERR("Message failed to be enqueued");
			}
		}
	}

	/* Reset the record */
	data->edges_count = 0;
	k_sem_give(&data->decode_sem);
}

/*
 * Internal callback
 */
static void vs1838b_input_callback(struct device const *dev, struct gpio_callback *cb,
				   uint32_t pins)
{
	/*
	 * We want to:
	 * - register the timestamps of interrupts
	 * - try and decode the received bits when we reach the appropriate threshold
	 */
	uint32_t const tick = k_cycle_get_32();
	struct vs1838b_data *data = CONTAINER_OF(cb, struct vs1838b_data, input_cb);


	if (k_sem_take(&data->decode_sem, K_NO_WAIT) != 0) {
		/* Decoding might be pending */
		return;
	}

	/* Keep a complete frame stable until the worker has decoded it. */
	if (data->edges_count == NEC_SINGLE_COMMAND_EDGES_COUNT) {
		k_sem_give(&data->decode_sem);
		return;
	}

	/* Discard incomplete frames after an idle gap. Unsigned subtraction
	 * also handles a cycle-counter wrap within a frame.
	 */
	if ((data->edges_count > 0U) &&
	    (k_cyc_to_us_floor32(tick - data->edges_cycles[data->edges_count - 1U]) >
	     NEC_FRAME_GAP_USEC)) {
		data->edges_count = 0U;
	}
	data->edges_cycles[data->edges_count++] = tick;

	/* If the first 3 edges do not match a leading burst,
	 * shift left the edges_cycles to get rid of leading noises.
	 */
	if ((data->edges_count == 3) && !detect_leading_burst(data->edges_cycles)) {
		data->edges_cycles[0] = data->edges_cycles[1];
		data->edges_cycles[1] = data->edges_cycles[2];
		data->edges_count = 2;
	}

	if (data->edges_count == NEC_SINGLE_COMMAND_EDGES_COUNT) {
		k_work_schedule(&data->decode_work, K_NO_WAIT);
	}
	k_sem_give(&data->decode_sem);
}

static int vs1838b_init(struct device const *dev)
{
	struct vs1838b_config const *config = dev->config;
	struct gpio_dt_spec const *data_input = &config->input;
	struct vs1838b_data *data = dev->data;

	data->dev = dev;

	if (!gpio_is_ready_dt(data_input)) {
		LOG_ERR_DEVICE_NOT_READY(data_input->port);
		return -ENODEV;
	}

	k_sem_init(&data->decode_sem, 1, 1);
	k_work_init_delayable(&data->decode_work, vs1838b_decode_work_handler);
	gpio_init_callback(&data->input_cb, vs1838b_input_callback, BIT(data_input->pin));

	int ret = gpio_pin_configure_dt(data_input, GPIO_INPUT);

	if (ret != 0) {
		return ret;
	}

	ret = gpio_add_callback_dt(data_input, &data->input_cb);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(data_input, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		(void)gpio_remove_callback_dt(data_input, &data->input_cb);
	}

	return ret;
}

#define IR_RECEIVER_DEFINE(node_id, gpio_property)                                                 \
	static struct vs1838b_data data_##node_id;                                                 \
	static const struct vs1838b_config config_##node_id = {                                    \
		.input = GPIO_DT_SPEC_GET(node_id, gpio_property),                                 \
	};                                                                                         \
	DEVICE_DT_DEFINE(node_id, vs1838b_init, NULL, &data_##node_id, &config_##node_id,          \
			 POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

#define VS1838B_DEFINE(node_id) IR_RECEIVER_DEFINE(node_id, data_gpios)
#define GPIO_IR_DEFINE(node_id) IR_RECEIVER_DEFINE(node_id, gpios)

#ifdef CONFIG_INPUT_VS1838B
DT_FOREACH_STATUS_OKAY(vishay_vs1838b, VS1838B_DEFINE)
#endif

#ifdef CONFIG_INPUT_GPIO_IR_RECEIVER
DT_FOREACH_STATUS_OKAY(gpio_ir_receiver, GPIO_IR_DEFINE)
#endif
