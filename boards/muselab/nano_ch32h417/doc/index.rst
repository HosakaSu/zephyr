.. zephyr:board:: nano_ch32h417

Overview
********

The MuseLab nanoCH32H417 is a compact development board for the WCH
CH32H417QEU6. The SoC contains two asymmetric RISC-V cores:

* QingKe V3F boot core, hart 0
* QingKe V5F application core, hart 1

Zephyr treats the CH32H417 as an AMP system with separate CPU cluster targets,
not as an SMP system. On cold reset only V3F starts; a small V3F image must
wake the V5F application image.

Hardware
********

The board has a 25 MHz HSE crystal and an onboard WCH-LinkE. The LinkE exposes
the target USART1 console as a USB VCOM: USART1 TX is PA9 AF7 and RX is PA10
AF7. The board also provides active-low blue and green user LEDs on PC3 and
PC2.

Schematics and other hardware files are available from the
`nanoCH32H417 hardware repository`_.

Supported Features
^^^^^^^^^^^^^^^^^^

.. zephyr:board-supported-hw::

The initial support scope is dual-core boot, system clocks, the two GPIO LEDs,
USART1 console/shell, build metadata, and the supplied dual-core OpenOCD
configuration. USB, Ethernet, LCD, SDIO, and other board peripherals are not
enabled by this board definition.

Connections and IOs
^^^^^^^^^^^^^^^^^^^

.. list-table:: V5F console
   :header-rows: 1

   * - Signal
     - Pin
     - Alternate function
     - Settings
   * - USART1 TX
     - PA9
     - AF7
     - 115200 8N1
   * - USART1 RX
     - PA10
     - AF7
     - 115200 8N1

.. list-table:: User LEDs
   :header-rows: 1

   * - Alias
     - Color
     - Pin
     - Polarity
   * - led0
     - Blue
     - PC3
     - Active low
   * - led1
     - Green
     - PC2
     - Active low

CPU Cluster Targets
*******************

Applications must be built for one of these targets:

.. list-table:: Board targets
   :header-rows: 1

   * - Target
     - Purpose
     - Boot hart
     - Image address
   * - ``nano_ch32h417/ch32h417/v3f``
     - Console-free V5F waker
     - 0
     - 0x00000000
   * - ``nano_ch32h417/ch32h417/v5f``
     - Main application
     - 1
     - 0x08010000

The recommended application target is ``nano_ch32h417/ch32h417/v5f``. The V3F
target is intended only to wake V5F and does not own board peripherals.

Programming and Debugging
*************************

A bootable image requires both cores. Build the V3F waker and a V5F
application separately, convert them to raw binaries, pad the V3F binary with
``0xff`` to offset ``0x10000``, append the V5F binary, and program that merged
image once at ``0x00000000``.

For example:

.. code-block:: console

   $ west build -p always -b nano_ch32h417/ch32h417/v3f \
       -d build-v3f samples/basic/minimal
   $ west build -p always -b nano_ch32h417/ch32h417/v5f \
       -d build-v5f samples/hello_world
   $ python3 - <<'PY'
   from pathlib import Path
   v3f = Path("build-v3f/zephyr/zephyr.bin").read_bytes()
   v5f = Path("build-v5f/zephyr/zephyr.bin").read_bytes()
   if len(v3f) > 0x10000:
       raise SystemExit("V3F image overlaps V5F")
   merged = bytearray([0xff]) * 0x10000
   merged[:len(v3f)] = v3f
   Path("nano_ch32h417_dual.bin").write_bytes(merged + v5f)
   PY
   $ openocd -f boards/muselab/nano_ch32h417/support/openocd.cfg \
       -c init -c halt \
       -c "program nano_ch32h417_dual.bin 0x00000000 verify" \
       -c reset -c shutdown

The V5F image is linked at ``0x08010000``. CH32H417 maps that location to the
same physical flash as offset ``0x00010000`` in the low alias used by the
merged-image programming flow. Programming the two images independently can
erase the other core's image, so merge them before flashing. A complete
hardware verification should also dump the exact merged-image length and
compare it byte-for-byte with the input image.

The onboard WCH-LinkE VCOM normally appears as ``/dev/ttyACM0`` on Linux. Open
the port at 115200 8N1 to view the V5F console. The V3F target is intentionally
silent.

References
**********

.. target-notes::

.. _WCH: https://www.wch-ic.com
.. _nanoCH32H417 hardware repository: https://github.com/wuxx/nanoCH32H417
