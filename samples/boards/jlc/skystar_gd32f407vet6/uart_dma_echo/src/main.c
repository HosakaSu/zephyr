/*
 * Copyright (c) 2026, Liu Changjie <liucj1228@outlook.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define UART_NODE     DT_CHOSEN(zephyr_console)
#define RX_BUF_COUNT  2U
#define RX_BUF_SIZE   32U
#define RX_TIMEOUT_US 10000
#define TX_QUEUE_SIZE 128U

static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);
static uint8_t rx_buf[RX_BUF_COUNT][RX_BUF_SIZE];
static uint8_t next_rx_buf = 1U;
static uint8_t tx_byte;
static volatile bool tx_busy;

K_MSGQ_DEFINE(tx_queue, sizeof(uint8_t), TX_QUEUE_SIZE, 1);

static void start_rx(const struct device *dev)
{
	int ret;

	next_rx_buf = 1U;
	ret = uart_rx_enable(dev, rx_buf[0], sizeof(rx_buf[0]), RX_TIMEOUT_US);

	if (ret != 0) {
		k_oops();
	}
}

static void start_tx(const struct device *dev)
{
	int ret;

	if (tx_busy || k_msgq_get(&tx_queue, &tx_byte, K_NO_WAIT) != 0) {
		return;
	}

	ret = uart_tx(dev, &tx_byte, sizeof(tx_byte), SYS_FOREVER_US);
	if (ret == 0) {
		tx_busy = true;
	}
}

static void queue_rx_data(const struct device *dev, const struct uart_event *evt)
{
	for (size_t i = 0U; i < evt->data.rx.len; i++) {
		uint8_t byte = evt->data.rx.buf[evt->data.rx.offset + i];

		if (k_msgq_put(&tx_queue, &byte, K_NO_WAIT) != 0) {
			break;
		}
	}

	start_tx(dev);
}

static void uart_dma_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		tx_busy = false;
		start_tx(dev);
		break;
	case UART_RX_RDY:
		queue_rx_data(dev, evt);
		break;
	case UART_RX_BUF_REQUEST:
		if (uart_rx_buf_rsp(dev, rx_buf[next_rx_buf], sizeof(rx_buf[next_rx_buf])) == 0) {
			next_rx_buf = (next_rx_buf + 1U) % RX_BUF_COUNT;
		}
		break;
	case UART_RX_DISABLED:
		start_rx(dev);
		break;
	default:
		break;
	}
}

int main(void)
{
	int ret;

	if (!device_is_ready(uart_dev)) {
		return 0;
	}

	ret = uart_callback_set(uart_dev, uart_dma_callback, NULL);
	if (ret != 0) {
		return 0;
	}

	start_rx(uart_dev);
	printk("UART DMA echo ready\n");

	while (true) {
		k_sleep(K_FOREVER);
	}
}
