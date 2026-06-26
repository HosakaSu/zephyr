/*
 * Copyright (c) 2026 Liu Changjie <liucj1228@outlook.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT wch_ch395

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/phy.h>

LOG_MODULE_REGISTER(eth_ch395, CONFIG_ETHERNET_LOG_LEVEL);

#define CH395_CMD_GET_IC_VER			0x01
#define CH395_CMD_RESET_ALL			0x05
#define CH395_CMD_CHECK_EXIST			0x06
#define CH395_CMD_GET_GLOB_INT_STATUS_ALL	0x19
#define CH395_CMD_SET_MAC_ADDR			0x21
#define CH395_CMD_SET_MAC_FILT			0x25
#define CH395_CMD_GET_PHY_STATUS		0x26
#define CH395_CMD_INIT_CH395			0x27
#define CH395_CMD_GET_CMD_STATUS		0x2c
#define CH395_CMD_CLEAR_RECV_BUF_SN		0x2e
#define CH395_CMD_GET_INT_STATUS_SN		0x30
#define CH395_CMD_SET_PROTO_TYPE_SN		0x34
#define CH395_CMD_OPEN_SOCKET_SN		0x35
#define CH395_CMD_WRITE_SEND_BUF_SN		0x39
#define CH395_CMD_GET_RECV_LEN_SN		0x3b
#define CH395_CMD_READ_RECV_BUF_SN		0x3c
#define CH395_CMD_CLOSE_SOCKET_SN		0x3d

#define CH395_CMD_STATUS_SUCCESS	0x00
#define CH395_CMD_STATUS_BUSY		0x10

#define CH395_CHECK_EXIST_TEST		0x65
#define CH395_CHECK_EXIST_EXPECTED	0x9a
#define CH395_SPI_DUMMY			0x00

#define CH395_RESET_ASSERT_MS		10
#define CH395_RESET_READY_MS		500
#define CH395_CMD_POLL_INTERVAL_MS	20
#define CH395_CMD_TIMEOUT_MS		5000
#define CH395_INIT_FALLBACK_WAIT_MS	500
#define CH395_SOCKET_FALLBACK_WAIT_MS	100
#define CH395_INTERBYTE_DELAY_US	1
#define CH395_CMD_STATUS_DELAY_US	2
#define CH395_PHY_LINK_POLL_INTERVAL_MS	200

#define CH395_SOCKET_MACRAW		0
#define CH395_PROTO_TYPE_MACRAW		1

#define CH395_PHY_DISCONN		BIT(0)
#define CH395_PHY_10M_FULL		BIT(1)
#define CH395_PHY_10M_HALF		BIT(2)
#define CH395_PHY_100M_FULL		BIT(3)
#define CH395_PHY_100M_HALF		BIT(4)

struct ch395_config {
	struct net_eth_mac_config mac_cfg;
	struct spi_dt_spec spi;
	struct gpio_dt_spec cs_gpio;
	struct gpio_dt_spec int_gpio;
	struct gpio_dt_spec reset_gpio;
	const struct device *phy_dev;
};

struct ch395_runtime {
	struct k_mutex cmd_lock;
	struct net_if *iface;
	struct k_thread thread;
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_ETH_CH395_RX_THREAD_STACK_SIZE);
	struct phy_link_state state;
	bool link_reported;
	uint8_t mac_addr[NET_ETH_ADDR_LEN];
};

static int ch395_spi_byte(const struct device *dev, uint8_t tx, uint8_t *rx)
{
	const struct ch395_config *config = dev->config;
	struct spi_config spi_cfg = config->spi.config;
	uint8_t tmp;
	struct spi_buf tx_buf = {
		.buf = &tx,
		.len = sizeof(tx),
	};
	struct spi_buf rx_buf = {
		.buf = rx == NULL ? &tmp : rx,
		.len = sizeof(tmp),
	};
	const struct spi_buf_set tx_set = {
		.buffers = &tx_buf,
		.count = 1,
	};
	const struct spi_buf_set rx_set = {
		.buffers = &rx_buf,
		.count = 1,
	};
	int ret;

	spi_cfg.cs.gpio.port = NULL;
	spi_cfg.cs.cs_is_gpio = false;

	ret = spi_transceive(config->spi.bus, &spi_cfg, &tx_set, &rx_set);
	if (ret < 0) {
		return ret;
	}

	k_busy_wait(CH395_INTERBYTE_DELAY_US);

	return 0;
}

static int ch395_cs_set(const struct device *dev, bool active)
{
	const struct ch395_config *config = dev->config;

	return gpio_pin_set_dt(&config->cs_gpio, active ? 1 : 0);
}

static void ch395_cmd_end(const struct device *dev)
{
	(void)ch395_cs_set(dev, false);
}

static int ch395_cmd_begin(const struct device *dev, uint8_t cmd)
{
	int ret;

	ch395_cmd_end(dev);
	k_busy_wait(CH395_INTERBYTE_DELAY_US);

	ret = ch395_cs_set(dev, true);
	if (ret < 0) {
		return ret;
	}

	k_busy_wait(CH395_INTERBYTE_DELAY_US);

	return ch395_spi_byte(dev, cmd, NULL);
}

static int ch395_write_bytes_locked(const struct device *dev, const uint8_t *data,
				    size_t len)
{
	int ret;

	for (size_t i = 0; i < len; i++) {
		ret = ch395_spi_byte(dev, data[i], NULL);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int ch395_read_bytes_locked(const struct device *dev, uint8_t *data,
				   size_t len)
{
	int ret;

	for (size_t i = 0; i < len; i++) {
		ret = ch395_spi_byte(dev, CH395_SPI_DUMMY, &data[i]);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int ch395_write_le16_locked(const struct device *dev, uint16_t val)
{
	uint8_t data[2] = {
		val & 0xff,
		val >> 8,
	};

	return ch395_write_bytes_locked(dev, data, sizeof(data));
}

static int ch395_write_le32_locked(const struct device *dev, uint32_t val)
{
	uint8_t data[4] = {
		val & 0xff,
		(val >> 8) & 0xff,
		(val >> 16) & 0xff,
		val >> 24,
	};

	return ch395_write_bytes_locked(dev, data, sizeof(data));
}

static int ch395_cmd_write_locked(const struct device *dev, uint8_t cmd,
				  const uint8_t *req, size_t req_len)
{
	int ret;

	ret = ch395_cmd_begin(dev, cmd);
	if (ret == 0 && req_len > 0) {
		ret = ch395_write_bytes_locked(dev, req, req_len);
	}

	ch395_cmd_end(dev);

	return ret;
}

static int ch395_cmd_read_locked(const struct device *dev, uint8_t cmd,
				 const uint8_t *req, size_t req_len,
				 uint8_t *resp, size_t resp_len)
{
	int ret;

	ret = ch395_cmd_begin(dev, cmd);
	if (ret == 0 && req_len > 0) {
		ret = ch395_write_bytes_locked(dev, req, req_len);
	}
	if (ret == 0 && resp_len > 0) {
		ret = ch395_read_bytes_locked(dev, resp, resp_len);
	}

	ch395_cmd_end(dev);

	return ret;
}

static int ch395_cmd_write(const struct device *dev, uint8_t cmd,
			   const uint8_t *req, size_t req_len)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);
	ret = ch395_cmd_write_locked(dev, cmd, req, req_len);
	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int ch395_cmd_read(const struct device *dev, uint8_t cmd,
			  const uint8_t *req, size_t req_len,
			  uint8_t *resp, size_t resp_len)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);
	ret = ch395_cmd_read_locked(dev, cmd, req, req_len, resp, resp_len);
	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int ch395_read_reg8(const struct device *dev, uint8_t cmd, uint8_t *val)
{
	return ch395_cmd_read(dev, cmd, NULL, 0, val, sizeof(*val));
}

static int ch395_check_exist(const struct device *dev, uint8_t test, uint8_t *val)
{
	return ch395_cmd_read(dev, CH395_CMD_CHECK_EXIST, &test, sizeof(test), val, sizeof(*val));
}

static int ch395_cmd_status_locked(const struct device *dev, uint8_t *status)
{
	int ret;

	ret = ch395_cmd_begin(dev, CH395_CMD_GET_CMD_STATUS);
	if (ret == 0) {
		/* CH395Q at high SPI speeds can echo the command byte back
		 * if read immediately. Add a short settle delay.
		 */
		k_busy_wait(CH395_CMD_STATUS_DELAY_US);
		ret = ch395_read_bytes_locked(dev, status, sizeof(*status));
	}
	if (ret == 0 && *status == CH395_CMD_GET_CMD_STATUS) {
		/* Command echo detected: read again to get the real status. */
		k_busy_wait(CH395_CMD_STATUS_DELAY_US);
		ret = ch395_read_bytes_locked(dev, status, sizeof(*status));
	}

	ch395_cmd_end(dev);

	return ret;
}

