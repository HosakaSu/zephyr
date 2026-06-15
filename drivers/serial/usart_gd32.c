/*
 * Copyright (c) 2021, ATL Electronics
 * Copyright (c) 2025 Aleksandr Senin <al@meshium.net>
 * Copyright (c) 2026 Liu Changjie <liucj1228@outlook.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_usart

#include <errno.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <gd32_usart.h>

#ifdef CONFIG_UART_ASYNC_API
#define GD32_USART_ASYNC 1
#else
#define GD32_USART_ASYNC 0
#endif

#if GD32_USART_ASYNC
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_gd32.h>
#endif

/* Unify GD32 HAL USART status register name to USART_STAT */
#ifndef USART_STAT
#define USART_STAT USART_STAT0
#endif

#if GD32_USART_ASYNC
struct gd32_usart_dma_stream {
	const struct device *dma_dev;
	uint32_t dma_channel;
	uint32_t slot;
	uint32_t config;
};
#endif /* GD32_USART_ASYNC */

struct gd32_usart_config {
	uint32_t reg;
	uint16_t clkid;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	uint32_t parity;
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || GD32_USART_ASYNC
	uart_irq_config_func_t irq_config_func;
#endif
#if GD32_USART_ASYNC
	struct gd32_usart_dma_stream dma_rx;
	struct gd32_usart_dma_stream dma_tx;
#endif /* GD32_USART_ASYNC */
};

struct gd32_usart_data {
	uint32_t baud_rate;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t user_cb;
	void *user_data;
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	enum uart_config_parity parity;
	enum uart_config_stop_bits stop_bits;
	enum uart_config_data_bits data_bits;
	enum uart_config_flow_control flow_ctrl;
	bool initialized;
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
#if GD32_USART_ASYNC
	const struct device *dev;
	uart_callback_t async_cb;
	void *async_user_data;
	/* DMA TX */
	struct dma_config dma_tx_cfg;
	struct dma_block_config dma_tx_blk;
	const uint8_t *tx_buf;
	size_t tx_len;
	bool tx_busy;
	bool tx_dma_done;
	/* DMA RX */
	struct dma_config dma_rx_cfg;
	struct dma_block_config dma_rx_blk;
	uint8_t *rx_buf;
	size_t rx_len;
	size_t rx_offset;
	size_t rx_idle_count;
	uint8_t *rx_next_buf;
	size_t rx_next_len;
	int32_t rx_timeout;
	struct k_work_delayable rx_timeout_work;
	bool rx_enabled;
	bool rx_stopping;
#endif /* GD32_USART_ASYNC */
};

#if GD32_USART_ASYNC
static void usart_gd32_async_rx_timeout(struct k_work *work);
static size_t usart_gd32_dma_rx_count(const struct device *dev);
static void usart_gd32_async_rx_stop(const struct device *dev, enum uart_rx_stop_reason reason);
static void usart_gd32_async_tx_complete(const struct device *dev, enum uart_event_type type,
					 size_t len);
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || GD32_USART_ASYNC
static void usart_gd32_isr(const struct device *dev)
{
	struct gd32_usart_data *const data = dev->data;

#if GD32_USART_ASYNC
	const struct gd32_usart_config *const cfg = dev->config;
	uint32_t status = USART_STAT(cfg->reg);
	enum uart_rx_stop_reason reason = 0;

	if (data->rx_enabled) {
		if ((status & USART_FLAG_ORERR) != 0U) {
			reason |= UART_ERROR_OVERRUN;
		}
		if ((status & USART_FLAG_PERR) != 0U) {
			reason |= UART_ERROR_PARITY;
		}
		if ((status & USART_FLAG_FERR) != 0U) {
			reason |= UART_ERROR_FRAMING;
		}
		if ((status & USART_FLAG_NERR) != 0U) {
			reason |= UART_ERROR_NOISE;
		}
	}

	if (reason != 0) {
		/* Reading DATA after STAT clears the USART error flags. */
		(void)USART_DATA(cfg->reg);
		usart_gd32_async_rx_stop(dev, reason);
	} else if (data->rx_enabled && usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_IDLE)) {
		/*
		 * On GD32F4 the IDLE flag is cleared by reading STAT0 followed
		 * by DATA. DMA has already consumed the last received byte.
		 */
		(void)USART_STAT(cfg->reg);
		(void)USART_DATA(cfg->reg);

		if (data->rx_timeout != SYS_FOREVER_US) {
			data->rx_idle_count = usart_gd32_dma_rx_count(dev);
			k_work_reschedule(&data->rx_timeout_work,
					  data->rx_timeout == 0 ? K_NO_WAIT
								: K_USEC(data->rx_timeout));
		}
	}

	if (data->tx_busy && data->tx_dma_done &&
	    usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_TC)) {
		usart_interrupt_disable(cfg->reg, USART_INT_TC);
		usart_interrupt_flag_clear(cfg->reg, USART_INT_FLAG_TC);
		usart_gd32_async_tx_complete(dev, UART_TX_DONE, data->tx_len);
	}
