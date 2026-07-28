/**
  ******************************************************************************
  * @file    bme280.h
  * @brief   Bosch BME280 (temp / humidity / pressure) over I2C.
  *
  *          One driver instance per sensor. EnviroNode uses TWO BME280s, one on
  *          I2C1 and one on I2C2 (a second bus avoids the shared-address clash) —
  *          each gets its own bme280_t bound to its I2C_HandleTypeDef + address.
  *          Fills air1_* / air2_* in sensor_readings_t via the umbrella.
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

/** @brief Per-sensor context: bus, address, and on-chip compensation params. */
typedef struct {
  I2C_HandleTypeDef *hi2c;   /*!< bound I2C bus (hi2c1 / hi2c2).               */
  uint8_t            addr7;  /*!< 7-bit address (0x76/0x77).                   */
  uint8_t            ready;  /*!< 1 once init read the chip id + calibration.  */
  /* TODO(Phase2): dig_T1..3, dig_P1..9, dig_H1..6 + int32 t_fine. */
} bme280_t;

/**
 * @brief  Probe the chip, soft-reset, load calibration, set an oversampling +
 *         IIR-filter profile suitable for a slow weather node (e.g. osrs x1,
 *         filter off, forced mode — sample-on-demand to save power).
 * @retval ENV_OK / ENV_ERR / ENV_NOTIMPL
 */
env_status_t bme280_init(bme280_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7);

/**
 * @brief  Trigger a forced measurement, read the raw ADC words, apply Bosch
 *         compensation, and return compensated values.
 * @param[out] t_c    temperature °C
 * @param[out] rh_pct relative humidity %
 * @param[out] p_hpa  pressure hPa
 */
env_status_t bme280_read(bme280_t *dev, float *t_c, float *rh_pct, float *p_hpa);

#ifdef __cplusplus
}
#endif
#endif /* BME280_H */