static int ch395_cmd_wait_status_locked(const struct device *dev, int32_t timeout_ms,
					uint8_t *status)
{
	int64_t end = k_uptime_get() + timeout_ms;
	uint8_t cmd_status;
	int ret;

	do {
		k_msleep(CH395_CMD_POLL_INTERVAL_MS);

		ret = ch395_cmd_status_locked(dev, &cmd_status);
		if (ret < 0) {
			return ret;
		}

		if (cmd_status != CH395_CMD_STATUS_BUSY) {
			if (status != NULL) {
				*status = cmd_status;
			}

			return cmd_status == CH395_CMD_STATUS_SUCCESS ? 0 : -EIO;
		}
	} while (k_uptime_get() < end);

	if (status != NULL) {
		*status = CH395_CMD_STATUS_BUSY;
	}

	return -ETIMEDOUT;
}

static int ch395_cmd_write_wait(const struct device *dev, uint8_t cmd,
				const uint8_t *req, size_t req_len,
				int32_t timeout_ms, uint8_t *status)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);

	ret = ch395_cmd_write_locked(dev, cmd, req, req_len);
	if (ret == 0) {
		ret = ch395_cmd_wait_status_locked(dev, timeout_ms, status);
		if (ret == -EIO && status != NULL && *status == CH395_CMD_GET_CMD_STATUS) {
			if (cmd == CH395_CMD_INIT_CH395) {
				k_msleep(CH395_INIT_FALLBACK_WAIT_MS);
				LOG_WRN("GET_CMD_STATUS returned command echo; "
					"using fixed INIT_CH395 wait");
				*status = CH395_CMD_STATUS_SUCCESS;
				ret = 0;
			} else if (cmd == CH395_CMD_OPEN_SOCKET_SN ||
				   cmd == CH395_CMD_CLOSE_SOCKET_SN) {
				k_msleep(CH395_SOCKET_FALLBACK_WAIT_MS);
				LOG_WRN("GET_CMD_STATUS returned command echo; "
					"using fixed socket command wait");
				*status = CH395_CMD_STATUS_SUCCESS;
				ret = 0;
			}
		}
	}

	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int __maybe_unused ch395_cmd_reset_all(const struct device *dev)
{
	return ch395_cmd_write(dev, CH395_CMD_RESET_ALL, NULL, 0);
}

