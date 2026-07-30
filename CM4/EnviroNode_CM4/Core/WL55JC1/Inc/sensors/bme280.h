/**
  ******************************************************************************
  * @file    bme280.h
  * @brief   Bosch BME280 (temp / humidity / pressure) over I2C.
  *
  *          One driver instance per sensor. EnviroNode uses TWO BME280s, one on
  *          I2C1 and one on I2C2 (a second bus avoids the shared-address clash) —
  *          each gets its own bme280_t bound to its I2C_HandleTypeDef + address.
  *          Fills air1_* / air2_* in sensor_readings_t via the umbrella.
  *
  *          Power profile: the chip is parked in sleep mode and each read kicks
  *          a single FORCED measurement (osrs x1, IIR filter off), which is the
  *          Bosch-recommended "weather station" setting — ~1 uA average.
  ******************************************************************************
  */
#ifndef BME280_H
#define BME280_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32wlxx_hal.h"
#include "sensors/envnode_sensors.h"   /* env_status_t */

/* 7-bit I2C addresses (SDO low / high). */
#define BME280_ADDR_PRIMARY    (0x76u)
#define BME280_ADDR_SECONDARY  (0x77u)

/* Register map (subset). */
#define BME280_REG_ID          (0xD0u)   /*!< chip id: expect 0x60             */
#define BME280_REG_RESET       (0xE0u)   /*!< write 0xB6 to soft-reset         */
#define BME280_REG_CTRL_HUM    (0xF2u)   /*!< osrs_h                           */
#define BME280_REG_STATUS      (0xF3u)
#define BME280_REG_CTRL_MEAS   (0xF4u)   /*!< osrs_t | osrs_p | mode           */
#define BME280_REG_CONFIG      (0xF5u)   /*!< t_sb | filter                    */
#define BME280_REG_CALIB00     (0x88u)   /*!< dig_T/P (0x88..0xA1)             */
#define BME280_REG_CALIB26     (0xE1u)   /*!< dig_H   (0xE1..0xF0)             */
#define BME280_REG_DATA        (0xF7u)   /*!< press(3) temp(3) hum(2), burst   */
#define BME280_CHIP_ID         (0x60u)
#define BME280_RESET_WORD      (0xB6u)

/* Oversampling x1 on all three channels + forced mode (Bosch "weather"). */
#define BME280_CTRL_HUM_X1     (0x01u)                 /*!< osrs_h = x1        */
#define BME280_CTRL_MEAS_FORCE (0x25u)                 /*!< osrs_t/p x1, forced*/
#define BME280_CONFIG_FILTOFF  (0x00u)                 /*!< t_sb 0.5ms, no IIR */
#define BME280_STATUS_MEASURING (0x08u)                /*!< bit3 of STATUS     */

/** @brief Per-sensor context: bus, address, and on-chip compensation params. */
typedef struct {
  I2C_HandleTypeDef *hi2c;   /*!< bound I2C bus (hi2c1 / hi2c2).               */
  uint8_t            addr7;  /*!< 7-bit address (0x76/0x77).                   */
  uint8_t            ready;  /*!< 1 once init read the chip id + calibration.  */
  uint8_t            chip_id;/*!< last chip id read (0x60 = BME280) — debug.   */

  /* Factory compensation coefficients (datasheet 4.2.2). */
  uint16_t dig_T1;
  int16_t  dig_T2, dig_T3;
  uint16_t dig_P1;
  int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
  uint8_t  dig_H1, dig_H3;
  int16_t  dig_H2, dig_H4, dig_H5;
  int8_t   dig_H6;

  int32_t  t_fine;           /*!< temperature carry-over for P and H comp.     */
} bme280_t;

/**
 * @brief  Probe the chip, soft-reset, load calibration, and park it in sleep
 *         with the weather-station oversampling profile (x1, filter off).
 * @param  dev    context to initialise (zeroed on entry).
 * @param  hi2c   bound bus (&hi2c1 / &hi2c2).
 * @param  addr7  7-bit address (0x76 / 0x77).
 * @retval ENV_OK on success, ENV_ERR on bad argument / bus error / wrong id.
 */
env_status_t bme280_init(bme280_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7);

/**
 * @brief  Same as bme280_init(), but tries 0x76 then 0x77 — handy because the
 *         two breakout boards may be strapped differently.
 */
env_status_t bme280_init_autoaddr(bme280_t *dev, I2C_HandleTypeDef *hi2c);

/**
 * @brief  Trigger a forced measurement, read the raw ADC words, apply Bosch
 *         compensation, and return compensated values.
 * @param[out] t_c    temperature °C
 * @param[out] rh_pct relative humidity %
 * @param[out] p_hpa  pressure hPa
 * @retval ENV_OK / ENV_ERR (bus error, timeout, or implausible reading)
 */
env_status_t bme280_read(bme280_t *dev, float *t_c, float *rh_pct, float *p_hpa);

#ifdef __cplusplus
}
#endif
#endif /* BME280_H */
