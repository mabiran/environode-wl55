/**
  ******************************************************************************
  * @file    pulse_counter.c
  * @brief   Rain tipping-bucket pulse counter.
  *
  *          pulse_rain_isr() is called from HAL_GPIO_EXTI_Callback
  *          (stm32wlxx_it.c) for PB3 (Arduino D3). It only touches volatile
  *          accumulators, so it stays short; the sample path snapshots and
  *          clears them inside a brief critical section so a tip arriving
  *          mid-read is never counted twice or dropped.
  *
  *          **Wind speed used to live here too.** It moved to ADC burst sampling
  *          (analog_sensors.c) on 2026-08-04: edge counting pinned the core awake
  *          for the whole interval, which cost far more energy than the sensor
  *          itself. Rain stays on EXTI because tips are rare, unpredictable, and
  *          must never be missed — a burst would simply not see them.
  *
  *          Timing note: intervals are measured with HAL_GetTick(). That freezes
  *          in STOP2, but envnode_power.c adds the slept time back to the HAL
  *          tick on wake, so the interval arithmetic below stays correct.
  ******************************************************************************
  */
#include "sensors/pulse_counter.h"
#include "stm32wlxx_hal.h"   /* HAL_GetTick, __disable_irq/__enable_irq */

static volatile uint32_t s_rain_tips;
static volatile uint32_t s_rain_last_ms;
static uint32_t          s_interval_start_ms;

static inline uint32_t now_ms(void) { return HAL_GetTick(); }

void pulse_counter_init(void)
{
  uint32_t now = now_ms();
  s_rain_tips = 0u;
  s_rain_last_ms = 0u;
  s_interval_start_ms = now;
}

void pulse_rain_isr(void)
{
  uint32_t now = now_ms();
  if ((uint32_t)(now - s_rain_last_ms) >= RAIN_DEBOUNCE_MS) {  /* debounce */
    s_rain_tips++;
    s_rain_last_ms = now;
  }
}

env_status_t pulse_read_and_reset(uint16_t *rain_tips, float *rain_mm)
{
  return pulse_read_stats(rain_tips, rain_mm, 1);
}

env_status_t pulse_read_stats(uint16_t *rain_tips, float *rain_mm, int reset)
{
  if (rain_tips == NULL || rain_mm == NULL) return ENV_ERR;

  uint32_t now = now_ms();

  /* Snapshot (and optionally clear) atomically: a tip landing here must not be
     counted in both this interval and the next. */
  uint32_t tips;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  tips = s_rain_tips;
  if (reset) {
    s_rain_tips = 0u;
  }
  __set_PRIMASK(primask);

  if (reset) s_interval_start_ms = now;

  *rain_tips = (uint16_t)(tips > 0xFFFFu ? 0xFFFFu : tips);
  *rain_mm   = (float)tips * RAIN_MM_PER_TIP;

  return ENV_OK;
}

void pulse_reset_rain(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_rain_tips = 0u;
  __set_PRIMASK(primask);
}

void pulse_peek(uint16_t *rain_tips)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t t = s_rain_tips;
  __set_PRIMASK(primask);

  if (rain_tips) *rain_tips = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
}