#endif

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	if (data->user_cb) {
		data->user_cb(dev, data->user_data);
	}
#endif
}
#endif

static int usart_gd32_init(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;
	struct gd32_usart_data *const data = dev->data;
	uint32_t word_length;
	uint32_t parity;
	int ret;

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/**
	 * In order to keep the transfer data size to 8 bits(1 byte),
	 * append word length to 9BIT if parity bit enabled.
	 */
	switch (cfg->parity) {
	case UART_CFG_PARITY_NONE:
		parity = USART_PM_NONE;
		word_length = USART_WL_8BIT;
		break;
	case UART_CFG_PARITY_ODD:
		parity = USART_PM_ODD;
		word_length = USART_WL_9BIT;
		break;
	case UART_CFG_PARITY_EVEN:
		parity = USART_PM_EVEN;
		word_length = USART_WL_9BIT;
		break;
	default:
		return -ENOTSUP;
	}

	(void)clock_control_on(GD32_CLOCK_CONTROLLER,
			       (clock_control_subsys_t)&cfg->clkid);

	(void)reset_line_toggle_dt(&cfg->reset);

	usart_baudrate_set(cfg->reg, data->baud_rate);
	usart_parity_config(cfg->reg, parity);
	usart_word_length_set(cfg->reg, word_length);
	/* Default to 1 stop bit */
	usart_stop_bit_set(cfg->reg, USART_STB_1BIT);
	usart_receive_config(cfg->reg, USART_RECEIVE_ENABLE);
	usart_transmit_config(cfg->reg, USART_TRANSMIT_ENABLE);
	usart_enable(cfg->reg);

#if GD32_USART_ASYNC
	data->dev = dev;
	k_work_init_delayable(&data->rx_timeout_work, usart_gd32_async_rx_timeout);
	usart_dma_receive_config(cfg->reg, USART_DENR_DISABLE);
	usart_dma_transmit_config(cfg->reg, USART_DENT_DISABLE);
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || GD32_USART_ASYNC
	cfg->irq_config_func(dev);
#endif
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	/* Initialize runtime configuration from Devicetree defaults */
	data->parity = cfg->parity;
	data->data_bits = UART_CFG_DATA_BITS_8;
	data->stop_bits = UART_CFG_STOP_BITS_1;
	data->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	data->initialized = true;
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
	return 0;
}

static int usart_gd32_poll_in(const struct device *dev, unsigned char *c)
{
	const struct gd32_usart_config *const cfg = dev->config;
	uint32_t status;

	status = usart_flag_get(cfg->reg, USART_FLAG_RBNE);

	if (!status) {
		return -EPERM;
	}

	*c = (unsigned char)usart_data_receive(cfg->reg);

	return 0;
}

static void usart_gd32_poll_out(const struct device *dev, unsigned char c)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_data_transmit(cfg->reg, c);

	while (usart_flag_get(cfg->reg, USART_FLAG_TBE) == RESET) {
		;
	}
}

