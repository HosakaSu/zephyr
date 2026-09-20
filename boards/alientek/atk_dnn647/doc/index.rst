.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

.. zephyr:board:: atk_dnn647

Overview
********

The ALIENTEK ATK-DNN647 development board combines an ATK-CNN647B core
board with a peripheral baseboard. It uses the STM32N647X0H3Q, an Arm
Cortex-M55 microcontroller with 4.2 MB of SRAM and no internal flash.

The board includes external NOR flash and HyperRAM, a USB-to-UART bridge,
user LEDs and buttons, and connectors for displays, cameras and networking.
See the `ALIENTEK board documentation`_ for hardware details.

Supported Features
******************

.. zephyr:board-supported-hw::

This board target runs a secure application from 511 KB of AXISRAM2 at
``0x34180400``. The CPU runs at 800 MHz, with a 400 MHz system clock and a
200 MHz AHB clock, using the 48 MHz HSE oscillator.

External NOR flash, HyperRAM and USB are not configured by this
board target. It does not provide an external flash bootloader or persistent
application programming. The optional video configuration below supports an
IMX335 camera and an 800 x 480 RGB LCD.

Connections
===========

The default console is USART1 at 115200 baud, 8 data bits, no parity and one
stop bit. Connect the baseboard's USB UART connector to the host, or connect
a 3.3 V USB-to-UART adapter: adapter RX to PE5 (USART1 TX), adapter TX to
PE6 (USART1 RX), and adapter GND to board GND.

.. list-table:: Default pin assignments
   :header-rows: 1

   * - Function
     - Pin
     - Devicetree alias
     - Active level
   * - LED0
     - PG10
     - ``led0``
     - Low
   * - LED1
     - PE10
     - ``led1``
     - Low
   * - KEY0
     - PC6
     - ``sw0``
     - Low
   * - KEY1
     - PD1
     - ``sw1``
     - Low
   * - KEY2
     - PG11
     - ``sw2``
     - Low
   * - WK_UP
     - PC13
     - ``sw3``
     - High

Baseboard peripherals
=====================

The board devicetree describes the peripherals below. Applications enable the
corresponding driver through Kconfig; the default console remains USART1.
The CH340C is a host USB-to-UART bridge and needs no Zephyr USB driver.

.. list-table:: Additional peripheral connections
   :header-rows: 1

   * - Function
     - Connection
     - Notes
   * - Active buzzer
     - PD3, active high
     - ``zephyr,user`` property ``buzzer-gpios``
   * - Potentiometer
     - PF6, ADC1 channel 15
     - P14 pins 2-3; 0 to 1.8 V, 1.8 V ADC reference
   * - LCD backlight
     - PA3, TIM16 channel 1
     - ``pwm-led0``; active high, 10 kHz default period
   * - EEPROM
     - I2C4 PE13/PE14, address 0x50
     - 24C02B, 256 bytes, 8-byte pages; ``eeprom-0``
   * - TF card
     - SDMMC1 PC8-PC12 and PH2
     - Disk ``SD``; 4-bit bus, no connected card-detect GPIO
   * - SD NAND
     - SDMMC2 PC4/PC5/PC0/PE4, PD2 clock, PC3 command
     - MKDV16GIL-STPA; disk ``SD2``, SD protocol, not eMMC
   * - USART3
     - PE1 TX, PD9 RX
     - P8 selects RS232 or RS485
   * - UART7
     - PH3 TX, PE7 RX
     - P9 selects RS232 or the optional ATK module
   * - SPI5
     - PE15 SCK, PH7 MOSI, PH8 MISO, PH6 CS
     - Wireless socket; no module driver enabled by default
   * - CAN
     - FDCAN3 PE11 TX, PE12 RX
     - SIT1050 transceiver, maximum bitrate 1 Mbit/s
   * - Six-axis IMU
     - I2C4 address 0x6a, INT1 on PF3
     - QMI8658A, using the existing ``qst,qmi8658a`` driver

The buzzer is an active device: configure ``buzzer-gpios`` as an output and
set its logical value to one to sound it, or zero to stop it. Do not drive
it as an LED or use a tone-generation PWM driver.

The ADC channel is available through ``ADC_DT_SPEC_GET(DT_PATH(zephyr_user))``.
The potentiometer supply and ADC reference are both 1.8 V. The long acquisition
time accommodates the potentiometer's source impedance. The generic
``samples/drivers/adc/adc_dt`` sample uses this channel directly.

The PWM backlight is available through ``PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0))``.
The camera preview overlay disables this PWM device because that configuration
uses the LTDC driver's GPIO backlight control on the same PA3 pin.

Serial expansion
----------------

