/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_timer_pulse_io

#include <soc.h>
#include <stm32_ll_tim.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pulse_io.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pulse_io_stm32_timer, CONFIG_PULSE_IO_LOG_LEVEL);

struct stm32_pulse_config {
	TIM_TypeDef *timer;
	volatile uint32_t *compare;
	uint32_t channel;
	const struct stm32_pclken *clocks;
	const struct pinctrl_dev_config *pinctrl;
	struct reset_dt_spec reset;
	const struct device *dma;
	uint32_t dma_channel;
	uint32_t dma_request;
	uint32_t dma_priority;
	uint16_t *buffer;
	size_t capacity;
};

struct stm32_pulse_data {
	struct k_mutex mutex;
	struct k_spinlock lock;
	struct k_sem done;
	uint32_t clock_hz;
	uint32_t period;
	int result;
	bool reserved;
	bool configured;
	bool active;
};

static bool valid_handle(const struct device *dev, struct pulse_io_channel *chan)
{
	return chan == (struct pulse_io_channel *)dev->data;
}

static void timer_idle(const struct stm32_pulse_config *cfg)
{
	LL_TIM_DisableDMAReq_UPDATE(cfg->timer);
	LL_TIM_OC_SetMode(cfg->timer, cfg->channel, LL_TIM_OCMODE_FORCED_INACTIVE);
	LL_TIM_DisableCounter(cfg->timer);
}

static void transfer_done(const struct device *dma, void *user, uint32_t channel, int status)
{
	const struct device *dev = user;
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;
	k_spinlock_key_t key;

	ARG_UNUSED(dma);
	ARG_UNUSED(channel);
	if (status > 0) {
		return;
	}
	key = k_spin_lock(&data->lock);
	if (data->active) {
		timer_idle(cfg);
		data->result = status;
		data->active = false;
		k_sem_give(&data->done);
	}
	k_spin_unlock(&data->lock, key);
}

static int stm32_pulse_caps(const struct device *dev, struct pulse_io_caps *caps)
{
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;

	*caps = (struct pulse_io_caps){
		.modes = PULSE_IO_MODE_CELL,
		.supports_tx = true,
		.tx_channel_mask = BIT(0),
		.num_channels = 1U,
		.cell_duty_bits = 16U,
		.min_tick_ns = DIV_ROUND_UP(NSEC_PER_SEC, data->clock_hz),
		.max_tick_ns = (uint64_t)NSEC_PER_SEC * 65536U / data->clock_hz,
		.max_duration_ticks = UINT16_MAX,
		.tx_min_chunk_symbols = 1U,
		.tx_max_streaming = cfg->capacity,
	};
	return 0;
}

static int stm32_pulse_get(const struct device *dev, uint8_t index, struct pulse_io_channel **chan)
{
	struct stm32_pulse_data *data = dev->data;
	k_spinlock_key_t key;

	if (index != 0U) {
		return -ENODEV;
	}
	key = k_spin_lock(&data->lock);
	if (data->reserved) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	data->reserved = true;
	*chan = (struct pulse_io_channel *)data;
	k_spin_unlock(&data->lock, key);
	return 0;
}