static int usart_gd32_err_check(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;
	uint32_t status = USART_STAT(cfg->reg);
	int errors = 0;

	if (status & USART_FLAG_ORERR) {
		usart_flag_clear(cfg->reg, USART_FLAG_ORERR);

		errors |= UART_ERROR_OVERRUN;
	}

	if (status & USART_FLAG_PERR) {
		usart_flag_clear(cfg->reg, USART_FLAG_PERR);

		errors |= UART_ERROR_PARITY;
	}

	if (status & USART_FLAG_FERR) {
		usart_flag_clear(cfg->reg, USART_FLAG_FERR);

		errors |= UART_ERROR_FRAMING;
	}

	if (status & USART_FLAG_NERR) {
		usart_flag_clear(cfg->reg, USART_FLAG_NERR);

		errors |= UART_ERROR_NOISE;
	}

	return errors;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int usart_gd32_configure(const struct device *dev, const struct uart_config *cfg_new)
{
	const struct gd32_usart_config *const cfg = dev->config;
	struct gd32_usart_data *const data = dev->data;
	uint32_t parity_bits;
	uint32_t word_length;
	uint32_t stop_bits_hw;

	if (cfg_new == NULL) {
		return -EINVAL;
	}

	if (cfg_new->baudrate == 0U) {
		return -EINVAL;
	}

	if (cfg_new->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) {
		return -ENOTSUP;
	}

#if GD32_USART_ASYNC
	if (data->tx_busy || data->rx_enabled) {
		return -EBUSY;
	}
#endif

	switch (cfg_new->parity) {
	case UART_CFG_PARITY_NONE:
		parity_bits = USART_PM_NONE;
		break;
	case UART_CFG_PARITY_ODD:
		parity_bits = USART_PM_ODD;
		break;
	case UART_CFG_PARITY_EVEN:
		parity_bits = USART_PM_EVEN;
		break;
	default:
		return -EINVAL;
	}

	switch (cfg_new->data_bits) {
	case UART_CFG_DATA_BITS_8:
	case UART_CFG_DATA_BITS_7:
		break;
	default:
		return -EINVAL;
	}

	if (cfg_new->data_bits == UART_CFG_DATA_BITS_7 && cfg_new->parity == UART_CFG_PARITY_NONE) {
		return -EINVAL;
	}

	/* Map word length depending on requested data bits and parity */
	if (cfg_new->parity == UART_CFG_PARITY_NONE) {
		/* 8N* uses 8-bit word length */
		word_length = USART_WL_8BIT;
	} else {
		/* With parity: 8 data bits -> 9-bit word length, 7 data bits -> 8-bit */
		word_length = (cfg_new->data_bits == UART_CFG_DATA_BITS_8) ? USART_WL_9BIT
									   : USART_WL_8BIT;
	}

	switch (cfg_new->stop_bits) {
	case UART_CFG_STOP_BITS_1:
		stop_bits_hw = USART_STB_1BIT;
		break;
	case UART_CFG_STOP_BITS_2:
		stop_bits_hw = USART_STB_2BIT;
		break;
	default:
		return -EINVAL;
	}

	if (data->baud_rate == cfg_new->baudrate && data->parity == cfg_new->parity &&
	    data->data_bits == cfg_new->data_bits && data->stop_bits == cfg_new->stop_bits &&
	    data->flow_ctrl == cfg_new->flow_ctrl) {
		return 0;
	}

	unsigned int key = irq_lock();

	usart_disable(cfg->reg);

	usart_parity_config(cfg->reg, parity_bits);
	usart_word_length_set(cfg->reg, word_length);
	usart_stop_bit_set(cfg->reg, stop_bits_hw);
	usart_baudrate_set(cfg->reg, cfg_new->baudrate);

	usart_receive_config(cfg->reg, USART_RECEIVE_ENABLE);
	usart_transmit_config(cfg->reg, USART_TRANSMIT_ENABLE);
	usart_enable(cfg->reg);

	irq_unlock(key);

	data->baud_rate = cfg_new->baudrate;
	data->parity = cfg_new->parity;
	data->data_bits = cfg_new->data_bits;
	data->stop_bits = cfg_new->stop_bits;
	data->flow_ctrl = cfg_new->flow_ctrl;

	return 0;
}

static int usart_gd32_config_get(const struct device *dev, struct uart_config *cfg_out)
{
	struct gd32_usart_data *const data = dev->data;

	if (cfg_out == NULL) {
		return -EINVAL;
	}

	if (!data->initialized) {
		return -ENODEV;
	}

	cfg_out->baudrate = data->baud_rate;
	cfg_out->parity = data->parity;
	cfg_out->stop_bits = data->stop_bits;
	cfg_out->data_bits = data->data_bits;
	cfg_out->flow_ctrl = data->flow_ctrl;

	return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
int usart_gd32_fifo_fill(const struct device *dev, const uint8_t *tx_data,
			 int len)
{
	const struct gd32_usart_config *const cfg = dev->config;
	int num_tx = 0U;

	while ((len - num_tx > 0) &&
	       usart_flag_get(cfg->reg, USART_FLAG_TBE)) {
		usart_data_transmit(cfg->reg, tx_data[num_tx++]);
	}

	return num_tx;
}

int usart_gd32_fifo_read(const struct device *dev, uint8_t *rx_data,
			 const int size)
{
	const struct gd32_usart_config *const cfg = dev->config;
	int num_rx = 0U;

	while ((size - num_rx > 0) &&
	       usart_flag_get(cfg->reg, USART_FLAG_RBNE)) {
		rx_data[num_rx++] = (uint8_t)usart_data_receive(cfg->reg);
	}

	return num_rx;
}

void usart_gd32_irq_tx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_TC);
}

void usart_gd32_irq_tx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_TC);
}

int usart_gd32_irq_tx_ready(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_TBE) &&
	       usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_TC);
}

int usart_gd32_irq_tx_complete(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_TC);
}

void usart_gd32_irq_rx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_RBNE);
}

void usart_gd32_irq_rx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_RBNE);
}

int usart_gd32_irq_rx_ready(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_RBNE);
}

void usart_gd32_irq_err_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_ERR);
	usart_interrupt_enable(cfg->reg, USART_INT_PERR);
}

void usart_gd32_irq_err_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_ERR);
	usart_interrupt_disable(cfg->reg, USART_INT_PERR);
}