static int __maybe_unused ch395_cmd_set_mac_addr(const struct device *dev,
						 const uint8_t *mac)
{
	return ch395_cmd_write(dev, CH395_CMD_SET_MAC_ADDR, mac, NET_ETH_ADDR_LEN);
}

static int __maybe_unused ch395_cmd_set_mac_filter(const struct device *dev,
						   uint8_t flags,
						   uint32_t hash0,
						   uint32_t hash1)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);

	ret = ch395_cmd_begin(dev, CH395_CMD_SET_MAC_FILT);
	if (ret == 0) {
		ret = ch395_spi_byte(dev, flags, NULL);
	}
	if (ret == 0) {
		ret = ch395_write_le32_locked(dev, hash0);
	}
	if (ret == 0) {
		ret = ch395_write_le32_locked(dev, hash1);
	}
	ch395_cmd_end(dev);

	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int ch395_cmd_get_phy_status(const struct device *dev, uint8_t *status)
{
	return ch395_read_reg8(dev, CH395_CMD_GET_PHY_STATUS, status);
}

static int __maybe_unused ch395_cmd_init_ch395(const struct device *dev, uint8_t *status)
{
	return ch395_cmd_write_wait(dev, CH395_CMD_INIT_CH395, NULL, 0,
				    CH395_CMD_TIMEOUT_MS, status);
}

