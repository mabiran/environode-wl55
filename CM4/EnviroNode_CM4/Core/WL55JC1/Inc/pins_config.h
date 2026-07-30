#pragma once
#include "main.h"   // gives you *_Pin and *_GPIO_Port from Cube labels

/* --- ADC & divider parameters --- */
#define VBAT_RTOP_OHMS      (56060.0f)
#define VBAT_RBOT_OHMS      (14711.0f)
#define VBAT_DIVIDER_GAIN   ((VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS)  /* ~4.748 */
#define ADC_VREF_VOLT       (3.3f)
#define ADC_FULL_SCALE      (4095.0f)

/* --- LiFePO4 (4S) voltage thresholds (resting/light load) --- */
#define VBAT_MAX_CHARGE_V   (14.3f)
#define VBAT_FULL_REST_V    (13.4f)
#define VBAT_WARN_REST_V    (12.8f)
#define VBAT_CUTOFF_LOAD_V  (11.2f)

/* --- INA219 + battery --- */
#define INA219_I2C_ADDR_7B  (0x45)             /* change to 0x40 if needed */
#define INA219_ADDR         (INA219_I2C_ADDR_7B << 1)
#define INA_REG_CONFIG      (0x00)
#define INA_REG_SHUNT_V     (0x01)
#define INA_REG_BUS_V       (0x02)
#define SHUNT_OHMS          (0.1f)

/* --- Battery capacity & charge detection (moved from scattered #defines) --- */
#define BATTERY_NOMINAL_mAh          (12000.0f)  /* pack nominal capacity */
#define CHARGE_VOLTAGE_THRESHOLD_V   (14.1f)    /* voltage considered “full” */
#define CHARGE_NEGATIVE_CURRENT_A    (-0.05f)    /* charging if I <= this */
#define CHARGE_CONFIRM_MS            (5000u)     /* must hold for this long */


/* ============================================================================
   EnviroNode-WL55 sensor pins  (ground truth: docs/PINOUT.md)
   NUCLEO-WL55JC1 + Seeed Grove Base Shield V2.

     Grove "D3" socket (D3+D4 = PB3+PB5) -> rain reed + anemometer reed
     Arduino SPI  (D13/D12/D11 + D10)    -> MAX31865 PT1000 front-end
     Arduino A0/A1/A3/A4                 -> leaf / soil / wind-vane / battery
       A0 = Decagon LWS leaf wetness (3-wire analog, ratiometric output)
     Grove I2C sockets (D14/D15)         -> I2C2: BME280 #1 + INA219
     Board pins PA9/PA10                 -> I2C1: BME280 #2
   ========================================================================== */

/* MAX31865 chip-select — Arduino D10 (PA4). Idle HIGH. */
#define ENV_RTD_CS_Port       GPIOA
#define ENV_RTD_CS_Pin        GPIO_PIN_4

/* Tipping-bucket rain gauge — Arduino D3 (PB3), EXTI3, pull-up, falling edge. */
#define ENV_RAIN_Port         GPIOB
#define ENV_RAIN_Pin          GPIO_PIN_3
#define ENV_RAIN_EXTI_IRQn    EXTI3_IRQn

/* Anemometer (wind speed) — Arduino **A4** (PB14), EXTI14, pull-up, falling edge.
   On an analog-capable pin on purpose: it puts the speed contact on A4 right next
   to the direction wiper on A3, so the Davis 7911's single 4-wire cable lands on
   ONE Grove socket (A3+A4 = wiper + contact, plus VCC and GND). A4 is used as a
   plain digital input here — no ADC involved. Moved from PB5/D4 on 2026-07-30. */
#define ENV_WIND_Port         GPIOB
#define ENV_WIND_Pin          GPIO_PIN_14
#define ENV_WIND_EXTI_IRQn    EXTI15_10_IRQn

/* --- Davis 7911 anemometer wiring (datasheet DS7911 Rev G) -----------------
     Yellow  pot supply voltage        -> 3V3
     Red     ground                    -> GND
     Green   direction pot wiper (20k) -> A3 = PB4 = ADC_IN3
     Black   speed contact to ground   -> A4 = PB14 = EXTI14 (digital, pull-up)
   Direction is linear: 0 ohm = 0 deg (north), 10 k = 180 deg (south), 20 k = 360.
   Speed: one contact closure per revolution, 1 Hz = 2.25 mph (see
   pulse_counter.h). Recommended external parts — no transistor needed:
     10 k  pull-up   A4 -> 3V3   (noise immunity over the 12 m cable)
     1 M   pull-down A3 -> GND   (defines the vane's dead band as north)
     100 nF          A4 -> GND   (optional RC debounce)                        */

/* PB10 (D6) and PC1 (D7) are FREE — the inherited Raspberry-Pi wake / 5V-enable
   lines that used to own them are gone (no Pi in this project). Left unlisted on
   purpose so a future sensor claims them via docs/PINOUT.md, not by accident. */

/* --- Status LED (external) — free Grove "D8" socket, pin PC2 = Arduino D8 ---
   HIGH = LED on. The firmware emits brief, low-duty pulses encoding system state
   (Status_Led_Tick in main.c) so the LED is on only ~30 ms per pulse => minimal
   power. NOTE the Grove "D8" socket carries D8+D9 = PC2+PA9, and PA9 is I2C1 SCL
   for BME280 #2 (docs/PINOUT.md) — the socket is NOT free for a two-wire Grove
   module, only for this single-pin LED. */
#define STATUS_LED_Port      GPIOC
#define STATUS_LED_Pin       GPIO_PIN_2
#define STATUS_LED_CHARGE_A  (-0.10f)   /* battery current <= this (A) => "charging" */