/* All operations except stop hold the mutex until the hardware is quiescent. */
static int lock_channel(const struct device *dev, struct pulse_io_channel *chan)
{
	struct stm32_pulse_data *data = dev->data;
	k_spinlock_key_t key;
	bool reserved;

	if (!valid_handle(dev, chan)) {
		return -EINVAL;
	}
	if (k_mutex_lock(&data->mutex, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	key = k_spin_lock(&data->lock);
	reserved = data->reserved;
	k_spin_unlock(&data->lock, key);
	if (!reserved) {
		k_mutex_unlock(&data->mutex);
		return -EINVAL;
	}
	return 0;
}

static int stm32_pulse_release(const struct device *dev, struct pulse_io_channel *chan)
{
	struct stm32_pulse_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = lock_channel(dev, chan);

	if (ret != 0) {
		return ret;
	}
	key = k_spin_lock(&data->lock);
	data->configured = false;
	data->reserved = false;
	k_spin_unlock(&data->lock, key);
	k_mutex_unlock(&data->mutex);
	return 0;
}

static int stm32_pulse_configure(const struct device *dev, struct pulse_io_channel *chan,
				 const struct pulse_io_config *config)
{
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;
	uint32_t divisor;
	int ret = lock_channel(dev, chan);

	if (ret != 0) {
		return ret;
	}
	if ((config->mode != PULSE_IO_MODE_CELL) || (config->dir != PULSE_IO_DIR_TX) ||
	    config->idle_high || config->carrier_en) {
		ret = -ENOTSUP;
		goto out;
	}
	if ((config->resolution_hz == 0U) || (config->cell_period_ticks == 0U) ||
	    (config->cell_period_ticks > UINT16_MAX)) {
		ret = -EINVAL;
		goto out;
	}
	divisor = CLAMP(DIV_ROUND_CLOSEST((uint64_t)data->clock_hz, config->resolution_hz), 1U,
			65536U);
	timer_idle(cfg);
	LL_TIM_SetPrescaler(cfg->timer, divisor - 1U);
	LL_TIM_SetAutoReload(cfg->timer, config->cell_period_ticks - 1U);
	LL_TIM_GenerateEvent_UPDATE(cfg->timer);
	data->period = config->cell_period_ticks;
	data->configured = true;
out:
	k_mutex_unlock(&data->mutex);
	return ret;
}

static int stm32_pulse_transmit(const struct device *dev, struct pulse_io_channel *chan,
				const struct pulse_io_tx_req *req, k_timeout_t timeout)
{
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;
	struct dma_block_config block = {
		.source_address = (uintptr_t)&cfg->buffer[1],
		.dest_address = (uintptr_t)cfg->compare,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	struct dma_config dma_cfg = {
		.dma_slot = cfg->dma_request,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = sizeof(uint16_t),
		.dest_data_size = sizeof(uint16_t),
		.source_burst_length = sizeof(uint16_t),
		.dest_burst_length = sizeof(uint16_t),
		.channel_priority = cfg->dma_priority,
		.dma_callback = transfer_done,
		.user_data = (void *)dev,
		.block_count = 1U,
		.head_block = &block,
	};
	k_spinlock_key_t key;
	int stop_ret;
	int ret = lock_channel(dev, chan);

	if (ret != 0) {
		return ret;
	}
	if (!data->configured) {
		ret = -EINVAL;
		goto out;
	}
	if (req->loop_count != 0U) {
		ret = -ENOTSUP;
		goto out;
	}
	if ((req->count > cfg->capacity) || ((req->count > 0U) && (req->cells == NULL))) {
		ret = -EINVAL;
		goto out;
	}
	if (req->count == 0U) {
		goto out;
	}
	for (size_t i = 0U; i < req->count; i++) {
		if ((req->cells[i].duty > data->period) || (req->cells[i].rsvd != 0U)) {
			ret = -EINVAL;
			goto out;
		}
		cfg->buffer[i] = req->cells[i].duty;
	}
	/* The final DMA write occurs only after the first idle cell is active. */
	cfg->buffer[req->count] = 0U;
	cfg->buffer[req->count + 1U] = 0U;
	if (IS_ENABLED(CONFIG_DCACHE)) {
		ret = sys_cache_data_flush_range(cfg->buffer, (req->count + 2U) * sizeof(uint16_t));
		if (ret != 0) {
			goto out;
		}
	}
	block.block_size = (req->count + 1U) * sizeof(uint16_t);
	ret = dma_config(cfg->dma, cfg->dma_channel, &dma_cfg);
	if (ret != 0) {
		goto out;
	}
	k_sem_reset(&data->done);
	key = k_spin_lock(&data->lock);
	timer_idle(cfg);
	/* Start with a full idle period; update transfers the first duty into use. */
	*cfg->compare = 0U;
	LL_TIM_GenerateEvent_UPDATE(cfg->timer);
	LL_TIM_ClearFlag_UPDATE(cfg->timer);
	*cfg->compare = cfg->buffer[0];
	LL_TIM_SetCounter(cfg->timer, 0U);
	ret = dma_start(cfg->dma, cfg->dma_channel);
	if (ret == 0) {
		data->result = 0;
		data->active = true;
		LL_TIM_EnableDMAReq_UPDATE(cfg->timer);
		LL_TIM_OC_SetMode(cfg->timer, cfg->channel, LL_TIM_OCMODE_PWM1);
		LL_TIM_EnableCounter(cfg->timer);
	}
	k_spin_unlock(&data->lock, key);
	if (ret == 0) {
		ret = k_sem_take(&data->done, timeout);
		key = k_spin_lock(&data->lock);
		timer_idle(cfg);
		data->active = false;
		ret = ret == 0 ? data->result : -ETIMEDOUT;
		k_spin_unlock(&data->lock, key);
	}
	stop_ret = dma_stop(cfg->dma, cfg->dma_channel);
	if (stop_ret != 0) {
		LOG_ERR("DMA stop failed: %d", stop_ret);
		if (ret == 0) {
			ret = stop_ret;
		}
	}
out:
	k_mutex_unlock(&data->mutex);
	return ret;
}

static int stm32_pulse_receive(const struct device *dev, struct pulse_io_channel *chan,
			       const struct pulse_io_rx_req *req, size_t *count,
			       k_timeout_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(req);
	ARG_UNUSED(count);
	ARG_UNUSED(timeout);
	return -ENOTSUP;
}

static int stm32_pulse_stop(const struct device *dev, struct pulse_io_channel *chan)
{
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;
	k_spinlock_key_t key;

	if (!valid_handle(dev, chan)) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	if (!data->reserved) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	if (data->active) {
		timer_idle(cfg);
		data->result = -ECANCELED;
		data->active = false;
		k_sem_give(&data->done);
	}
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int stm32_pulse_init(const struct device *dev)
{
	const struct stm32_pulse_config *cfg = dev->config;
	struct stm32_pulse_data *data = dev->data;
	const struct device *clock = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	int ret;

	if (!device_is_ready(cfg->dma) || !device_is_ready(clock)) {
		return -ENODEV;
	}
	k_mutex_init(&data->mutex);
	k_sem_init(&data->done, 0, 1);
	ret = clock_control_on(clock, (clock_control_subsys_t)&cfg->clocks[0]);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_configure(clock, (clock_control_subsys_t)&cfg->clocks[1], NULL);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_get_rate(clock, (clock_control_subsys_t)&cfg->clocks[1],
				     &data->clock_hz);
	if (ret != 0) {
		return ret;
	}
	if (data->clock_hz == 0U) {
		return -EINVAL;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}
	LL_TIM_EnableARRPreload(cfg->timer);
	LL_TIM_OC_EnablePreload(cfg->timer, cfg->channel);
	LL_TIM_OC_SetPolarity(cfg->timer, cfg->channel, LL_TIM_OCPOLARITY_HIGH);
	LL_TIM_CC_EnableChannel(cfg->timer, cfg->channel);
	if (IS_TIM_BREAK_INSTANCE(cfg->timer)) {
		LL_TIM_EnableAllOutputs(cfg->timer);
	}
	timer_idle(cfg);
	return pinctrl_apply_state(cfg->pinctrl, PINCTRL_STATE_DEFAULT);
}

static DEVICE_API(pulse_io, stm32_pulse_api) = {
	.get_capabilities = stm32_pulse_caps,
	.channel_get = stm32_pulse_get,
	.channel_release = stm32_pulse_release,
	.channel_configure = stm32_pulse_configure,
	.transmit_sync = stm32_pulse_transmit,
	.receive_sync = stm32_pulse_receive,
	.stop = stm32_pulse_stop,
};

#define PULSE_TIMER(inst)      DT_INST_PARENT(inst)
#define PULSE_CHANNEL(inst)    DT_INST_PROP(inst, st_channel)
#define PULSE_COMPARE(inst)    CONCAT(CCR, PULSE_CHANNEL(inst))
#define PULSE_LL_CHANNEL(inst) CONCAT(LL_TIM_CHANNEL_CH, PULSE_CHANNEL(inst))

#define STM32_PULSE_DEFINE(inst)                                                                   \
	BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(PULSE_TIMER(inst)) == 1,                             \
		     "Pulse IO requires exclusive use of the parent timer");                       \
	BUILD_ASSERT(DT_NUM_CLOCKS(PULSE_TIMER(inst)) >= 2, "Timer clock source required");        \
	BUILD_ASSERT(DT_INST_PROP(inst, zephyr_max_cells) > 0 &&                                   \
			     DT_INST_PROP(inst, zephyr_max_cells) <= 32766,                        \
		     "Invalid DMA buffer size");                                                   \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
	static const struct stm32_pclken pulse_clocks_##inst[] =                                   \
		STM32_DT_CLOCKS(PULSE_TIMER(inst));                                                \
	static uint16_t pulse_buffer_##inst[DT_INST_PROP(inst, zephyr_max_cells) + 2]              \
		__aligned(32);                                                                     \
	static struct stm32_pulse_data pulse_data_##inst;                                          \
	static const struct stm32_pulse_config pulse_config_##inst = {                             \
		.timer = (TIM_TypeDef *)DT_REG_ADDR(PULSE_TIMER(inst)),                            \
		.compare = &((TIM_TypeDef *)DT_REG_ADDR(PULSE_TIMER(inst)))->PULSE_COMPARE(inst),  \
		.channel = PULSE_LL_CHANNEL(inst),                                                 \
		.clocks = pulse_clocks_##inst,                                                     \
		.pinctrl = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                   \
		.reset = RESET_DT_SPEC_GET(PULSE_TIMER(inst)),                                     \
		.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, up)),                         \
		.dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, up, channel),                       \
		.dma_request = DT_INST_DMAS_CELL_BY_NAME(inst, up, slot),                          \
		.dma_priority = STM32_DMA_CONFIG_PRIORITY(STM32_DMA_CHANNEL_CONFIG(inst, up)),     \
		.buffer = pulse_buffer_##inst,                                                     \
		.capacity = DT_INST_PROP(inst, zephyr_max_cells),                                  \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, stm32_pulse_init, NULL, &pulse_data_##inst,                    \
			      &pulse_config_##inst, POST_KERNEL, CONFIG_PULSE_IO_INIT_PRIORITY,    \
			      &stm32_pulse_api);

DT_INST_FOREACH_STATUS_OKAY(STM32_PULSE_DEFINE)
