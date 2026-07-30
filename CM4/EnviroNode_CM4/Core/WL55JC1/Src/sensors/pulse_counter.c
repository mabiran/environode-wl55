/**
  ******************************************************************************
  * @file    pulse_counter.c
  * @brief   Rain + wind-speed pulse counters.
  *
  *          pulse_rain_isr() / pulse_wind_isr() are called from
  *          HAL_GPIO_EXTI_Callback (stm32wlxx_it.c) for PB3 (rain, D3) and PB5
  *          (anemometer, D4). Both only touch volatile accumulators, so they
  *          stay short; the sample path snapshots and clears them inside a
  *          brief critical section so a pulse arriving mid-read is never lost
  *          twice or dropped.
  *
  *          Gust uses rolling GUST_WINDOW_MS buckets: each pulse either lands in
  *          the current bucket or closes it and starts a new one, and the
  *          fullest bucket of the interval is the gust. That tracks the WMO
  *          3-second definition and cannot be spiked by one bounced edge the
  *          way a "shortest gap" estimate can.
  *
  *          Timing note: intervals are measured with HAL_GetTick(), which stops
  *          in STOP mode. When the low-power scheduler lands (Phase 5), swap the
  *          two now_ms() call sites for an RTC-based timestamp.
  ******************************************************************************
  */
#include "sensors/pulse_counter.h"
#include "stm32wlxx_hal.h"   /* HAL_GetTick, __disable_irq/__enable_irq */

static volatile uint32_t s_rain_tips;
static volatile uint32_t s_wind_pulses;
static volatile uint32_t s_rain_last_ms;
static volatile uint32_t s_wind_last_ms;

/* Gust tracking: pulses inside the current GUST_WINDOW_MS bucket, plus the
   fullest bucket seen since the last read. */
static volatile uint32_t s_gust_bucket_start_ms;
static volatile uint32_t s_gust_bucket_count;
static volatile uint32_t s_gust_bucket_max;

static uint32_t s_interval_start_ms;

static inline uint32_t now_ms(void) { return HAL_GetTick(); }

/* Close the running gust bucket if it is older than the window. Called from the
   wind ISR and from the sample path (so a calm tail still gets accounted). */
static void gust_roll(uint32_t now)
{
  if ((now - s_gust_bucket_start_ms) >= GUST_WINDOW_MS) {
    if (s_gust_bucket_count > s_gust_bucket_max) s_gust_bucket_max = s_gust_bucket_count;
    s_gust_bucket_count    = 0u;
    s_gust_bucket_start_ms = now;
  }
}

void pulse_counter_init(void)
{
  uint32_t now = now_ms();
  s_rain_tips = 0u; s_wind_pulses = 0u;
  s_rain_last_ms = 0u; s_wind_last_ms = 0u;
  s_gust_bucket_start_ms = now;
  s_gust_bucket_count = 0u;
  s_gust_bucket_max = 0u;
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

void pulse_wind_isr(void)
{
  uint32_t now = now_ms();
  if ((uint32_t)(now - s_wind_last_ms) >= WIND_DEBOUNCE_MS) {  /* debounce */
    s_wind_pulses++;
    s_wind_last_ms = now;
    gust_roll(now);
    s_gust_bucket_count++;
  }
}

env_status_t pulse_read_and_reset(uint16_t *rain_tips, float *rain_mm, float *wind_speed_ms, float *wind_gust_ms)
{
  return pulse_read_stats(rain_tips, rain_mm, wind_speed_ms, wind_gust_ms, 1);
}

env_status_t pulse_read_stats(uint16_t *rain_tips, float *rain_mm,
                              float *wind_speed_ms, float *wind_gust_ms, int reset)
{
  if (rain_tips == NULL || rain_mm == NULL || wind_speed_ms == NULL || wind_gust_ms == NULL) return ENV_ERR;

  uint32_t now = now_ms();

  /* Snapshot (and optionally clear) atomically: an edge landing here must not be
     counted in both this interval and the next. */
  uint32_t tips, pulses, gust_max;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  gust_roll(now);                                   /* fold the tail bucket in */
  if (s_gust_bucket_count > s_gust_bucket_max) s_gust_bucket_max = s_gust_bucket_count;
  tips     = s_rain_tips;
  pulses   = s_wind_pulses;
  gust_max = s_gust_bucket_max;
  if (reset) {
    s_rain_tips = 0u; s_wind_pulses = 0u;
    s_gust_bucket_max = 0u; s_gust_bucket_count = 0u; s_gust_bucket_start_ms = now;
  }
  __set_PRIMASK(primask);

  float interval_s = (float)(uint32_t)(now - s_interval_start_ms) / 1000.0f;
  if (interval_s < 0.001f) interval_s = 0.001f;       /* guard div-by-zero */
  if (reset) s_interval_start_ms = now;

  *rain_tips     = (uint16_t)(tips > 0xFFFFu ? 0xFFFFu : tips);
  *rain_mm       = (float)tips * RAIN_MM_PER_TIP;
  *wind_speed_ms = ((float)pulses / interval_s) * ANEMO_MS_PER_HZ;
  *wind_gust_ms  = ((float)gust_max / ((float)GUST_WINDOW_MS / 1000.0f)) * ANEMO_MS_PER_HZ;

  /* A gust can never be slower than the interval mean (it contains it). */
  if (*wind_gust_ms < *wind_speed_ms) *wind_gust_ms = *wind_speed_ms;

  return ENV_OK;
}

void pulse_reset_rain(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_rain_tips = 0u;
  __set_PRIMASK(primask);
}

void pulse_peek(uint16_t *rain_tips, uint32_t *wind_pulses)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t t = s_rain_tips;
  uint32_t p = s_wind_pulses;
  __set_PRIMASK(primask);

  if (rain_tips)   *rain_tips   = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
  if (wind_pulses) *wind_pulses = p;
}
