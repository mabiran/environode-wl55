/**
  ******************************************************************************
  * @file    pulse_counter.c
  * @brief   Rain + wind-speed pulse counters — skeleton.
  *
  *          ISR-safe accumulators (volatile). Wire pulse_rain_isr /
  *          pulse_wind_isr into HAL_GPIO_EXTI_Callback for the rain + anemometer
  *          pins once they are assigned in CubeMX (docs/PINOUT.md).
  ******************************************************************************
  */
#include "sensors/pulse_counter.h"
#include "stm32wlxx_hal.h"   /* HAL_GetTick */

static volatile uint32_t s_rain_tips;
static volatile uint32_t s_wind_pulses;
static volatile uint32_t s_rain_last_ms;
static volatile uint32_t s_wind_last_ms;
static volatile uint32_t s_wind_min_dt_ms;  /* smallest gap -> peak gust        */
static uint32_t          s_interval_start_ms;

void pulse_counter_init(void)
{
  s_rain_tips = 0u; s_wind_pulses = 0u;
  s_rain_last_ms = 0u; s_wind_last_ms = 0u;
  s_wind_min_dt_ms = 0xFFFFFFFFu;
  s_interval_start_ms = HAL_GetTick();
}

void pulse_rain_isr(void)
{
  uint32_t now = HAL_GetTick();
  if ((now - s_rain_last_ms) >= PULSE_DEBOUNCE_MS) {  /* debounce */
    s_rain_tips++;
    s_rain_last_ms = now;
  }
}

void pulse_wind_isr(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t dt  = now - s_wind_last_ms;
  if (dt >= PULSE_DEBOUNCE_MS) {                       /* debounce */
    s_wind_pulses++;
    if (dt < s_wind_min_dt_ms) s_wind_min_dt_ms = dt;  /* track fastest -> gust */
    s_wind_last_ms = now;
  }
}

env_status_t pulse_read_and_reset(uint16_t *rain_tips, float *rain_mm, float *wind_speed_ms, float *wind_gust_ms)
{
  if (rain_tips == NULL || rain_mm == NULL || wind_speed_ms == NULL || wind_gust_ms == NULL) return ENV_ERR;

  uint32_t now = HAL_GetTick();
  float interval_s = (float)(now - s_interval_start_ms) / 1000.0f;
  if (interval_s < 0.001f) interval_s = 0.001f;       /* guard div-by-zero */

  /* Snapshot + reset (brief critical section recommended in Phase 2). */
  uint32_t tips   = s_rain_tips;
  uint32_t pulses = s_wind_pulses;
  uint32_t min_dt = s_wind_min_dt_ms;
  s_rain_tips = 0u; s_wind_pulses = 0u; s_wind_min_dt_ms = 0xFFFFFFFFu;
  s_interval_start_ms = now;

  *rain_tips     = (uint16_t)(tips > 0xFFFFu ? 0xFFFFu : tips);
  *rain_mm       = (float)tips * RAIN_MM_PER_TIP;
  *wind_speed_ms = ((float)pulses / interval_s) * ANEMO_MS_PER_HZ;
  *wind_gust_ms  = (min_dt != 0xFFFFFFFFu && min_dt > 0u)
                     ? (1000.0f / (float)min_dt) * ANEMO_MS_PER_HZ
                     : 0.0f;
  return ENV_OK;   /* pulse block is real (no external chip to stub) */
}