int usart_gd32_irq_is_pending(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return ((usart_flag_get(cfg->reg, USART_FLAG_RBNE) &&
		 usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_RBNE)) ||
		(usart_flag_get(cfg->reg, USART_FLAG_TC) &&
		 usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_TC)));
}

void usart_gd32_irq_callback_set(const struct device *dev,
				 uart_irq_callback_user_data_t cb,
				 void *user_data)
{
	struct gd32_usart_data *const data = dev->data;

	data->user_cb = cb;
	data->user_data = user_data;

#if defined(CONFIG_UART_EXCLUSIVE_API_CALLBACKS) && GD32_USART_ASYNC
	data->async_cb = NULL;
	data->async_user_data = NULL;
#endif
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

#if GD32_USART_ASYNC

static void usart_gd32_dma_rx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_dma_receive_config(cfg->reg, USART_DENR_ENABLE);
}

static void usart_gd32_dma_rx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_dma_receive_config(cfg->reg, USART_DENR_DISABLE);
}

static void usart_gd32_dma_tx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_dma_transmit_config(cfg->reg, USART_DENT_ENABLE);
}

static void usart_gd32_dma_tx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_dma_transmit_config(cfg->reg, USART_DENT_DISABLE);
}

static void usart_gd32_dma_rx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
				       int status);

static void usart_gd32_async_callback(const struct device *dev, struct uart_event *evt)
{
	struct gd32_usart_data *data = dev->data;

	if (data->async_cb != NULL) {
		data->async_cb(dev, evt, data->async_user_data);
	}
}

static size_t usart_gd32_dma_tx_count(const struct device *dev)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct dma_status status;

	if (data->tx_dma_done) {
		return data->tx_len;
	}

	if (dma_get_status(cfg->dma_tx.dma_dev, cfg->dma_tx.dma_channel, &status) < 0 ||
	    status.pending_length > data->tx_len) {
		return 0;
	}

	return data->tx_len - status.pending_length;
}

static void usart_gd32_async_tx_complete(const struct device *dev, enum uart_event_type type,
					 size_t len)
{
	struct gd32_usart_data *data = dev->data;
	struct uart_event evt = {
		.type = type,
		.data.tx.buf = data->tx_buf,
		.data.tx.len = len,
	};

	data->tx_busy = false;
	data->tx_dma_done = false;
	data->tx_buf = NULL;
	data->tx_len = 0;
	usart_gd32_async_callback(dev, &evt);
}

static size_t usart_gd32_dma_rx_count(const struct device *dev)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct dma_status status;

	if (dma_get_status(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel, &status) < 0) {
		return data->rx_offset;
	}

	if (status.pending_length > data->rx_len) {
		return data->rx_offset;
	}

	return data->rx_len - status.pending_length;
}

static void usart_gd32_async_rx_rdy(const struct device *dev, size_t count)
{
	struct gd32_usart_data *data = dev->data;
	struct uart_event evt;

	if ((data->rx_buf == NULL) || (count <= data->rx_offset)) {
		return;
	}

	evt = (struct uart_event){
		.type = UART_RX_RDY,
		.data.rx.buf = data->rx_buf,
		.data.rx.offset = data->rx_offset,
		.data.rx.len = count - data->rx_offset,
	};

	data->rx_offset = count;
	usart_gd32_async_callback(dev, &evt);
}

static void usart_gd32_async_rx_buf_released(const struct device *dev, uint8_t *buf)
{
	struct uart_event evt = {
		.type = UART_RX_BUF_RELEASED,
		.data.rx_buf.buf = buf,
	};

	if (buf != NULL) {
		usart_gd32_async_callback(dev, &evt);
	}
}

static void usart_gd32_async_rx_buf_request(const struct device *dev)
{
	struct uart_event evt = {
		.type = UART_RX_BUF_REQUEST,
	};

	usart_gd32_async_callback(dev, &evt);
}

static void usart_gd32_async_rx_disabled(const struct device *dev)
{
	struct uart_event evt = {
		.type = UART_RX_DISABLED,
	};

	usart_gd32_async_callback(dev, &evt);
}

