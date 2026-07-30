/**
  ******************************************************************************
  * @file    pulse_counter.h
  * @brief   Debounced pulse counters: rain (tipping bucket) + wind speed.
  *
  *          Both inputs are reed contacts to GND (PB3 = rain / D3, PB5 = wind /
  *          D4) that bounce for milliseconds. The EXTI callbacks accumulate
  *          debounced pulses between samples; pulse_read_and_reset() turns them
  *          into per-interval statistics and rearms:
  *            rain_mm  = rain_tips * RAIN_MM_PER_TIP
  *            wind m/s = (pulses / interval_s) * ANEMO_MS_PER_HZ
  *          Gust is the strongest 3-second burst inside the interval — the
  *          conventional meteorological definition, and far more robust than
  *          "shortest gap seen", which a single bounce can spike.
  ******************************************************************************
  */
#ifndef PULSE_COUNTER_H
#define PULSE_COUNTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sensors/envnode_sensors.h"

/* Calibration — set to the specific gauges. */
#define RAIN_MM_PER_TIP    (0.2794f)  /*!< common 0.011" tipping bucket.        */

/**
 * Davis 7911 anemometer (datasheet DS7911 Rev G).
 *
 *   "V = P(2.25/T)"   V = speed in mph, P = pulses per sample period,
 *                     T = sample period in seconds
 *
 * so one closure per second is 2.25 mph. Cross-checks against the datasheet's
 * other statement, "1600 rev/hr = 1 mph"  ->  1600/3600 = 0.444 Hz per mph
 * ->  1 Hz = 2.25 mph. Converting once, here, in m/s:
 *
 *   2.25 mph x 0.44704 (m/s)/mph = 1.00584 m/s per Hz
 *
 * The old 0.34 value was a generic placeholder and under-read this sensor by 3x.
 */
#define ANEMO_MS_PER_HZ    (1.00584f) /*!< Davis 7911: 1 Hz = 2.25 mph.         */

/* Debounce windows: a bucket cannot physically tip twice within 100 ms, while
   the anemometer must still resolve fast rotation (5 ms => up to 200 Hz). */
#define RAIN_DEBOUNCE_MS   (100u)
#define WIND_DEBOUNCE_MS   (5u)
#define PULSE_DEBOUNCE_MS  (RAIN_DEBOUNCE_MS)  /*!< kept for older callers.     */

/** Gust window (ms) — WMO-style 3 s gust. */
#define GUST_WINDOW_MS     (3000u)

/**
 * @brief  Reset accumulators + timestamps. Call once at startup.
 */
void pulse_counter_init(void);

/** @brief  Rain tipping-bucket edge — call from the EXTI callback. Debounced. */
void pulse_rain_isr(void);

/** @brief  Anemometer edge — call from the EXTI callback. Debounced; feeds gust.*/
void pulse_wind_isr(void);

/**
 * @brief  Compute this interval's rain + wind stats and reset for the next one.
 * @param[out] rain_tips      tips since last reset
 * @param[out] rain_mm        rain_tips * RAIN_MM_PER_TIP
 * @param[out] wind_speed_ms  interval-average wind speed
 * @param[out] wind_gust_ms   peak 3-second wind speed within the interval
 * @retval ENV_OK / ENV_ERR (NULL argument)
 */
env_status_t pulse_read_and_reset(uint16_t *rain_tips, float *rain_mm, float *wind_speed_ms, float *wind_gust_ms);

/**
 * @brief  Same statistics, with the reset made optional.
 *
 * The uplink path consumes the interval (@p reset = 1); the console's `sensors`
 * command must NOT, or typing it would quietly steal rainfall from the next
 * frame. With @p reset = 0 the numbers are "so far this interval".
 */
env_status_t pulse_read_stats(uint16_t *rain_tips, float *rain_mm,
                              float *wind_speed_ms, float *wind_gust_ms, int reset);

/**
 * @brief  Zero the rain accumulator without touching the wind counters
 *         (downlink command 0x03 `reset_rain`).
 */
void pulse_reset_rain(void);

/**
 * @brief  Peek at the live counters without resetting them (console `sensors`).
 * @param[out] rain_tips   tips accumulated so far this interval
 * @param[out] wind_pulses anemometer pulses so far this interval
 */
void pulse_peek(uint16_t *rain_tips, uint32_t *wind_pulses);

#ifdef __cplusplus
}
#endif
#endif /* PULSE_COUNTER_H */
