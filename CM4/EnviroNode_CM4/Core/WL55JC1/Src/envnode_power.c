/**
  ******************************************************************************
  * @file    envnode_power.c
  * @brief   STOP2 sleep between measurement cycles, woken by the RTC.
  *
  *          See envnode_power.h for the three hazards this handles (watchdog,
  *          frozen tick, edge-counted sensors).
  *
  *          Dual-core note: on the STM32WL each core requests low power for
  *          itself, and the *system* only reaches Stop when BOTH cores have. CM0+
  *          is running the LoRaWAN stack and manages its own low-power state, so
  *          calling this stops the CM4 core regardless of what CM0+ is doing —
  *          the application core halts and the RTC brings it back either way.
  *          The deepest system-level saving therefore depends on CM0+ being idle
  *          too, which it is between RX windows.
  ******************************************************************************
  */
#include "envnode_power.h"
#include "envnode_config.h"
#include "envnode_sensorset.h"
#include "rtc.h"                 /* hrtc  */
#include "main.h"                /* SystemClock_Config, HAL */

/* SysTick is stopped in STOP2; uwTick is the HAL's millisecond counter and is
   advanced by hand on wake so every tick-based deadline stays honest. */
extern __IO uint32_t uwTick;

/* Defined in main.c. STOP2 leaves the device on MSI with HSI off, so the clock
   tree has to be rebuilt on every wake before any timed peripheral is used. */
extern void SystemClock_Config(void);

static uint8_t  s_enabled = 1u;      /*!< `nucleo sleep on|off`               */
static uint32_t s_total_asleep_s;    /*!< cumulative, for the console         */

void envnode_power_init(void)
{
  s_enabled = 1u;
  s_total_asleep_s = 0u;

  /* Make sure no stale wake-up timer is left armed from a previous run. */
  (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);

  /* HAL_RTCEx_SetWakeUpTimer_IT() enables the RTC's own interrupt and its EXTI
     line, but not the NVIC line — without this the core would enter STOP2 and
     never come out. */
  HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

void envnode_power_set_enabled(int enabled)
{
  s_enabled = enabled ? 1u : 0u;
}

int envnode_power_is_enabled(void)
{
  return (int)s_enabled;
}

int envnode_power_may_sleep(void)
{
  if (!s_enabled) return 0;
  /* Rain and wind-speed are EXTI edge-counted with millisecond timestamps —
     stopping the core would drop tips and corrupt the gust window. */
  return envnode_sensorset_requires_awake(envnode_config_get_sensor_mask()) ? 0 : 1;
}

uint32_t envnode_power_total_asleep_s(void)
{
  return s_total_asleep_s;
}

/**
 * @brief  One STOP2 nap of @p seconds (1..ENVNODE_SLEEP_CHUNK_S).
 * @retval 1 if the core actually stopped, 0 if the wake-up timer refused.
 *
 * The wake-up timer is clocked from ck_spre (1 Hz) so the counter is simply
 * "seconds - 1"; that keeps the arithmetic obvious and the resolution is far
 * finer than a measurement interval needs.
 */
static int power_stop2_for(uint32_t seconds)
{
  if (seconds == 0u) return 0;

  if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, (uint32_t)(seconds - 1u),
                                  RTC_WAKEUPCLOCK_CK_SPRE_16BITS, 0u) != HAL_OK) {
    return 0;                              /* caller falls back to staying awake */
  }

  /* Stop the tick so its interrupt cannot pre-empt the entry sequence, then
     stop the core. Execution resumes on the next line after the RTC fires. */
  HAL_SuspendTick();
  HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

  /* --- awake again ---------------------------------------------------------
     STOP2 leaves HSI off and the system running from MSI, so the clock tree has
     to be rebuilt before anything that cares about timing (UART baud, I2C,
     SPI) is used again. */
  SystemClock_Config();
  HAL_ResumeTick();

  (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
  return 1;
}

uint32_t envnode_power_sleep_seconds(uint32_t seconds)
{
  if (seconds < ENVNODE_SLEEP_MIN_S) return 0u;

  uint32_t slept = 0u;

  while (slept < seconds) {
    uint32_t chunk = seconds - slept;
    if (chunk > ENVNODE_SLEEP_CHUNK_S) chunk = ENVNODE_SLEEP_CHUNK_S;

    /* Refresh the watchdog immediately before the nap: the IWDG keeps running
       in STOP2, so each chunk must fit inside its timeout. */
    IWDG->KR = 0x0000AAAAu;

    if (!power_stop2_for(chunk)) break;    /* timer refused — stop trying */

    slept += chunk;
    /* Give the rest of the firmware its time back: every deadline in this
       codebase is measured in HAL ticks. */
    uwTick += (chunk * 1000u);
    IWDG->KR = 0x0000AAAAu;
  }

  s_total_asleep_s += slept;
  return slept;
}
