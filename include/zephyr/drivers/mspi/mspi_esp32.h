/*
 * Copyright (c) 2026 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MSPI_MSPI_ESP32_H_
#define ZEPHYR_INCLUDE_DRIVERS_MSPI_MSPI_ESP32_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_MSPI_ESP32_STATS
/** @brief Bench counters of one ESP32 MSPI controller (CONFIG_MSPI_ESP32_STATS). */
struct mspi_esp32_stats {
	uint32_t transactions;         /* hardware transactions started */
	uint32_t dma_transactions;     /* ... through GDMA */
	uint32_t reg_transactions;     /* ... through the SPI data registers, no DMA */
	uint32_t short_transactions;   /* ... whose data phase is at most 64 B (commands and their parameters) */
	uint32_t bounce_copies;        /* copies into the bounce buffer */
	uint64_t bounce_bytes;
	uint64_t bounce_cycles;        /* cycles spent copying */
	uint64_t wait_cycles;          /* cycles from a transaction's start to its completion, summed */
	uint32_t timeouts;
	uint32_t gaps;                 /* transaction starts that followed a transaction which left CE asserted */
	uint64_t gap_cycles;           /* end of the earlier one to the start of the next, summed */
	uint32_t gap_cycles_max;
	uint32_t psram_transactions;   /* DMA transactions read straight from PSRAM (CONFIG_MSPI_ESP32_PSRAM_DMA) */
	uint32_t psram_underruns;      /* ... of which the TX FIFO ran dry (real ones only); each failed its transfer with -EIO */
	uint32_t injected_faults;      /* pieces failed on purpose (CONFIG_MSPI_ESP32_FAULT_INJECT_PERIOD), not in psram_underruns */
};

/** @brief Consistent snapshot of the counters. */
void mspi_esp32_stats_get(const struct device *controller, struct mspi_esp32_stats *out);

/** @brief Zero the counters (and drop a pending gap stamp). */
void mspi_esp32_stats_reset(const struct device *controller);
#endif /* CONFIG_MSPI_ESP32_STATS */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MSPI_MSPI_ESP32_H_ */
