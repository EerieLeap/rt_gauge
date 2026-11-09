/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/display.h>
#include <zephyr/spinlock.h>
#ifdef CONFIG_ELCO5300_STATS
#include <zephyr/drivers/display/display_co5300.h>
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
#define CO5300_MADCTL_MY 0x80 // Row address order (vertical flip)
#define CO5300_MADCTL_MX 0x40 // Column address order (horizontal flip)

// Color Order
#define CO5300_MADCTL_RGB 0x00                      // Red-Green-Blue pixel order
#define CO5300_MADCTL_BGR 0x08                      // Blue-Green-Red pixel order

// Pixel Format
#define CO5300_PIXFMT_8BIT_GRAY256 0x11 // 8-bit GRAY256 pixel format
#define CO5300_PIXFMT_16BIT_RGB565 0x55 // 16-bit RGB565 pixel format
#define CO5300_PIXFMT_18BIT_RGB666 0x66 // 18-bit RGB666 pixel format
#define CO5300_PIXFMT_24BIT_RGB888 0x77 // 24-bit RGB888 pixel format

#define CO5300_TE_TIMEOUT_MS 20  // a bit over one 60 Hz scan
#define CO5300_TE_MAX_TIMEOUTS 5 // consecutive timeouts before frames stop waiting

/* Driver configuration structure */
struct co5300_config {
	const struct device* mspi_bus;
	struct mspi_dev_id dev_id;
	const struct gpio_dt_spec te_gpio;
	const struct gpio_dt_spec reset_gpio;
	uint16_t height;
	uint16_t width;
    uint16_t x_offset;
    uint16_t y_offset;
	uint8_t pixel_format;
	uint8_t orientation;
};

#ifdef CONFIG_ELCO5300_TE_SYNC
/* Intervals within this many percent of the estimate are accepted. */
#define CO5300_TE_ACCEPT_PCT 3
/* Consecutive agreeing intervals before the estimate counts as valid, and consecutive rejected
 * ones after which it is dropped and seeded again. */
#define CO5300_TE_SEED_COUNT 8
#define CO5300_TE_REJECT_LIMIT 8

/* The TE edge state. The ISR writes it and threads read it, always under lock: the 64-bit stamps
 * are not atomic on Xtensa. */
struct co5300_te {
	struct k_spinlock lock;
	bool have_level;      /* level is the previous sample's */
	bool level;
	bool have_fall;       /* fall_cycles is a falling edge since the interrupt was (re)armed */
	bool have_rise;
	uint32_t fall_seq;    /* falling edges seen, counts scan starts */
	uint64_t fall_cycles; /* last falling edge (a scan starts) */
	uint64_t rise_cycles; /* last rising edge (the porch starts) */
	uint64_t s_cycles;    /* scan period estimate, EWMA of the falling-edge intervals */
	bool s_valid;
	uint8_t s_agree;      /* consecutive agreeing intervals while seeding */
	uint8_t s_rejects;    /* consecutive rejected intervals */
#ifdef CONFIG_ELCO5300_STATS
	uint32_t falls;
	uint32_t rises;
	uint32_t coalesced;
	uint32_t iv_count;
	uint64_t iv_min;
	uint64_t iv_max;
	uint32_t tvdl_count;
	uint64_t tvdl_sum;
	uint64_t tvdl_max;
	uint32_t tvdh_count;
	uint64_t tvdh_sum;
	uint64_t tvdh_max;
	struct co5300_te_sample *cap_buf; /* the capture is armed while this is set */
	size_t cap_count;
	size_t cap_n;
	struct k_sem cap_sem;
#endif
};
#endif

/* Display data struct */
struct co5300_data {
	struct k_mutex lock; /* one operation (command sequence or whole write) at a time */
	uint8_t bytes_per_pixel;
	enum display_pixel_format pixel_format;
	enum display_orientation orientation;
	struct mspi_dev_cfg mspi_dev_config; /* current bus configuration, written only under lock */
	bool io_mode_valid;                  /* mspi_dev_config.io_mode is what the bus runs */
	bool caset_valid;                    /* caset_start/end are what the panel holds */
	uint16_t caset_start;                /* big-endian, as sent */
	uint16_t caset_end;
	bool configured;
	struct gpio_callback te_gpio_cb_data;
#ifdef CONFIG_ELCO5300_TE_SYNC
	const struct device *dev;       /* the ISR finds te-gpios through it */
	struct gpio_callback te_sync_cb_data;
	struct co5300_te te;            /* edge state shared by the ISR and threads, under te.lock */
	struct k_sem te_sem;            /* given by the TE edge that starts a scan, limit 1 */
	bool te_sync;                   /* TE sync is armed (te-gpios present and set up) */
	bool blanked;                   /* blanking_on() ran; no TE wait */
	bool frame_start;               /* the next write starts a frame (flush thread only) */
	volatile bool te_seen;          /* an edge came since the last time the wait looked */
	uint8_t te_timeouts_in_row;     /* flush thread only */
#endif
#ifdef CONFIG_ELCO5300_STATS
	struct co5300_stats stats;
	struct k_spinlock stats_lock;
	bool post_pending;	/* co5300_stats_mark_post() waits for the next write */
	bool post_lifted;
	uint32_t post_stamp;
#endif
};

/* Transaction classes for the bench counters; OTHER is everything outside display_write(). RAMWR is no longer sent. */
enum co5300_stat_kind {
	CO5300_STAT_OTHER,
	CO5300_STAT_CASET,
	CO5300_STAT_PASET,
	CO5300_STAT_RAMWR,
	CO5300_STAT_PIXEL,
};