static void usart_gd32_async_rx_shutdown(const struct device *dev, enum uart_rx_stop_reason reason,
					 bool stopped)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	uint8_t *rx_buf = data->rx_buf;
	uint8_t *next_buf = data->rx_next_buf;
	size_t count = usart_gd32_dma_rx_count(dev);

	data->rx_stopping = true;
	(void)k_work_cancel_delayable(&data->rx_timeout_work);
	usart_interrupt_disable(cfg->reg, USART_INT_IDLE);
	usart_interrupt_disable(cfg->reg, USART_INT_ERR);
	usart_interrupt_disable(cfg->reg, USART_INT_PERR);
	usart_gd32_dma_rx_disable(dev);
	(void)dma_stop(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel);

	if (stopped) {
		struct uart_event evt = {
			.type = UART_RX_STOPPED,
			.data.rx_stop.reason = reason,
			.data.rx_stop.data.buf = rx_buf,
			.data.rx_stop.data.offset = data->rx_offset,
			.data.rx_stop.data.len =
				count > data->rx_offset ? count - data->rx_offset : 0,
		};

		usart_gd32_async_callback(dev, &evt);
	}

	usart_gd32_async_rx_rdy(dev, count);
	usart_gd32_async_rx_buf_released(dev, rx_buf);
	usart_gd32_async_rx_buf_released(dev, next_buf);

	data->rx_buf = NULL;
	data->rx_len = 0;
	data->rx_offset = 0;
	data->rx_idle_count = 0;
	data->rx_next_buf = NULL;
	data->rx_next_len = 0;
	data->rx_enabled = false;
	data->rx_stopping = false;
	usart_gd32_async_rx_disabled(dev);
}

static void usart_gd32_async_rx_stop(const struct device *dev, enum uart_rx_stop_reason reason)
{
	struct gd32_usart_data *data = dev->data;

	if (data->rx_enabled && !data->rx_stopping) {
		usart_gd32_async_rx_shutdown(dev, reason, true);
	}
}

static int usart_gd32_dma_rx_configure(const struct device *dev, uint8_t *buf, size_t len)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;

	memset(&data->dma_rx_blk, 0, sizeof(data->dma_rx_blk));
	data->dma_rx_blk.source_address = (uint32_t)&USART_DATA(cfg->reg);
	data->dma_rx_blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	data->dma_rx_blk.dest_address = (uint32_t)buf;
	data->dma_rx_blk.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	data->dma_rx_blk.block_size = len;

	memset(&data->dma_rx_cfg, 0, sizeof(data->dma_rx_cfg));
	data->dma_rx_cfg.dma_slot = cfg->dma_rx.slot;
	data->dma_rx_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	data->dma_rx_cfg.source_data_size = 1U;
	data->dma_rx_cfg.dest_data_size = 1U;
	data->dma_rx_cfg.source_burst_length = 1U;
	data->dma_rx_cfg.dest_burst_length = 1U;
	data->dma_rx_cfg.block_count = 1U;
	data->dma_rx_cfg.head_block = &data->dma_rx_blk;
	data->dma_rx_cfg.dma_callback = usart_gd32_dma_rx_callback;
	data->dma_rx_cfg.user_data = (void *)dev;
	data->dma_rx_cfg.channel_priority = GD32_DMA_CONFIG_PRIORITY(cfg->dma_rx.config);

	return dma_config(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel, &data->dma_rx_cfg);
}

static void usart_gd32_dma_rx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
				       int status)
{
	const struct device *dev = (const struct device *)arg;
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	uint8_t *completed_buf;
	uint8_t *next_buf;
	size_t completed_len;
	size_t completed_offset;
	size_t next_len;
	unsigned int key;
	int ret = 0;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	key = irq_lock();

	if (!data->rx_enabled || data->rx_stopping) {
		irq_unlock(key);
		return;
	}

	if (status < 0) {
		usart_gd32_async_rx_stop(dev, UART_ERROR_OVERRUN);
		irq_unlock(key);
		return;
	}

	(void)k_work_cancel_delayable(&data->rx_timeout_work);

	completed_buf = data->rx_buf;
	completed_len = data->rx_len;
	completed_offset = data->rx_offset;
	next_buf = data->rx_next_buf;
	next_len = data->rx_next_len;

	if (next_buf != NULL) {
		usart_gd32_dma_rx_disable(dev);
		ret = usart_gd32_dma_rx_configure(dev, next_buf, next_len);
		if (ret == 0) {
			ret = dma_start(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel);
		}
		if (ret == 0) {
			usart_gd32_dma_rx_enable(dev);
		}
	}

	if ((next_buf == NULL) || (ret < 0)) {
		data->rx_stopping = true;
		usart_interrupt_disable(cfg->reg, USART_INT_IDLE);
		usart_interrupt_disable(cfg->reg, USART_INT_ERR);
		usart_interrupt_disable(cfg->reg, USART_INT_PERR);
		usart_gd32_dma_rx_disable(dev);
		(void)dma_stop(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel);
	}

	if (ret < 0) {
		struct uart_event stopped_evt = {
			.type = UART_RX_STOPPED,
			.data.rx_stop.reason = UART_ERROR_OVERRUN,
			.data.rx_stop.data.buf = completed_buf,
			.data.rx_stop.data.offset = completed_offset,
			.data.rx_stop.data.len = completed_len - completed_offset,
		};

		usart_gd32_async_callback(dev, &stopped_evt);
	}

	if (completed_offset < completed_len) {
		struct uart_event rdy_evt = {
			.type = UART_RX_RDY,
			.data.rx.buf = completed_buf,
			.data.rx.offset = completed_offset,
			.data.rx.len = completed_len - completed_offset,
		};

		usart_gd32_async_callback(dev, &rdy_evt);
	}

	usart_gd32_async_rx_buf_released(dev, completed_buf);

	if ((next_buf != NULL) && (ret == 0)) {
		data->rx_buf = next_buf;
		data->rx_len = next_len;
		data->rx_offset = 0;
		data->rx_idle_count = 0;
		data->rx_next_buf = NULL;
		data->rx_next_len = 0;

		if (data->rx_enabled && !data->rx_stopping) {
			usart_gd32_async_rx_buf_request(dev);
		}
		irq_unlock(key);
		return;
	}

	usart_gd32_async_rx_buf_released(dev, next_buf);

	data->rx_buf = NULL;
	data->rx_len = 0;
	data->rx_offset = 0;
	data->rx_idle_count = 0;
	data->rx_next_buf = NULL;
	data->rx_next_len = 0;
	data->rx_enabled = false;
	data->rx_stopping = false;
	usart_gd32_async_rx_disabled(dev);
	irq_unlock(key);
}

