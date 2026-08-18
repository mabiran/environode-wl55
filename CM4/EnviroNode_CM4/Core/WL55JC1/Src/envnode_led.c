/**
  ******************************************************************************
  * @file    envnode_led.c
  * @brief   Status LED pattern generator (see header for the blink language).
  *
  *          Pure polling, no timers, no interrupts: envnode_led_tick() derives
  *          the pin level from HAL_GetTick() alone, so the whole module is one
  *          GPIO write per main-loop pass and vanishes entirely in STOP2.
  ******************************************************************************
  */
#include "envnode_led.h"
#include "stm32wlxx_hal.h"

#define ENV_PWRLED_Port         GPIOB
#define ENV_PWRLED_Pin          GPIO_PIN_10     /* Arduino D6 */
#define ENV_PWRLED_ACTIVE_HIGH  (1)

/* Beat geometry: N flashes of 70 ms spaced 180 ms apart, then dark until the
   2 s beat rolls over. Duty stays under 11 % even at three flashes, so the
   LED costs ~0.2 mA average while awake against a ~2 mA lit current. */
#define LED_BEAT_MS    (2000u)
#define LED_ON_MS      (70u)
#define LED_SPACE_MS   (180u)
#define LED_PULSE_MS   (120u)

static envled_mode_t s_mode = ENVLED_BOOT;
static uint32_t      s_pulse_until;   /* TX pulse override, 0 = none */
static uint8_t       s_ready;

static void led_write(int on)
{
#if ENV_PWRLED_ACTIVE_HIGH
  HAL_GPIO_WritePin(ENV_PWRLED_Port, ENV_PWRLED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(ENV_PWRLED_Port, ENV_PWRLED_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

void envnode_led_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOB_CLK_ENABLE();
  g.Pin   = ENV_PWRLED_Pin;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ENV_PWRLED_Port, &g);
  led_write(0);
  s_ready = 1u;
}

void envnode_led_set_mode(envled_mode_t m) { s_mode = m; }

void envnode_led_pulse(void)
{
  s_pulse_until = HAL_GetTick() + LED_PULSE_MS;
  if (s_ready) led_write(1);
}

void envnode_led_off(void)
{
  s_pulse_until = 0u;
  if (s_ready) led_write(0);
}

void envnode_led_tick(void)
{
  if (!s_ready) return;
  uint32_t now = HAL_GetTick();

  if (s_pulse_until != 0u) {              /* TX pulse outranks the pattern */
    if ((int32_t)(s_pulse_until - now) > 0) { led_write(1); return; }
    s_pulse_until = 0u;
  }

  if (s_mode == ENVLED_BOOT) { led_write(1); return; }

  const uint32_t flashes = (s_mode == ENVLED_OK)     ? 1u
                         : (s_mode == ENVLED_NOJOIN) ? 2u
                         :                             3u;   /* ENVLED_FAULT */
  const uint32_t phase = now % LED_BEAT_MS;
  const uint32_t slot  = phase / LED_SPACE_MS;
  led_write(slot < flashes && (phase % LED_SPACE_MS) < LED_ON_MS);
}
