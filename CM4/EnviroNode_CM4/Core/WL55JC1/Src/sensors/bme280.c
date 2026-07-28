/**
  ******************************************************************************
  * @file    bme280.c
  * @brief   BME280 I2C driver — skeleton (Phase 2 fills the compensation math).
  ******************************************************************************
  */
#include "sensors/bme280.h"

#define BME280_I2C_TIMEOUT_MS  (50u)

/* Read `len` bytes starting at `reg` (auto-increment). */
static env_status_t bme280_rd(bme280_t *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
  if (HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr7 << 1), &reg, 1, BME280_I2C_TIMEOUT_MS) != HAL_OK) return ENV_ERR;
  if (HAL_I2C_Master_Receive (dev->hi2c, (uint16_t)(dev->addr7 << 1), buf, len, BME280_I2C_TIMEOUT_MS) != HAL_OK) return ENV_ERR;
  return ENV_OK;
}

/* Write one register. */
static env_status_t bme280_wr(bme280_t *dev, uint8_t reg, uint8_t val)
{
  uint8_t b[2] = { reg, val };
  return (HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr7 << 1), b, 2, BME280_I2C_TIMEOUT_MS) == HAL_OK) ? ENV_OK : ENV_ERR;
}

env_status_t bme280_init(bme280_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
  if (dev == NULL || hi2c == NULL) return ENV_ERR;
  dev->hi2c = hi2c; dev->addr7 = addr7; dev->ready = 0u;

  uint8_t id = 0;
  if (bme280_rd(dev, BME280_REG_ID, &id, 1) != ENV_OK) return ENV_ERR;
  if (id != BME280_CHIP_ID) return ENV_ERR;

  /* TODO(Phase2):
   *  - soft reset (write 0xB6 to REG_RESET), wait ~2 ms
   *  - read calibration blocks CALIB00 (0x88..0xA1) and CALIB26 (0xE1..0xF0)
   *    into dig_T*/dig_P*/dig_H* (mind the signed/unsigned + split H4/H5 nibbles)
   *  - CTRL_HUM = osrs_h x1 (0x01); CTRL_MEAS = osrs_t x1 | osrs_p x1 | sleep;
   *    CONFIG = filter off. Use FORCED mode per read to minimise power.
   */
  (void)bme280_wr; /* silence unused until Phase 2 uses it */
  dev->ready = 1u;
  return ENV_NOTIMPL;
}

env_status_t bme280_read(bme280_t *dev, float *t_c, float *rh_pct, float *p_hpa)
{
  if (dev == NULL || !dev->ready || t_c == NULL || rh_pct == NULL || p_hpa == NULL) return ENV_ERR;

  /* TODO(Phase2):
   *  - kick a FORCED measurement (CTRL_MEAS mode=0b01), poll STATUS.measuring
   *  - burst-read 8 bytes from REG_DATA -> raw press(20b), temp(20b), hum(16b)
   *  - apply Bosch compensation (t_fine chain) -> *t_c, *p_hpa (Pa/100), *rh_pct
   */
  *t_c = 0.0f; *rh_pct = 0.0f; *p_hpa = 0.0f;
  return ENV_NOTIMPL;
}