static void usart_gd32_dma_tx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
				       int status)
{
	const struct device *dev = (const struct device *)arg;
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	size_t count;
	unsigned int key;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	key = irq_lock();
	if (!data->tx_busy) {
		irq_unlock(key);
		return;
	}

	count = usart_gd32_dma_tx_count(dev);
	usart_gd32_dma_tx_disable(dev);
	(void)dma_stop(cfg->dma_tx.dma_dev, cfg->dma_tx.dma_channel);

	if (status < 0) {
		usart_gd32_async_tx_complete(dev, UART_TX_ABORTED, count);
	} else {
		data->tx_dma_done = true;
		usart_interrupt_enable(cfg->reg, USART_INT_TC);
	}

	irq_unlock(key);
}

static int usart_gd32_callback_set(const struct device *dev, uart_callback_t callback,
				   void *user_data)
{
	struct gd32_usart_data *data = dev->data;
	unsigned int key = irq_lock();

	data->async_cb = callback;
	data->async_user_data = user_data;

#if defined(CONFIG_UART_EXCLUSIVE_API_CALLBACKS) && defined(CONFIG_UART_INTERRUPT_DRIVEN)
	data->user_cb = NULL;
	data->user_data = NULL;
#endif

	irq_unlock(key);
	return 0;
}

static int usart_gd32_tx(const struct device *dev, const uint8_t *buf, size_t len, int32_t timeout)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	unsigned int key;
	int ret;

	ARG_UNUSED(timeout);

	if (!data->async_cb) {
		return -ENOTSUP;
	}

	if ((buf == NULL) || (len == 0U) || (len > UINT16_MAX)) {
		return -EINVAL;
	}

	if (cfg->dma_tx.dma_dev == NULL) {
		return -ENOTSUP;
	}

	if (!device_is_ready(cfg->dma_tx.dma_dev)) {
		return -ENODEV;
	}

	key = irq_lock();
	if (data->tx_busy) {
		irq_unlock(key);
		return -EBUSY;
	}

	data->tx_buf = buf;
	data->tx_len = len;
	data->tx_busy = true;
	data->tx_dma_done = false;

	memset(&data->dma_tx_blk, 0, sizeof(data->dma_tx_blk));
	data->dma_tx_blk.source_address = (uint32_t)buf;
	data->dma_tx_blk.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	data->dma_tx_blk.dest_address = (uint32_t)&USART_DATA(cfg->reg);
	data->dma_tx_blk.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	data->dma_tx_blk.block_size = len;

	memset(&data->dma_tx_cfg, 0, sizeof(data->dma_tx_cfg));
	data->dma_tx_cfg.dma_slot = cfg->dma_tx.slot;
	data->dma_tx_cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	data->dma_tx_cfg.source_data_size = 1U;
	data->dma_tx_cfg.dest_data_size = 1U;
	data->dma_tx_cfg.source_burst_length = 1U;
	data->dma_tx_cfg.dest_burst_length = 1U;
	data->dma_tx_cfg.block_count = 1U;
	data->dma_tx_cfg.head_block = &data->dma_tx_blk;
	data->dma_tx_cfg.dma_callback = usart_gd32_dma_tx_callback;
	data->dma_tx_cfg.user_data = (void *)dev;
	data->dma_tx_cfg.channel_priority = GD32_DMA_CONFIG_PRIORITY(cfg->dma_tx.config);

	ret = dma_config(cfg->dma_tx.dma_dev, cfg->dma_tx.dma_channel, &data->dma_tx_cfg);
	if (ret < 0) {
		data->tx_busy = false;
		data->tx_buf = NULL;
		data->tx_len = 0;
		irq_unlock(key);
		return ret;
	}

	usart_interrupt_disable(cfg->reg, USART_INT_TC);
	usart_interrupt_flag_clear(cfg->reg, USART_INT_FLAG_TC);
	usart_gd32_dma_tx_enable(dev);

	ret = dma_start(cfg->dma_tx.dma_dev, cfg->dma_tx.dma_channel);
	if (ret < 0) {
		usart_gd32_dma_tx_disable(dev);
		data->tx_busy = false;
		data->tx_buf = NULL;
		data->tx_len = 0;
		irq_unlock(key);
		return ret;
	}

	irq_unlock(key);
	return 0;
}

