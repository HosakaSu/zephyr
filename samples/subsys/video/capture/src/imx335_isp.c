/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <stm32n6xx_hal.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video/stm32_dcmipp.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/video/video.h>

LOG_MODULE_REGISTER(imx335_isp, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(CONFIG_SAMPLE_IMX335_SENSOR_GAIN % 300 == 0,
	     "IMX335 gain must be a multiple of 300 millidecibels");

static const struct device *sensor;

static void encode_gain(uint32_t gain, uint8_t *shift, uint8_t *multiplier)
{
	/* DCMIPP gain is multiplier / 128 times 2 raised to shift. */
	uint32_t value = DIV_ROUND_CLOSEST(gain * 128U, 1000U);

	*shift = 0U;
	while (value > UINT8_MAX) {
		value /= 2U;
		(*shift)++;
	}
	*multiplier = value;
}

int stm32_dcmipp_isp_init(DCMIPP_HandleTypeDef *hdcmipp, const struct device *source)
{
	const DCMIPP_BlackLevelConfTypeDef black_level = {
		.RedCompBlackLevel = 12U,
		.GreenCompBlackLevel = 12U,
		.BlueCompBlackLevel = 12U,
	};
	DCMIPP_ExposureConfTypeDef gains = {
		.MultiplierGreen = 128U,
	};

	sensor = source;
	encode_gain(CONFIG_SAMPLE_IMX335_RED_GAIN, &gains.ShiftRed, &gains.MultiplierRed);
	encode_gain(CONFIG_SAMPLE_IMX335_BLUE_GAIN, &gains.ShiftBlue, &gains.MultiplierBlue);

	if (HAL_DCMIPP_PIPE_SetISPBlackLevelCalibrationConfig(hdcmipp, DCMIPP_PIPE1,
							      &black_level) != HAL_OK ||
	    HAL_DCMIPP_PIPE_EnableISPBlackLevelCalibration(hdcmipp, DCMIPP_PIPE1) != HAL_OK ||
	    HAL_DCMIPP_PIPE_SetISPExposureConfig(hdcmipp, DCMIPP_PIPE1, &gains) != HAL_OK ||
	    HAL_DCMIPP_PIPE_EnableISPExposure(hdcmipp, DCMIPP_PIPE1) != HAL_OK ||
	    HAL_DCMIPP_PIPE_EnableGammaConversion(hdcmipp, DCMIPP_PIPE1) != HAL_OK) {
		LOG_ERR("Failed to configure IMX335 image processing");
		return -EIO;
	}

	LOG_INF("Black level and gamma enabled; R/B gains %u/%u (1000 = unity)",
		CONFIG_SAMPLE_IMX335_RED_GAIN, CONFIG_SAMPLE_IMX335_BLUE_GAIN);
	return 0;
}

int stm32_dcmipp_isp_start(void)
{
	struct video_control gain = {
		.id = VIDEO_CID_ANALOGUE_GAIN,
		.val = CONFIG_SAMPLE_IMX335_SENSOR_GAIN,
	};
	int ret;

	if (sensor == NULL) {
		return -ENODEV;
	}

	ret = video_set_ctrl(sensor, &gain);
	if (ret < 0) {
		LOG_ERR("Failed to set sensor gain (%d)", ret);
		return ret;
	}

	LOG_INF("Sensor gain: %u millidecibels", CONFIG_SAMPLE_IMX335_SENSOR_GAIN);
	return 0;
}
