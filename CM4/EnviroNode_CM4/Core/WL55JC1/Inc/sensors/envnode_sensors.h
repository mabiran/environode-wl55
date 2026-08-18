/**
  ******************************************************************************
  * @file    envnode_sensors.h
  * @brief   EnviroNode-WL55 sensor subsystem — umbrella header.
  *
  *          Defines the shared sensor-reading struct, the per-sensor status
  *          bitfield, the module return-status enum, and the top-level
  *          init/sample API. The application includes this one header to
  *          acquire a full sensor frame; envnode_sensors_sample() fans out to
  *          the individual drivers listed in the module map and merges their
  *          results (plus per-sensor OK bits) into one sensor_readings_t.
  ******************************************************************************
  * @attention
  *   Ground truth: docs/PINOUT.md (sensor -> interface map) and
  *   docs/PAYLOAD.md (on-air encoding). Do not contradict them.
  *
  *   Copyright (c) 2026. Licensed AS-IS; see the project LICENSE file.
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef ENVNODE_SENSORS_H
#define ENVNODE_SENSORS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

/*
 * Module map — sensor subsystem
 * -----------------------------
 *   envnode_sensors.{h,c}   this file: shared reading struct + top-level API
 *   envnode_payload.{h,c}   scaled LoRaWAN frame packer / downlink dispatcher
 *   bme280.{h,c}            2x BME280 air T/RH/press   (I2C1 -> hi2c1,
 *                                                       I2C2 -> hi2c2)
 *   max31865.{h,c}          PT1000 RTD soil temp        (SPI1 -> hspi1 + CS)
 *   analog_sensors.{h,c}    soil-moist / leaf / battery / wind-dir (ADC -> hadc)
 *   pulse_counter.{h,c}     rain tips + wind speed/gust (EXTI / TIM)
 *
 * NOTE (Phase 1): I2C1 / I2C2 / SPI1 / ADC are NOT yet configured in the .ioc.
 * Each driver must guard peripheral use and carry // TODO(Phase1/Phase2)
 * markers until CubeMX generates the extern handles (hi2c1, hi2c2, hspi1,
 * hadc, hrtc) exposed through "i2c.h" / "spi.h" / "adc.h".
 */

/* Exported types ------------------------------------------------------------*/

/**
 * @brief Module-wide return status.
 */
typedef enum {
  ENV_OK      = 0,   /*!< Operation completed successfully.                    */
  ENV_ERR     = 1,   /*!< Operation failed (bus error / timeout / bad arg).    */
  ENV_NOTIMPL = 2    /*!< Not implemented yet (skeleton — Phase 1/Phase 2).    */
} env_status_t;

/**
 * @brief One interval's worth of merged sensor readings.
 *
 * Physical/engineering units live here as floats for readability; the on-air
 * packer (see envnode_payload.h) does all scaling to fixed-width integers.
 * A sensor whose status OK-bit is clear should be treated as "no data" — the
 * packer substitutes the sentinels defined in docs/PAYLOAD.md.
 */
typedef struct {
  float    air1_temp_c, air1_rh_pct, air1_press_hpa;  // BME280 #1 on I2C1
  float    air2_temp_c, air2_rh_pct, air2_press_hpa;  // BME280 #2 on I2C2
  uint16_t soil_moist_raw;   // ADC counts (or permille after calibration)
  uint16_t leaf_wet_raw;     // ADC counts
  float    batt_v;           // volts (INA219 bus voltage; divider fallback)
  float    batt_i_a;         // amps, discharge positive (INA219)
  uint8_t  batt_i_ok;        // 1 = batt_i_a was actually measured this frame
  float    wind_dir_deg;     // 0..359.9 (vane potentiometer)
  float    soil_temp_c;      // PT1000 via MAX31865
  float    wind_speed_ms;    // interval average
  float    wind_gust_ms;     // interval max
  uint16_t rain_tips;        // tips this interval
  float    rain_mm;          // mm this interval (derived from tips)
  uint8_t  status;           // bit0 air1 ok, b1 air2 ok, b2 soil, b3 leaf, b4 pt1000, b5 wind, b6 rain, b7 fault
} sensor_readings_t;

/* Exported constants --------------------------------------------------------*/

/**
 * @name Status bitfield (bit set = sensor OK, except b7)
 * Mirrors the `status` byte at offset 1 of the uplink frame (docs/PAYLOAD.md).
 * @{
 */
#define SENS_OK_AIR1    (0x01u)  /*!< b0: BME280 #1 (air block A) OK.          */
#define SENS_OK_AIR2    (0x02u)  /*!< b1: BME280 #2 (air block B) OK.          */
#define SENS_OK_SOIL    (0x04u)  /*!< b2: soil-moisture channel OK.            */
#define SENS_OK_LEAF    (0x08u)  /*!< b3: leaf-wetness channel OK.             */
#define SENS_OK_PT1000  (0x10u)  /*!< b4: PT1000 / MAX31865 soil temp OK.      */
#define SENS_OK_WIND    (0x20u)  /*!< b5: wind speed/dir/gust OK.              */
#define SENS_OK_RAIN    (0x40u)  /*!< b6: rain tip counter OK.                 */
#define SENS_FAULT      (0x80u)  /*!< b7: fault — any sensor errored this frame*/
/** @} */

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  Initialise the sensor subsystem and all underlying drivers.
 *
 * Brings up the BME280 pair, MAX31865 RTD front-end, the ADC channels, and the
 * pulse counters. Intended to be called once after CubeMX peripheral init.
 *
 * @retval ENV_OK       all required drivers initialised.
 * @retval ENV_ERR      one or more drivers failed to initialise.
 * @retval ENV_NOTIMPL  skeleton — peripherals not configured yet (Phase 1).
 */
env_status_t envnode_sensors_init(void);

/**
 * @brief  Take one full sample across every sensor and merge into @p out.
 *
 * Each driver is polled in turn; a driver failure clears that sensor's OK-bit
 * and sets SENS_FAULT in @p out->status rather than aborting the whole frame.
 * On any path @p out->status reflects which fields are valid.
 *
 * @param[out] out  Destination reading struct (must be non-NULL).
 * @retval ENV_OK       at least the frame was populated (check per-sensor bits).
 * @retval ENV_ERR      @p out was NULL / unrecoverable error.
 * @retval ENV_NOTIMPL  skeleton — no drivers implemented yet (Phase 1/2).
 */
env_status_t envnode_sensors_sample(sensor_readings_t *out);

/**
 * @brief  Same as envnode_sensors_sample(), but leaves the rain/wind
 *         accumulators running — for the console's `sensors` command, which
 *         must not consume the interval the next uplink is going to report.
 */
env_status_t envnode_sensors_peek(sensor_readings_t *out);

/**
 * @brief  Bitmask of the drivers that came up at init (for the console's
 *         `nucleo sensors` diagnostic).
 *   b0 BME280 #1 (I2C2) · b1 BME280 #2 (I2C1) · b2 MAX31865 · b3 analog ·
 *   b4 pulse counters · b5 INA219
 */
uint8_t envnode_sensors_present(void);

/** 7-bit address the INA219 answered at during init (0x45/0x40), 0 if absent. */
uint8_t envnode_sensors_ina_addr(void);

#ifdef __cplusplus
}
#endif

#endif /* ENVNODE_SENSORS_H */
