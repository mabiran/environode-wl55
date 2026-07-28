/**
  ******************************************************************************
  * @file    pulse_counter.h
  * @brief   Debounced pulse counters: rain (tipping bucket) + wind speed.
  *
  *          Both inputs are reed/hall contacts that bounce. ISRs (called from
  *          the EXTI callback) accumulate debounced pulses between intervals;
  *          pulse_read_and_reset() computes per-interval stats and rearms.
  *            rain_mm    = rain_tips * RAIN_MM_PER_TIP
  *            wind m/s   = (pulses / interval_s) * ANEMO_MS_PER_HZ
  *          Gust = the max instantaneous speed seen within the interval.
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
#define ANEMO_MS_PER_HZ    (0.34f)    /*!< e.g. 1 Hz ~= 2.4 km/h (Davis-like).  */
#define PULSE_DEBOUNCE_MS  (10u)      /*!< min inter-pulse time (reed bounce).  */

/**
 * @brief  Reset accumulators + timestamps. Call once at startup.
 */
void pulse_counter_init(void);

/** @brief  Rain tipping-bucket edge — call from the EXTI callback. Debounced. */
void pulse_rain_isr(void);

/** @brief  Anemometer edge — call from the EXTI callback. Debounced; tracks gust.*/
void pulse_wind_isr(void);

/**
 * @brief  Compute this interval's rain + wind stats and reset for the next one.
 * @param[out] rain_tips      tips since last reset
 * @param[out] rain_mm        rain_tips * RAIN_MM_PER_TIP
 * @param[out] wind_speed_ms  interval-average wind speed
 * @param[out] wind_gust_ms   peak instantaneous wind speed in the interval
 * @retval ENV_OK / ENV_ERR / ENV_NOTIMPL
 */
env_status_t pulse_read_and_reset(uint16_t *rain_tips, float *rain_mm, float *wind_speed_ms, float *wind_gust_ms);

#ifdef __cplusplus
}
#endif
#endif /* PULSE_COUNTER_H */