For USART3 RS232, fit P8 jumpers 1-3 and 2-4. For RS485, use P8 jumpers
3-5 and 4-6 instead. The RS485 transceiver's tied DE and active-low RE
signals are connected to PD15, exposed as ``zephyr,user`` property
``rs485-de-gpios``. Low selects reception; high selects transmission.
PD15 is controlled as a GPIO, not as the USART hardware DE alternate function.
The application must drive DE high before transmission, wait until the last
stop bit has left the USART, then drive DE low. With the interrupt-driven UART
API, use ``uart_irq_tx_complete()`` to determine completion; TX FIFO readiness
alone does not mean that the line is idle.

For UART7 RS232, fit P9 jumpers 1-3 and 2-4. P9 jumpers 3-5 and 4-6 route
the UART to the optional ATK module. The UARTs default to 115200 baud, 8N1.
RS232 and RS485 connectors use their transceiver signal levels; a TTL adapter
connects to the MCU-side pins, not to these external connectors.

The SPI5 wireless socket supplies PH6 as chip select 0. Applications can add
an appropriate SPI child node in an overlay when a module is fitted. The
socket's PD13 CE and PD11 interrupt signals are module-specific and are not
claimed by the base configuration.

CAN
---

Fit P10 jumpers 1-3 and 2-4 to connect the SIT1050 to FDCAN3. Connect CANH,
CANL and a common signal ground to the peer; account for the board's fitted
120-ohm termination when terminating the bus. ``zephyr,canbus`` selects FDCAN3.
Use classic CAN at up to 1 Mbit/s. The MCU's CAN FD controller does not imply
that the populated SIT1050 interface supports high-speed CAN FD data phases.
Internal loopback tests the controller only; external communication requires
a peer at the same bitrate and correct wiring.

The default peripheral shell uses classic CAN. Applications can enable
``CONFIG_CAN_FD_MODE`` for CAN FD experiments. With this board, keep the
arbitration and data-phase bitrates at or below 1 Mbit/s; the transceiver's
rating is not an assurance of CAN FD qualification. A host supporting CAN FD
can exercise 64-byte frames at 500 kbit/s without BRS, or 500 kbit/s arbitration
and 1 Mbit/s data with BRS. Higher data-phase rates are not supported by this
board configuration.

Storage
-------

Both storage interfaces use the SD subsystem and the STM32 SDHC driver. The
legacy STM32 disk driver is disabled by a board default because it supports
only one controller instance. Enable ``CONFIG_DISK_ACCESS`` and provide a heap
for the SDHC driver's transfer buffers, for example
``CONFIG_HEAP_MEM_POOL_SIZE=4096``.

The two disks are named ``SD`` (removable TF card) and ``SD2`` (soldered SD NAND).
The shared kernel clock is 50 MHz and the bus clock is limited to 25 MHz.
Secure applications configure both SDMMC DMA masters for secure SRAM access.
SDMMC2 uses PD2 for its clock; the Discovery Kit's PC2 mapping does not apply.
Neither interface provides a connected card-detect GPIO. Insert the TF card
before initializing its disk.

For FatFS, a single FAT32 partition in an MBR partition table is suitable.
The shell configuration below also supports an existing FAT filesystem on the
SD NAND. It disables formatting and mounts filesystems read-only so that
inspecting the board's existing data does not create or modify files. An
application that requires writes can use its own filesystem configuration.

To address both volumes with FatFS, set::

   CONFIG_FS_FATFS_CUSTOM_MOUNT_POINT_COUNT=2
   CONFIG_FS_FATFS_CUSTOM_MOUNT_POINTS="SD,SD2"

Peripheral shell
----------------

Build the shell sample with the optional board configuration:

.. code-block:: console

   west build -b atk_dnn647 samples/subsys/shell/shell_module -d build/peripherals -- \
     -DEXTRA_CONF_FILE="$ZEPHYR_BASE/boards/alientek/atk_dnn647/peripherals.conf"
   west flash -d build/peripherals

This enables ADC, PWM, I2C, EEPROM, SPI, CAN, sensor, input and filesystem shell
commands. Use ``device list`` to obtain device names and each command's
``help`` output for its arguments. For example, mount and inspect the TF card
with ``fs mount fat /SD:`` followed by ``fs ls /SD:``; use ``/SD2:`` for the
board's SD NAND. The built-in shell mount command supports one FAT
mount per boot; restart the application before choosing the other disk.
Applications can mount both disks concurrently using separate ``FATFS`` and
``fs_mount_t`` objects. The configuration enables the QMI8658A's
polled accelerometer and gyroscope API; declaring INT1 in devicetree alone
does not establish that data-ready interrupt operation has been verified.

The EEPROM shell permits writes. Back up bytes that must be preserved before
using a write command; EEPROM contents persist across firmware downloads.

Ethernet
========

