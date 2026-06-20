/*
 * Copyright (c) 2024 Dhiru Kholia
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NOTE: See this table for IC family reference,
 * in conjunction with Page 5 of the reference manual:
 * https://www.wch-ic.com/products/productsCenter/mcuInterface?categoryId=70
 */

#ifndef _CH32FUN_H
#define _CH32FUN_H

#if defined(CONFIG_SOC_SERIES_QINGKE_V2A)
#define CH32V003 1
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_QINGKE_V2A) */

#if defined(CONFIG_SOC_SERIES_QINGKE_V2C)
#define CH32V00x 1
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_QINGKE_V2C) */

#if defined(CONFIG_SOC_SERIES_QINGKE_V4B)
#define CH32V20x    1
#define CH32V20x_D6 1
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_QINGKE_V4B) */

#if defined(CONFIG_SOC_SERIES_QINGKE_V4C)
#define CH32V20x 1
#if defined(CONFIG_SOC_CH32V208)
#define CH32V20x_D8W 1
#endif
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_QINGKE_V4C) */

#if defined(CONFIG_SOC_SERIES_QINGKE_V4F)
#define CH32V30x 1
#if defined(CONFIG_SOC_CH32V303)
#define CH32V30x_D8 1
#elif defined(CONFIG_SOC_CH32V305) || defined(CONFIG_SOC_CH32V307) || defined(CONFIG_SOC_CH32V317)
#define CH32V30x_D8C 1
#endif
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_QINGKE_V4F) */

#if defined(CONFIG_SOC_SERIES_CH32H41X)
#define CH32H41x 1
/*
 * ch32fun's CH32H41x header emits an informational #warning reminding bare-metal
 * users that H41x GPIO output-speed bits live in their own register. Zephyr
 * handles that in its pinctrl driver, so the reminder is harmless here. It is
 * left visible on purpose: the proper fix is to skip the #warning under
 * __ZEPHYR__ in the hal_wch module itself, after which the west.yml hal_wch
 * revision can be bumped.
 */
#include <ch32fun.h>
#endif /* defined(CONFIG_SOC_SERIES_CH32H41X) */

#endif
