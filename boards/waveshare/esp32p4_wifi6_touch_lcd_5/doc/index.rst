.. zephyr:board:: esp32p4_wifi6_touch_lcd_5

Overview
********

The Waveshare ESP32-P4-WIFI6-Touch-LCD-5 is a multimedia development board built
around the ESP32-P4 SoC. It stacks 32 MB of PSRAM and connects a 32 MB NOR
flash, carries a 5-inch 720x1280 IPS panel driven over 2-lane MIPI DSI with a
GT911 capacitive touch controller, exposes a Type-C USB-to-UART port for
flashing and console, a 4-pin USB-OTG HS connector, a MIPI CSI camera connector,
a MicroSD slot, and a 40-pin GPIO header with the Raspberry Pi Pico pinout
(compatible with Pico HATs). Wireless connectivity is provided by an on-board
ESP32-C6-MINI-1 module connected over SDIO.

This board definition provides both high-performance (HP) core and low-power
(LP) core targets. The Zephyr console is routed to UART0 (GPIO37/38), which is
the port wired to the Type-C USB-to-UART bridge.

Hardware
********

The board included peripherals:

- ESP32-P4 SoC (silicon revision v1.3) with 32 MB stacked PSRAM and 32 MB
  on-board NOR flash
- 5-inch 720x1280 IPS panel (HX8394 controller) on a 2-lane MIPI DSI link at
  700 Mbit/s per lane, with reset on GPIO27 and a PWM-dimmed backlight on
  GPIO26
- GT911 capacitive touch controller on I2C0
- Type-C USB-to-UART port (CH343P) for power, flashing and serial console
- 4-pin USB 2.0 OTG HS connector
- 2-lane MIPI CSI camera connector
- MicroSD card slot (4-bit SDHC at 40 MHz: clk=43, cmd=44, d0=39, d1=40,
  d2=41, d3=42), powered through a GPIO45 load switch
- I2C0 bus (SDA=GPIO7, SCL=GPIO8, 400 kHz), shared by the touch controller and
  the audio codecs
- ESP32-C6-MINI-1 hosted Wi-Fi/Bluetooth coprocessor on SDIO (clk=18, cmd=19,
  d0..d3=14..17) with its enable line on GPIO54
- 40-pin GPIO expansion header (Raspberry Pi Pico pinout)
- Boot (GPIO35) and reset buttons

The 32 MB flash uses a custom partition table with two 15.75 MB application
slots (``image-0`` / ``image-1``) for MCUboot and two 32 KB slots for the
LP core images, followed by a 192 KB ``storage`` partition; the
``image-scratch`` and ``coredump`` partitions are located at the end of the
flash.

Four internal LDO regulators are configured as always-on: ``ldo1`` and ``ldo4``
at 3.3 V, ``ldo2`` at 1.8 V and ``ldo3`` at 2.5 V. ``ldo3`` supplies
``VDD_MIPI_DPHY``, so the DSI link does not come up without it. MIPI CSI and
I2S audio are not enabled in this board port.

Wi-Fi and Bluetooth reach the ESP32-C6 over the ``esp-hosted-mcu`` SDIO
transport, which pulls in the ``nanopb`` module (and therefore a ``protoc`` on
``PATH``). The SDIO pin map matches what the vendor firmware reports, but the
C6 ships with Waveshare's esp-hosted slave firmware for an ESP-IDF host: the
link enumerates and the coprocessor answers, yet every RPC times out. Flash the
coprocessor with esp-hosted-mcu firmware matching
``CONFIG_ESP_HOSTED_MCU_FW_VERSION_*`` before enabling ``CONFIG_BT`` or
``CONFIG_WIFI``.

Display and touch
=================

The panel is scanned out by the DSI bridge, so ``zephyr,display`` points at
``dsi_display`` rather than at the panel controller; the panel node only runs
the bring-up sequence and publishes the DPI timings that the host programs when
the panel attaches. Two RGB565 framebuffers of 1.76 MB each are allocated from
PSRAM, which is why ``ESP_SPIRAM_HEAP_SIZE`` is raised for this board.

The panel uses the out-of-tree ``waveshare,hx8394`` driver rather than the
in-tree ``himax,hx8394`` one. Both drive the same controller, but the power,
VCOM, GIP and gamma registers are properties of the glass: programmed with the
in-tree values, which belong to a Rocktech module, this panel stays black.

The GT911 interrupt line only reaches GPIO2 when the optional ``R108`` resistor
is populated, so the driver is left in polling mode and the I2C address that the
controller latched at reset is probed (``0x5D``, falling back to ``0x14``)
instead of being forced through the INT pin. GPIO2 is also routed to the 40-pin
header and must not be driven by an attached HAT.

The backlight is not a plain enable line: GPIO26 feeds the averaging network in
front of the AP3032 LED driver, so the panel stays dark unless the pin carries a
PWM signal. Neither the DSI bridge nor the HX8394 implements
``display_set_brightness()``, so the board turns the backlight on itself from a
``SYS_INIT`` in ``board.c``. It is exposed as the ``pwm-led0`` alias on LEDC
channel 0 at 5 kHz, and an application dims it with, for example:

.. code-block:: c

   static const struct pwm_dt_spec bl = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));

   pwm_set_pulse_dt(&bl, bl.period / 2);

.. include:: ../../../espressif/common/soc-esp32p4-features.rst
   :start-after: espressif-soc-esp32p4-features

Supported Features
==================

.. zephyr:board-supported-hw::

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

.. include:: ../../../espressif/common/building-flashing.rst
   :start-after: espressif-building-flashing

Debugging
=========

.. include:: ../../../espressif/common/openocd-debugging.rst
   :start-after: espressif-openocd-debugging

References
**********

.. target-notes::

.. _`ESP32-P4-WIFI6-Touch-LCD-5 product page`: https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm
.. _`ESP32-P4-WIFI6-Touch-LCD-5 documentation`: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5
.. _`ESP32-P4-WIFI6-Touch-LCD-5 schematic`: https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5/blob/main/hardware/schematic/ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf
