/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/display.h>
#ifdef CONFIG_ELCO5300_STATS
#include <zephyr/drivers/display/display_co5300.h>
#include <zephyr/spinlock.h>
#endif

#define SPI_WRITE_TIMEOUT 100

#define CO5300_RST_DELAY 150    ///< delay ms wait for reset finish
#define CO5300_SLPIN_DELAY 120  ///< delay ms wait for sleep in finish
#define CO5300_SLPOUT_DELAY 120 ///< delay ms wait for sleep out finish

// SPI Mode
#define CO5300_SPI_MODE_SINGLE 0x02
#define CO5300_SPI_MODE_QUAD_1_1_4 0x32

#define CO5300_C_QUAD_1_1_4 0x2C

// User Command
#define CO5300_C_NOP 0x00          // nop
#define CO5300_C_SWRESET 0x01      // Software Reset
#define CO5300_R_RDID 0x04         // Read Display Identification Information ID/1/2/3
#define CO5300_R_RDNERRORSDSI 0x05 // Read Number of Errors on DSI
#define CO5300_R_RDPOWERMODE 0x0A  // Read Display Power Mode
#define CO5300_R_RDMADCTL 0x0B     // Read Display MADCTL
#define CO5300_R_RDPIXFMT 0x0C     // Read Display Pixel Format
#define CO5300_R_RDIMGFMT 0x0D     // Read Display Image Mode
#define CO5300_R_RDSIGMODE 0x0E    // Read Display Signal Mode
#define CO5300_R_RDSELFDIAG 0x0F   // Read Display Self-Diagnostic Result

#define CO5300_C_SLPIN 0x10  // Sleep In
#define CO5300_C_SLPOUT 0x11 // Sleep Out
#define CO5300_C_PTLON 0x12  // Partial Display On
#define CO5300_C_NORON 0x13  // Normal Display mode on

#define CO5300_C_INVOFF 0x20  // Inversion Off
#define CO5300_C_INVON 0x21   // Inversion On
#define CO5300_C_ALLPOFF 0x22 // All pixels off
#define CO5300_C_ALLPON 0x23  // All pixels on
#define CO5300_C_DISPOFF 0x28 // Display off
#define CO5300_C_DISPON 0x29  // Display on
#define CO5300_W_CASET 0x2A   // Column Address Set
#define CO5300_W_PASET 0x2B   // Page Address Set
#define CO5300_W_RAMWR 0x2C   // Memory Write Start

#define CO5300_W_PTLAR 0x30   // Partial Area Row Set
#define CO5300_W_PTLAC 0x31   // Partial Area Column Set
#define CO5300_C_TEAROFF 0x34 // Tearing effect off
#define CO5300_WC_TEARON 0x35 // Tearing effect on
#define CO5300_W_MADCTL 0x36  // Memory data access control
#define CO5300_C_IDLEOFF 0x38 // Idle Mode Off
#define CO5300_C_IDLEON 0x39  // Idle Mode On
#define CO5300_W_PIXFMT 0x3A  // Write Display Pixel Format
#define CO5300_W_WRMC 0x3C    // Memory Write Continue

#define CO5300_W_SETTSL 0x44             // Write Tearing Effect Scan Line
#define CO5300_R_GETSL 0x45              // Read Scan Line Number
#define CO5300_C_SPIROFF 0x46            // SPI read Off
#define CO5300_C_SPIRON 0x47             // SPI read On
#define CO5300_C_AODMOFF 0x48            // AOD Mode Off
#define CO5300_C_AODMON 0x49             // AOD Mode On
#define CO5300_W_WDBRIGHTNESSVALAOD 0x4A // Write Display Brightness Value in AOD Mode
#define CO5300_R_RDBRIGHTNESSVALAOD 0x4B // Read Display Brightness Value in AOD Mode
#define CO5300_W_DEEPSTMODE 0x4F         // Deep Standby Mode On

#define CO5300_W_WDBRIGHTNESSVALNOR 0x51 // Write Display Brightness Value in Normal Mode
#define CO5300_R_RDBRIGHTNESSVALNOR 0x52 // Read display brightness value in Normal Mode
#define CO5300_W_WCTRLD1 0x53            // Write CTRL Display1
#define CO5300_R_RCTRLD1 0x54            // Read CTRL Display1
#define CO5300_W_WCTRLD2 0x55            // Write CTRL Display2
#define CO5300_R_RCTRLD2 0x56            // Read CTRL Display2
#define CO5300_W_WCE 0x58                // Write CE
#define CO5300_R_RCE 0x59                // Read CE

#define CO5300_W_WDBRIGHTNESSVALHBM 0x63 // Write Display Brightness Value in HBM Mode
#define CO5300_R_WDBRIGHTNESSVALHBM 0x64 // Read Display Brightness Value in HBM Mode
#define CO5300_W_WHBMCTL 0x66            // Write HBM Control

#define CO5300_W_COLORSET0 0x70  // Color Set 0
#define CO5300_W_COLORSET1 0x71  // Color Set 1
#define CO5300_W_COLORSET2 0x72  // Color Set 2
#define CO5300_W_COLORSET3 0x73  // Color Set 3
#define CO5300_W_COLORSET4 0x74  // Color Set 4
#define CO5300_W_COLORSET5 0x75  // Color Set 5
#define CO5300_W_COLORSET6 0x76  // Color Set 6
#define CO5300_W_COLORSET7 0x77  // Color Set 7
#define CO5300_W_COLORSET8 0x78  // Color Set 8
#define CO5300_W_COLORSET9 0x79  // Color Set 9
#define CO5300_W_COLORSET10 0x7A // Color Set 10
#define CO5300_W_COLORSET11 0x7B // Color Set 11
#define CO5300_W_COLORSET12 0x7C // Color Set 12
#define CO5300_W_COLORSET13 0x7D // Color Set 13
#define CO5300_W_COLORSET14 0x7E // Color Set 14
#define CO5300_W_COLORSET15 0x7F // Color Set 15

#define CO5300_W_COLOROPTION 0x80 // Color Option

#define CO5300_R_RDDBSTART 0xA1         // Read DDB start
#define CO5300_R_DDBCONTINUE 0xA8       // Read DDB Continue
#define CO5300_R_RFIRCHECKSUN 0xAA      // Read First Checksum
#define CO5300_R_RCONTINUECHECKSUN 0xAF // Read Continue Checksum

#define CO5300_W_SPIMODECTL 0xC4 // SPI mode control

#define CO5300_R_RDID1 0xDA // Read ID1
#define CO5300_R_RDID2 0xDB // Read ID2
#define CO5300_R_RDID3 0xDC // Read ID3

#define CO5300_W_PAGESET 0xFE // Set Page

// Flip
#define CO5300_MADCTL_X_AXIS_FLIP 0x02 // Flip Horizontal
#define CO5300_MADCTL_Y_AXIS_FLIP 0x05 // Flip Vertical

// Color Order
#define CO5300_MADCTL_RGB 0x00                      // Red-Green-Blue pixel order
#define CO5300_MADCTL_BGR 0x08                      // Blue-Green-Red pixel order

// Pixel Format
#define CO5300_PIXFMT_8BIT_GRAY256 0x11 // 8-bit GRAY256 pixel format
#define CO5300_PIXFMT_16BIT_RGB565 0x55 // 16-bit RGB565 pixel format
#define CO5300_PIXFMT_18BIT_RGB666 0x66 // 18-bit RGB666 pixel format
#define CO5300_PIXFMT_24BIT_RGB888 0x77 // 24-bit RGB888 pixel format

/* Driver configuration structure */
struct co5300_config {
	const struct device* mspi_bus;
	struct mspi_dev_id dev_id;
	struct mspi_dev_cfg mspi_dev_config;
	const struct gpio_dt_spec te_gpio;
	const struct gpio_dt_spec reset_gpio;
	uint16_t height;
	uint16_t width;
    uint16_t x_offset;
    uint16_t y_offset;
	uint8_t pixel_format;
	bool write_only;
	uint8_t orientation;
	bool display_invert;
	bool color_invert;
	uint32_t sram_size;
	int max_buf_size;
};

/* Display data struct */
struct co5300_data {
	struct k_mutex lock;
	uint8_t bytes_per_pixel;
	enum display_pixel_format pixel_format;
	enum display_orientation orientation;
	struct mspi_dev_cfg dev_cfg;
	bool configured;
	struct gpio_callback te_gpio_cb_data;
#ifdef CONFIG_ELCO5300_STATS
	struct co5300_stats stats;
	struct k_spinlock stats_lock;
	bool post_pending;	/* co5300_stats_mark_post() waits for the next write */
	bool post_lifted;
	uint32_t post_stamp;
#endif
};

/* Transaction classes for the bench counters; OTHER is everything outside display_write(). */
enum co5300_stat_kind {
	CO5300_STAT_OTHER,
	CO5300_STAT_CASET,
	CO5300_STAT_PASET,
	CO5300_STAT_RAMWR,
	CO5300_STAT_PIXEL,
};
