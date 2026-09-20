/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_AP3216C_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_AP3216C_H_

#include <zephyr/drivers/sensor.h>

/** @brief AP3216C channels expressed in uncalibrated ADC counts. */
enum sensor_channel_ap3216c {
	/** Ambient light ADC value, 0 to 65535 counts. */
	SENSOR_CHAN_AP3216C_ALS_RAW = SENSOR_CHAN_PRIV_START,
	/** Infrared ADC value, 0 to 1023 counts. */
	SENSOR_CHAN_AP3216C_IR_RAW,
	/** Proximity ADC value, 0 to 1023 counts; not a distance. */
	SENSOR_CHAN_AP3216C_PROX_RAW,
};

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_AP3216C_H_ */
