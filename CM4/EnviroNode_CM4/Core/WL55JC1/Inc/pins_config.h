#pragma once
#include "main.h"   // gives you *_Pin and *_GPIO_Port from Cube labels

/* --- ADC & divider parameters --- */
#define VBAT_RTOP_OHMS      (56060.0f)
#define VBAT_RBOT_OHMS      (14711.0f)
#define VBAT_DIVIDER_GAIN   ((VBAT_RTOP_OHMS + VBAT_RBOT_OHMS) / VBAT_RBOT_OHMS)  /* ~4.748 */
#define ADC_VREF_VOLT       (3.3f)
#define ADC_FULL_SCALE      (4095.0f)

/* --- Battery: 1S Li-ion, two 3.7 V / 6600 mAh cells in PARALLEL (2P) ------
   Parallel keeps the pack at cell voltage: 4.2 V full, ~3.0 V empty. The old
   4S-LiFePO4 thresholds (14.3/13.4/12.8/11.2) came from KoreroNet's pack and
   were wrong for this hardware — fitted 2026-08-17, LOGBOOK r22. */
#define VBAT_MAX_CHARGE_V   (4.25f)
#define VBAT_FULL_REST_V    (4.15f)
#define VBAT_WARN_REST_V    (3.50f)
#define VBAT_CUTOFF_LOAD_V  (3.00f)

/* --- INA219 + battery --- */
#define INA219_I2C_ADDR_7B  (0x45)             /* change to 0x40 if needed */
#define INA219_ADDR         (INA219_I2C_ADDR_7B << 1)
#define INA_REG_CONFIG      (0x00)
#define INA_REG_SHUNT_V     (0x01)
#define INA_REG_BUS_V       (0x02)
#define SHUNT_OHMS          (0.1f)

/* --- Battery capacity & charge detection (moved from scattered #defines) --- */
#define BATTERY_NOMINAL_mAh          (13200.0f)  /* 2 x 6600 mAh in parallel */
#define CHARGE_VOLTAGE_THRESHOLD_V   (4.15f)     /* voltage considered "full" */
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

/* Anemometer (wind speed) — Arduino **A4** (PB14) = **ADC_IN1**, sampled.
   The contact is read by bursting the ADC at ~1 kHz for a few seconds and
   counting transitions (analog_sensors.c), NOT by an EXTI interrupt. Edge
   counting would pin the core awake for the whole interval — roughly ten times
   the energy of everything else on this sensor — whereas a burst lets the node
   sleep. The trade is temporal coverage: a burst sees a few seconds of wind per
   cycle rather than integrating the whole interval (docs/SENSORS.md §9).
   A4 sits next to the wiper on A3, so the 7911's 4-wire cable still lands on one
   Grove socket. External 47 k pull-up to 3V3 at the connector. */

/* --- Switched sensor rail (VSENS) — Arduino D4 (PB5) ------------------------
   One GPIO gates the excitation of every *powered* analog sensor, so they draw
   nothing between measurements:

     VSENS feeds : Decagon LWS excitation (~4 mA)
                   Davis 7911 direction pot supply, yellow (165 uA @ 20 k)
                   any future powered analog probe (e.g. soil moisture)

   Grounds are NOT switched. That is the whole point of gating the HIGH side: a
   low-side switch would lift every sensor's ground by its Vce(sat) (0.1-0.2 V),
   and since these outputs are ground-referenced that offset lands straight in
   the ADC reading — ~150 counts, when the LWS wet threshold sits only ~3 % above
   its dry baseline. High-side switching keeps the analog reference clean.

   Polarity depends on the part fitted — flip this one define, nothing else:
     1 = active HIGH : NPN driving a PNP/P-MOS high-side switch (GPIO high = on)
     0 = active LOW  : P-MOSFET driven straight from the GPIO (GPIO low = on)
   See docs/PINOUT.md for the two circuits. */
#define ENV_SENSPWR_Port         GPIOB
#define ENV_SENSPWR_Pin          GPIO_PIN_5
#define ENV_SENSPWR_ACTIVE_HIGH  (1)

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