static int ch395_cmd_get_global_int_status_all(const struct device *dev, uint16_t *status)
{
	uint8_t resp[2];
	int ret;

	ret = ch395_cmd_read(dev, CH395_CMD_GET_GLOB_INT_STATUS_ALL, NULL, 0,
			     resp, sizeof(resp));
	if (ret < 0) {
		return ret;
	}

	*status = resp[0] | ((uint16_t)resp[1] << 8);

	return 0;
}

static int ch395_cmd_get_socket_int_status(const struct device *dev, uint8_t sock,
					   uint8_t *status)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	/* WCH reference requires a TSC delay between writing the socket
	 * index and reading the interrupt status byte.
	 */
	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);

	ret = ch395_cmd_begin(dev, CH395_CMD_GET_INT_STATUS_SN);
	if (ret == 0) {
		ret = ch395_spi_byte(dev, sock, NULL);
	}
	if (ret == 0) {
		k_busy_wait(CH395_INTERBYTE_DELAY_US);
		ret = ch395_spi_byte(dev, CH395_SPI_DUMMY, status);
	}

	ch395_cmd_end(dev);
	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int __maybe_unused ch395_cmd_set_socket_proto(const struct device *dev,
						     uint8_t sock,
						     uint8_t proto)
{
	uint8_t req[2] = { sock, proto };

	return ch395_cmd_write(dev, CH395_CMD_SET_PROTO_TYPE_SN, req, sizeof(req));
}

static int __maybe_unused ch395_cmd_open_socket(const struct device *dev, uint8_t sock,
						uint8_t *status)
{
	return ch395_cmd_write_wait(dev, CH395_CMD_OPEN_SOCKET_SN, &sock, sizeof(sock),
				    CH395_CMD_TIMEOUT_MS, status);
}

static int __maybe_unused ch395_cmd_close_socket(const struct device *dev, uint8_t sock,
						 uint8_t *status)
{
	return ch395_cmd_write_wait(dev, CH395_CMD_CLOSE_SOCKET_SN, &sock, sizeof(sock),
				    CH395_CMD_TIMEOUT_MS, status);
}

static int ch395_cmd_get_recv_len(const struct device *dev, uint8_t sock, uint16_t *len)
{
	struct ch395_runtime *ctx = dev->data;
	uint8_t resp[2];
	int ret;

	/* WCH reference requires a TSC delay between writing the socket
	 * index and reading the receive length bytes.
	 */
	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);

	ret = ch395_cmd_begin(dev, CH395_CMD_GET_RECV_LEN_SN);
	if (ret == 0) {
		ret = ch395_spi_byte(dev, sock, NULL);
	}
	if (ret == 0) {
		k_busy_wait(CH395_INTERBYTE_DELAY_US);
		ret = ch395_read_bytes_locked(dev, resp, sizeof(resp));
	}

	ch395_cmd_end(dev);
	k_mutex_unlock(&ctx->cmd_lock);

	if (ret < 0) {
		return ret;
	}

	*len = resp[0] | ((uint16_t)resp[1] << 8);

	return 0;
}

static int __maybe_unused ch395_cmd_read_recv_buf(const struct device *dev, uint8_t sock,
						  uint16_t len, uint8_t *buf)
{
	uint8_t req[3] = {
		sock,
		len & 0xff,
		len >> 8,
	};

	return ch395_cmd_read(dev, CH395_CMD_READ_RECV_BUF_SN, req, sizeof(req), buf, len);
}

static int __maybe_unused ch395_cmd_write_send_buf(const struct device *dev, uint8_t sock,
						   uint16_t len, const uint8_t *buf)
{
	struct ch395_runtime *ctx = dev->data;
	int ret;

	k_mutex_lock(&ctx->cmd_lock, K_FOREVER);

	ret = ch395_cmd_begin(dev, CH395_CMD_WRITE_SEND_BUF_SN);
	if (ret == 0) {
		ret = ch395_spi_byte(dev, sock, NULL);
	}
	if (ret == 0) {
		ret = ch395_write_le16_locked(dev, len);
	}
	if (ret == 0) {
		ret = ch395_write_bytes_locked(dev, buf, len);
	}
	ch395_cmd_end(dev);

	k_mutex_unlock(&ctx->cmd_lock);

	return ret;
}