static int usart_gd32_tx_abort(const struct device *dev)
{
	struct gd32_usart_data *data = dev->data;
	const struct gd32_usart_config *cfg = dev->config;
	size_t count;
	unsigned int key;

	key = irq_lock();
	if (!data->tx_busy) {
		irq_unlock(key);
		return -EFAULT;
	}

	count = usart_gd32_dma_tx_count(dev);
	usart_interrupt_disable(cfg->reg, USART_INT_TC);
	usart_gd32_dma_tx_disable(dev);
	(void)dma_stop(cfg->dma_tx.dma_dev, cfg->dma_tx.dma_channel);
	usart_gd32_async_tx_complete(dev, UART_TX_ABORTED, count);
	irq_unlock(key);

	return 0;
}

static int usart_gd32_rx_enable(const struct device *dev, uint8_t *buf, size_t len, int32_t timeout)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	unsigned int key;
	int ret;

	if (!data->async_cb) {
		return -ENOTSUP;
	}

	if (cfg->dma_rx.dma_dev == NULL) {
		return -ENOTSUP;
	}

	if (!device_is_ready(cfg->dma_rx.dma_dev)) {
		return -ENODEV;
	}

	if ((buf == NULL) || (len == 0U) || (len > UINT16_MAX) ||
	    ((timeout < 0) && (timeout != SYS_FOREVER_US))) {
		return -EINVAL;
	}

	key = irq_lock();

	if (data->rx_enabled || data->rx_stopping) {
		irq_unlock(key);
		return -EBUSY;
	}

	usart_gd32_dma_rx_disable(dev);
	/* Clear stale RX/IDLE/error state before arming DMA for a new buffer. */
	(void)USART_STAT(cfg->reg);
	(void)USART_DATA(cfg->reg);

	data->rx_buf = buf;
	data->rx_len = len;
	data->rx_offset = 0;
	data->rx_idle_count = 0;
	data->rx_next_buf = NULL;
	data->rx_next_len = 0;
	data->rx_timeout = timeout;
	data->rx_enabled = true;
	data->rx_stopping = false;

	ret = usart_gd32_dma_rx_configure(dev, buf, len);
	if (ret < 0) {
		data->rx_enabled = false;
		data->rx_buf = NULL;
		data->rx_len = 0;
		irq_unlock(key);
		return ret;
	}

	ret = dma_start(cfg->dma_rx.dma_dev, cfg->dma_rx.dma_channel);
	if (ret < 0) {
		data->rx_enabled = false;
		data->rx_buf = NULL;
		data->rx_len = 0;
		irq_unlock(key);
		return ret;
	}

	if (timeout != SYS_FOREVER_US) {
		usart_interrupt_enable(cfg->reg, USART_INT_IDLE);
	}
	usart_interrupt_enable(cfg->reg, USART_INT_ERR);
	usart_interrupt_enable(cfg->reg, USART_INT_PERR);

	usart_gd32_dma_rx_enable(dev);
	usart_gd32_async_rx_buf_request(dev);
	irq_unlock(key);
	return 0;
}

static int usart_gd32_rx_disable(const struct device *dev)
{
	struct gd32_usart_data *data = dev->data;
	unsigned int key;

	key = irq_lock();

	if (!data->rx_enabled || data->rx_stopping) {
		irq_unlock(key);
		return -EFAULT;
	}

	usart_gd32_async_rx_shutdown(dev, 0, false);
	irq_unlock(key);

	return 0;
}

static int usart_gd32_rx_buf_rsp(const struct device *dev, uint8_t *buf, size_t len)
{
	struct gd32_usart_data *data = dev->data;
	unsigned int key;
	int ret = 0;

	if ((buf == NULL) || (len == 0U) || (len > UINT16_MAX)) {
		return -EINVAL;
	}

	key = irq_lock();

	if (!data->rx_enabled || data->rx_stopping) {
		ret = -EACCES;
	} else if (data->rx_next_buf != NULL) {
		ret = -EBUSY;
	} else {
		data->rx_next_buf = buf;
		data->rx_next_len = len;
	}

	irq_unlock(key);
	return ret;
}

