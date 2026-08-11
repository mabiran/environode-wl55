/**
  ******************************************************************************
  * @file    envnode_log.h
  * @brief   Offline sensor log — every measurement, timestamped, in flash.
  *
  *          A flash ring in pages 55–61 (14 KB, reserved by the linker) records
  *          the exact 30-byte frame each cycle produced, stamped with the RTC.
  *          It fills oldest-first and, when full, erases the oldest page and
  *          keeps going — the node never stops logging, it forgets the oldest
  *          ~14 % instead.
  *
  *          Capacity: 40-byte records, 51 per page, 7 pages = **357 records**.
  *          At the 15-minute field interval that is ~3.7 days of history; at the
  *          1-minute bench interval, ~6 hours.
  *
  *          Reading it out: `nucleo log dump [n]` prints CSV on the ST-LINK
  *          console — same USB cable that powers the board. (The WL55 has no
  *          USB peripheral, so the node cannot present its log as a USB drive
  *          and cannot host a memory stick; the drive that appears when the
  *          board is plugged in belongs to the ST-LINK programmer. The
  *          removable-media option is an SD card on SPI1 — docs/LOGBOOK.md.)
  *
  *          Timestamps are seconds since 2000-01-01 from the RTC. **They are
  *          only meaningful once the clock is set** (`nucleo time is …`); an
  *          unset RTC starts at its power-on default, so records still order
  *          correctly but carry the wrong absolute date.
  ******************************************************************************
  */
#ifndef ENVNODE_LOG_H
#define ENVNODE_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define ENVLOG_FRAME_LEN   (30u)   /*!< the FPort-1 frame, verbatim            */
#define ENVLOG_CAPACITY    (357u)  /*!< records before the oldest page recycles */

/**
 * @brief  Scan the ring and position the write head. Call once at boot.
 * @return number of valid records currently stored.
 */
uint32_t envnode_log_init(void);

/**
 * @brief  Append one measurement.
 *
 * Costs five double-word programs (~0.5 ms); every 51st append also erases the
 * next page (~20–40 ms, stalls CM0+ fetches). Call it from the sample path
 * BEFORE the uplink is posted, while the radio is idle — never inside an RX
 * window.
 *
 * @param  epoch2000  RTC seconds since 2000-01-01 (rtc_now_epoch2000()).
 * @param  frame      the packed 30-byte FPort-1 frame.
 * @retval 1 on success, 0 on a flash error.
 */
int envnode_log_append(uint32_t epoch2000, const uint8_t frame[ENVLOG_FRAME_LEN]);

/** @brief Number of valid records stored right now. */
uint32_t envnode_log_count(void);

/**
 * @brief  Fetch the @p back-th newest record (0 = newest).
 * @retval 1 and fills the outputs, or 0 if no such record.
 */
int envnode_log_get(uint32_t back, uint32_t *epoch2000, uint8_t frame[ENVLOG_FRAME_LEN]);

/** @brief Erase the whole log (console `nucleo log erase`). 1 = success. */
int envnode_log_erase_all(void);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_LOG_H */