static int __maybe_unused ch395_cmd_clear_recv_buf(const struct device *dev, uint8_t sock)
{
	return ch395_cmd_write(dev, CH395_CMD_CLEAR_RECV_BUF_SN, &sock, sizeof(sock));
}

static int ch395_hw_reset(const struct device *dev)
{
	const struct ch395_config *config = dev->config;
	int ret;

	ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Unable to configure reset GPIO pin %u", config->reset_gpio.pin);
		return ret;
	}

	ret = gpio_pin_set_dt(&config->reset_gpio, 1);
	if (ret < 0) {
		return ret;
	}

	k_msleep(CH395_RESET_ASSERT_MS);

	ret = gpio_pin_set_dt(&config->reset_gpio, 0);
	if (ret < 0) {
		return ret;
	}

	k_msleep(CH395_RESET_READY_MS);

	return 0;
}

static int ch395_phase3_validate(const struct device *dev, uint8_t *version)
{
	uint16_t global_int;
	uint16_t recv_len;
	uint8_t socket_int;
	uint8_t phy;
	uint8_t check;
	int ret;

	ret = ch395_check_exist(dev, CH395_CHECK_EXIST_TEST, &check);
	if (ret < 0) {
		LOG_ERR("CHECK_EXIST transfer failed: %d", ret);
		return ret;
	}

	if (check != CH395_CHECK_EXIST_EXPECTED) {
		LOG_ERR("CHECK_EXIST failed: wrote 0x%02x, read 0x%02x",
			CH395_CHECK_EXIST_TEST, check);
		return -ENODEV;
	}

	ret = ch395_read_reg8(dev, CH395_CMD_GET_IC_VER, version);
	if (ret < 0) {
		LOG_ERR("GET_IC_VER transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_get_phy_status(dev, &phy);
	if (ret < 0) {
		LOG_ERR("GET_PHY_STATUS transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_get_global_int_status_all(dev, &global_int);
	if (ret < 0) {
		LOG_ERR("GET_GLOB_INT_STATUS_ALL transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_get_socket_int_status(dev, CH395_SOCKET_MACRAW, &socket_int);
	if (ret < 0) {
		LOG_ERR("GET_INT_STATUS_SN transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_get_recv_len(dev, CH395_SOCKET_MACRAW, &recv_len);
	if (ret < 0) {
		LOG_ERR("GET_RECV_LEN_SN transfer failed: %d", ret);
		return ret;
	}

	LOG_INF("CH395 phase3 command validation OK: version 0x%02x phy 0x%02x "
		"gint 0x%04x sint0 0x%02x rxlen %u",
		*version, phy, global_int, socket_int, recv_len);

	return 0;
}

static const char *ch395_link_speed_str(enum phy_link_speed speed)
{
	return PHY_LINK_IS_SPEED_100M(speed) ? "100" : "10";
}

static enum phy_link_speed ch395_phy_status_to_speed(uint8_t phy)
{
	if ((phy & CH395_PHY_100M_FULL) != 0) {
		return LINK_FULL_100BASE;
	}

	if ((phy & CH395_PHY_100M_HALF) != 0) {
		return LINK_HALF_100BASE;
	}

	if ((phy & CH395_PHY_10M_FULL) != 0) {
		return LINK_FULL_10BASE;
	}

	if ((phy & CH395_PHY_10M_HALF) != 0) {
		return LINK_HALF_10BASE;
	}

	return 0;
}

static void ch395_update_link_status(const struct device *dev)
{
	struct ch395_runtime *ctx = dev->data;
	enum phy_link_speed speed;
	uint8_t phy;
	int ret;

	if (ctx->iface == NULL) {
		return;
	}

	ret = ch395_cmd_get_phy_status(dev, &phy);
	if (ret < 0) {
		LOG_ERR("GET_PHY_STATUS transfer failed: %d", ret);
		return;
	}

	speed = ch395_phy_status_to_speed(phy);
	if ((phy & CH395_PHY_DISCONN) != 0 || speed == 0) {
		if (ctx->state.is_up) {
			ctx->state.is_up = false;
			ctx->state.speed = 0;
			net_eth_carrier_off(ctx->iface);
			LOG_INF("%s: Link down (phy 0x%02x)", dev->name, phy);
		} else if (!ctx->link_reported) {
			LOG_INF("%s: Link down (phy 0x%02x)", dev->name, phy);
		}

		ctx->link_reported = true;
		return;
	}

	if (!ctx->state.is_up) {
		ctx->state.is_up = true;
		net_eth_carrier_on(ctx->iface);
	}

	if (ctx->state.speed != speed) {
		ctx->state.speed = speed;
		LOG_INF("%s: Link speed %s Mb, %s duplex", dev->name,
			ch395_link_speed_str(speed),
			PHY_LINK_IS_FULL_DUPLEX(speed) ? "full" : "half");
	}

	ctx->link_reported = true;
}

static void ch395_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_msleep(CONFIG_ETH_CH395_MONITOR_PERIOD);
		ch395_update_link_status(dev);
	}
}

static void ch395_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct ch395_runtime *ctx = dev->data;

	ctx->iface = iface;
	net_if_set_link_addr(iface, ctx->mac_addr, sizeof(ctx->mac_addr), NET_LINK_ETHERNET);

	ethernet_init(iface);
	net_eth_carrier_off(iface);
	ch395_update_link_status(dev);

	k_thread_create(&ctx->thread, ctx->thread_stack,
			CONFIG_ETH_CH395_RX_THREAD_STACK_SIZE,
			ch395_thread, (void *)dev, NULL, NULL,
			K_PRIO_COOP(CONFIG_ETH_CH395_RX_THREAD_PRIO),
			0, K_NO_WAIT);
	k_thread_name_set(&ctx->thread, "eth_ch395");
}

static enum ethernet_hw_caps ch395_get_capabilities(const struct device *dev,
						    struct net_if *iface)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);

	return 0;
}

static const struct device *ch395_get_phy(const struct device *dev,
					  struct net_if *iface)
{
	const struct ch395_config *config = dev->config;

	ARG_UNUSED(iface);

	return config->phy_dev;
}

static int ch395_send(const struct device *dev, struct net_pkt *pkt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pkt);

	return -ENOTSUP;
}

static const struct ethernet_api ch395_api = {
	.iface_api.init = ch395_iface_init,
	.get_capabilities = ch395_get_capabilities,
	.get_phy = ch395_get_phy,
	.send = ch395_send,
};

static int ch395_get_link_state(const struct device *dev,
				struct phy_link_state *state)
{
	struct ch395_runtime *ctx = dev->data;

	*state = ctx->state;

	return 0;
}

static DEVICE_API(ethphy, ch395_phy_driver_api) = {
	.get_link = ch395_get_link_state,
};

static int ch395_init(const struct device *dev)
{
	const struct ch395_config *config = dev->config;
	struct ch395_runtime *ctx = dev->data;
	uint8_t cmd_status;
	uint8_t phy;
	uint8_t version;
	int64_t phy_deadline;
	int ret;

	k_mutex_init(&ctx->cmd_lock);

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus %s not ready", config->spi.bus->name);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->int_gpio)) {
		LOG_ERR("INT GPIO port %s not ready", config->int_gpio.port->name);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->cs_gpio)) {
		LOG_ERR("CS GPIO port %s not ready", config->cs_gpio.port->name);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->reset_gpio)) {
		LOG_ERR("Reset GPIO port %s not ready", config->reset_gpio.port->name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->cs_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Unable to configure CS GPIO pin %u", config->cs_gpio.pin);
		return ret;
	}

	ret = gpio_pin_configure_dt(&config->int_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Unable to configure INT GPIO pin %u", config->int_gpio.pin);
		return ret;
	}

	ret = ch395_hw_reset(dev);
	if (ret < 0) {
		LOG_ERR("Hardware reset failed: %d", ret);
		return ret;
	}

	ret = ch395_phase3_validate(dev, &version);
	if (ret < 0) {
		return ret;
	}

	ret = net_eth_mac_load(&config->mac_cfg, ctx->mac_addr);
	if (ret < 0) {
		LOG_ERR("Failed to load MAC address: %d", ret);
		return ret;
	}

	ret = ch395_cmd_set_mac_addr(dev, ctx->mac_addr);
	if (ret < 0) {
		LOG_ERR("SET_MAC_ADDR transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_init_ch395(dev, &cmd_status);
	if (ret < 0) {
		LOG_ERR("INIT_CH395 failed: status 0x%02x ret %d", cmd_status, ret);
		return ret;
	}

	/* Both WCH and GD32 reference implementations poll GET_PHY_STATUS
	 * until the link is up before SET_PROTO_TYPE_SN and OPEN_SOCKET_SN.
	 * Opening the socket before PHY link is established can leave the
	 * MACRAW path in an inconsistent state on CH395Q.
	 */
	if (CONFIG_ETH_CH395_PHY_LINK_TIMEOUT_MS > 0) {
		phy_deadline = k_uptime_get() + CONFIG_ETH_CH395_PHY_LINK_TIMEOUT_MS;
		do {
			ret = ch395_cmd_get_phy_status(dev, &phy);
			if (ret < 0) {
				LOG_ERR("GET_PHY_STATUS transfer failed: %d", ret);
				return ret;
			}

			if ((phy & CH395_PHY_DISCONN) == 0) {
				break;
			}

			LOG_INF("Waiting for PHY link (phy 0x%02x)...", phy);
			k_msleep(CH395_PHY_LINK_POLL_INTERVAL_MS);
		} while (k_uptime_get() < phy_deadline);

		if ((phy & CH395_PHY_DISCONN) != 0) {
			LOG_WRN("PHY link not up after %u ms (phy 0x%02x); "
				"continuing with socket setup",
				CONFIG_ETH_CH395_PHY_LINK_TIMEOUT_MS, phy);
		} else {
			LOG_INF("PHY link up (phy 0x%02x)", phy);
		}
	}

	ret = ch395_cmd_set_socket_proto(dev, CH395_SOCKET_MACRAW, CH395_PROTO_TYPE_MACRAW);
	if (ret < 0) {
		LOG_ERR("SET_PROTO_TYPE_SN transfer failed: %d", ret);
		return ret;
	}

	ret = ch395_cmd_open_socket(dev, CH395_SOCKET_MACRAW, &cmd_status);
	if (ret < 0) {
		LOG_ERR("OPEN_SOCKET_SN failed: status 0x%02x ret %d", cmd_status, ret);
		return ret;
	}

	LOG_INF("CH395 phase4 MACRAW init OK: version 0x%02x MAC "
		"%02x:%02x:%02x:%02x:%02x:%02x",
		version, ctx->mac_addr[0], ctx->mac_addr[1], ctx->mac_addr[2],
		ctx->mac_addr[3], ctx->mac_addr[4], ctx->mac_addr[5]);

	return 0;
}

#define CH395_SPI_OPERATION \
	(SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

#define CH395_INIT(inst)							\
	DEVICE_DECLARE(eth_ch395_phy_##inst);				\
	static struct ch395_runtime ch395_runtime_##inst;			\
	static const struct ch395_config ch395_config_##inst = {		\
		.mac_cfg = NET_ETH_MAC_DT_INST_CONFIG_INIT(inst),		\
		.spi = SPI_DT_SPEC_INST_GET(inst, CH395_SPI_OPERATION),		\
		.cs_gpio = SPI_CS_GPIOS_DT_SPEC_INST_GET(inst),			\
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),		\
		.phy_dev = DEVICE_GET(eth_ch395_phy_##inst),			\
	};									\
	ETH_NET_DEVICE_DT_INST_DEFINE(inst, ch395_init, NULL,			\
				      &ch395_runtime_##inst,			\
				      &ch395_config_##inst,			\
				      CONFIG_ETH_INIT_PRIORITY, &ch395_api,	\
				      NET_ETH_MTU);				\
	DEVICE_DEFINE(eth_ch395_phy_##inst,				\
		      DEVICE_DT_NAME(DT_DRV_INST(inst)) "_phy",		\
		      NULL, NULL, &ch395_runtime_##inst,			\
		      &ch395_config_##inst, POST_KERNEL,			\
		      CONFIG_ETH_INIT_PRIORITY, &ch395_phy_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CH395_INIT)