The baseboard provides a 10/100 Mbps Ethernet port through a Motorcomm
YT8512C PHY, connected to ETH1 over RMII. The PHY address is strapped to 0.
The PHY uses its own 25 MHz crystal and supplies the RMII reference clock.
The generic MII PHY driver handles reset, auto-negotiation and link polling.
The MAC address is derived from the STM32 device UID.

.. list-table:: Ethernet pin assignments
   :header-rows: 1

   * - Signal
     - Pin
   * - RMII_REF_CLK
     - PF7
   * - RMII_CRS_DV
     - PF10
   * - RMII_TX_EN
     - PF11
   * - RMII_TXD0
     - PF12
   * - RMII_TXD1
     - PF13
   * - RMII_RXD0
     - PF14
   * - RMII_RXD1
     - PF15
   * - MDIO
     - PF4
   * - MDC
     - PH5
   * - PHY reset (active low)
     - PF5

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Connect a CMSIS-DAP probe to SWDIO, SWCLK, NRST and GND, and power the board.
Set jumper P6 B0 to GND and B1 to 3V3 (Development boot), then reset or power
cycle the board. Development boot keeps the debug interface accessible.

The OpenOCD configuration uses AP1 to access the Cortex-M55. The default
runner loads and verifies the ELF image in SRAM and starts its entry point.
The application must be downloaded again after power is removed. The
``west flash`` command for this target does not program external NOR flash.

Building and running Hello World
===============================

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: atk_dnn647
   :goals: build flash
   :compact:

Open the serial console before downloading to capture the startup message::

   Hello World! atk_dnn647/stm32n647xx

The STM32N6 build may warn if ``STM32_SigningTool_CLI`` is missing. A signed
image is not required for the OpenOCD SRAM download described here.

Running the DHCPv4 client
========================

Connect the RJ45 port to a network with a DHCPv4 server, then build and
download the sample:

.. zephyr-app-commands::
   :zephyr-app: samples/net/dhcpv4_client
   :board: atk_dnn647
   :goals: build flash
   :compact:

The USART1 console reports the negotiated link speed, assigned IPv4 address,
subnet mask and router. Use ``net iface`` in the serial shell to inspect the
interface, or ``net ping <router-address>`` to test connectivity.

Camera preview on the RGB LCD
=============================

Connect an ALIENTEK MCIMX335 camera to baseboard connector J2 and a 4.3-inch
800 x 480 ATK-MD0430R-800480 RGB LCD (panel ID 0x4384) to core board
connector J4. Keep the board in
Development boot mode for SRAM download.

Build the video capture sample with the supplied accessory configuration:

.. code-block:: console

   west build -b atk_dnn647 samples/subsys/video/capture -d build/video -- \
     -DDTC_OVERLAY_FILE="$ZEPHYR_BASE/boards/alientek/atk_dnn647/video_capture.overlay" \
     -DEXTRA_CONF_FILE="$ZEPHYR_BASE/boards/alientek/atk_dnn647/video_capture.conf"
   west flash -d build/video

The camera uses I2C2 on PD14/PD4, reset on PG4, enable on PG6, two CSI-2
data lanes and a module-local 37.125 MHz oscillator. DCMIPP converts and
scales the sensor image to 800 x 480 RGB565 for LTDC. LCD backlight control
uses PA3; the pixel clock is approximately 33.33 MHz.

Two capture buffers use the first 1536 KB of AXISRAM1, marked non-cacheable
for DMA access. This region does not overlap the application in AXISRAM2
or the OpenOCD work area. LTDC displays the capture buffers directly, so
external HyperRAM is not required. The console reports each captured frame.

The configuration enables the video shell alongside capture. Sensor gain can
be adjusted for the lighting conditions; for example, set 6 dB of gain with:

.. code-block:: console

   video ctrl camera@1a analogue_gain 6000

Gain is specified in millidecibels, in steps of 300. The accessory configuration
enables the sample's IMX335 processing preset: black level correction, gamma,
6 dB sensor gain, and fixed red/blue gains of 1.1/1.9. These settings are a
starting point for indoor lighting. Adjust ``CONFIG_SAMPLE_IMX335_SENSOR_GAIN``,
``CONFIG_SAMPLE_IMX335_RED_GAIN`` and ``CONFIG_SAMPLE_IMX335_BLUE_GAIN`` for other
conditions; color gains use thousandths (1000 means unity).

The preset does not implement automatic exposure or automatic white balance.
Brightness, contrast and color can differ from the vendor's full ISP example.

Debugging
=========

Start a debug session with:

.. code-block:: console

   west debug

Keep the board in Development boot mode. Power cycle it before starting a
new download or debug session if a previous application changed the clock
or security configuration.

.. _ALIENTEK board documentation:
   https://wiki.alientek.com/docs/Boards/STM32/DNN647/TOC/