static void usart_gd32_async_rx_timeout(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct gd32_usart_data *data = CONTAINER_OF(dwork, struct gd32_usart_data, rx_timeout_work);
	const struct device *dev = data->dev;
	size_t count;
	unsigned int key = irq_lock();

	if (data->rx_enabled && !data->rx_stopping) {
		count = usart_gd32_dma_rx_count(dev);
		if (count == data->rx_idle_count) {
			usart_gd32_async_rx_rdy(dev, count);
		}
	}

	irq_unlock(key);
}

#endif /* GD32_USART_ASYNC */

static DEVICE_API(uart, usart_gd32_driver_api) = {
	.poll_in = usart_gd32_poll_in,
	.poll_out = usart_gd32_poll_out,
	.err_check = usart_gd32_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = usart_gd32_configure,
	.config_get = usart_gd32_config_get,
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = usart_gd32_fifo_fill,
	.fifo_read = usart_gd32_fifo_read,
	.irq_tx_enable = usart_gd32_irq_tx_enable,
	.irq_tx_disable = usart_gd32_irq_tx_disable,
	.irq_tx_ready = usart_gd32_irq_tx_ready,
	.irq_tx_complete = usart_gd32_irq_tx_complete,
	.irq_rx_enable = usart_gd32_irq_rx_enable,
	.irq_rx_disable = usart_gd32_irq_rx_disable,
	.irq_rx_ready = usart_gd32_irq_rx_ready,
	.irq_err_enable = usart_gd32_irq_err_enable,
	.irq_err_disable = usart_gd32_irq_err_disable,
	.irq_is_pending = usart_gd32_irq_is_pending,
	.irq_callback_set = usart_gd32_irq_callback_set,
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
#if GD32_USART_ASYNC
	.callback_set = usart_gd32_callback_set,
	.tx = usart_gd32_tx,
	.tx_abort = usart_gd32_tx_abort,
	.rx_enable = usart_gd32_rx_enable,
	.rx_buf_rsp = usart_gd32_rx_buf_rsp,
	.rx_disable = usart_gd32_rx_disable,
#endif /* GD32_USART_ASYNC */
};

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || GD32_USART_ASYNC
#define GD32_USART_IRQ_HANDLER(n)                                                                  \
	static void usart_gd32_config_func_##n(const struct device *dev)                           \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), usart_gd32_isr,             \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}
#define GD32_USART_IRQ_HANDLER_FUNC_INIT(n) .irq_config_func = usart_gd32_config_func_##n,
#else
#define GD32_USART_IRQ_HANDLER(n)
#define GD32_USART_IRQ_HANDLER_FUNC_INIT(n)
#endif

#if GD32_USART_ASYNC
#define GD32_USART_DMAS_INIT(n)                                                                    \
	COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),				\
		(.dma_rx = {							\
			.dma_dev = DEVICE_DT_GET_OR_NULL(			\
				DT_INST_DMAS_CTLR_BY_NAME(n, rx)),		\
			.dma_channel = DT_INST_DMAS_CELL_BY_NAME(n, rx, channel),\
			COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(gd_gd32_dma_v1),	\
				(.slot = DT_INST_DMAS_CELL_BY_NAME(n, rx, slot),), ())\
			.config = DT_INST_DMAS_CELL_BY_NAME(n, rx, config),	\
		},), ())                                            \
	COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),				\
		(.dma_tx = {							\
			.dma_dev = DEVICE_DT_GET_OR_NULL(			\
				DT_INST_DMAS_CTLR_BY_NAME(n, tx)),		\
			.dma_channel = DT_INST_DMAS_CELL_BY_NAME(n, tx, channel),\
			COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(gd_gd32_dma_v1),	\
				(.slot = DT_INST_DMAS_CELL_BY_NAME(n, tx, slot),), ())\
			.config = DT_INST_DMAS_CELL_BY_NAME(n, tx, config),	\
		},), ())
#else
#define GD32_USART_DMAS_INIT(n)
#endif /* GD32_USART_ASYNC */

#define GD32_USART_INIT(n)                                                                         \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	GD32_USART_IRQ_HANDLER(n)                                                                  \
	static struct gd32_usart_data usart_gd32_data_##n = {                                      \
		.baud_rate = DT_INST_PROP(n, current_speed),                                       \
	};                                                                                         \
	static const struct gd32_usart_config usart_gd32_config_##n = {                            \
		.reg = DT_INST_REG_ADDR(n),                                                        \
		.clkid = DT_INST_CLOCKS_CELL(n, id),                                               \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.parity = DT_INST_ENUM_IDX(n, parity),                                             \
		GD32_USART_IRQ_HANDLER_FUNC_INIT(n) GD32_USART_DMAS_INIT(n)};                      \
	DEVICE_DT_INST_DEFINE(n, usart_gd32_init, NULL, &usart_gd32_data_##n,                      \
			      &usart_gd32_config_##n, PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY,   \
			      &usart_gd32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GD32_USART_INIT)
