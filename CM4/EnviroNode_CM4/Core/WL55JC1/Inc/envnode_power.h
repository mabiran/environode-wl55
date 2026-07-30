/**
  ******************************************************************************
  * @file    envnode_power.h
  * @brief   Low-power sleep between measurement cycles (STOP2 + RTC wake-up).
  *
  *          The node spends almost all of its life waiting for the next cycle.
  *          This module puts the CM4 application core into STOP2 for that wait
  *          and brings it back with the RTC wake-up timer, which runs from the
  *          LSE and keeps counting while the core is stopped.
  *
  *          Three things make this safe rather than merely small:
  *
  *          1. **The watchdog still runs in STOP2.** The IWDG is clocked by the
  *             LSI and cannot be stopped once started, so a sleep longer than
  *             its ~15 s timeout would reset the node. Sleep is therefore taken
  *             in chunks of at most ENVNODE_SLEEP_CHUNK_S, refreshing the
  *             watchdog between chunks — the node still gets watchdog cover
  *             while it sleeps.
  *
  *          2. **HAL_GetTick() freezes in STOP2**, because SysTick stops with
  *             the core. Every deadline in this firmware is tick-based, so on
  *             wake the elapsed time is added back to the HAL tick. From the
  *             rest of the firmware's point of view time simply passed.
  *
  *          3. **Edge-counted sensors cannot sleep.** Rain (`R`) and wind speed
  *             (`WS`) are counted from GPIO interrupts with millisecond
  *             timestamps; stopping the core would drop tips and corrupt the
  *             gust window. Selecting either keeps the node awake, and
  *             envnode_power_may_sleep() reports that.
  *
  *          The console is not serviced while the core is stopped. Each cycle
  *          therefore keeps an awake window (see main.c) long enough to catch
  *          the LoRaWAN RX windows, drain downlinks and accept a typed command,
  *          and `nucleo sleep off` disables sleeping altogether for bench work.
  ******************************************************************************
  */
#ifndef ENVNODE_POWER_H
#define ENVNODE_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Longest single STOP2 nap, in seconds. Must stay comfortably under the IWDG
 *  timeout (~15 s at /256 with RLR 1875) — the watchdog is refreshed between
 *  chunks, not during one. */
#define ENVNODE_SLEEP_CHUNK_S    (8u)

/** Shortest wait worth stopping the core for. Below this the wake-up overhead
 *  (clock restart, ~1 ms) is a bigger cost than the saving. */
#define ENVNODE_SLEEP_MIN_S      (2u)

/**
 * @brief  Prepare the sleep subsystem. Call once after MX_RTC_Init().
 */
void envnode_power_init(void);

/**
 * @brief  May the node stop the core between cycles?
 * @retval 1 if sleeping is enabled AND no edge-counted sensor is selected.
 */
int envnode_power_may_sleep(void);

/**
 * @brief  Runtime enable/disable (`nucleo sleep on|off`). Disabling is the
 *         bench-work setting: the console stays responsive continuously.
 */
void envnode_power_set_enabled(int enabled);
int  envnode_power_is_enabled(void);

/**
 * @brief  Stop the core for up to @p seconds, in watchdog-safe chunks.
 *
 * Returns early if @p seconds is below ENVNODE_SLEEP_MIN_S. On return the HAL
 * tick has been advanced by the time actually spent asleep, so tick-based
 * deadlines elsewhere stay correct.
 *
 * @param  seconds  requested sleep duration.
 * @return seconds actually slept.
 */
uint32_t envnode_power_sleep_seconds(uint32_t seconds);

/**
 * @brief  Total seconds spent in STOP2 since boot (console diagnostics).
 */
uint32_t envnode_power_total_asleep_s(void);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_POWER_H */
