/**
  ******************************************************************************
  * @file    bme280.c
  * @brief   BME280 I2C driver — chip probe, calibration load, forced-mode read
  *          and the Bosch compensation chain (datasheet BST-BME280-DS002, 4.2).
  *
  *          Floating-point compensation is used deliberately: the CM4 has an
  *          FPU, the node samples once every few minutes, and the float form is
  *          much easier to check against the datasheet than the fixed-point one.
  ******************************************************************************
  */
#include <string.h>
#include "sensors/bme280.h"

#define BME280_I2C_TIMEOUT_MS   (50u)
#define BME280_MEAS_TIMEOUT_MS  (100u)  /*!< x1/x1/x1 finishes in ~8 ms max.   */

/* Read `len` bytes starting at `reg` (chip auto-increments). */
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

/* --- little-endian helpers over the calibration blocks -------------------- */
static inline uint16_t u16le(const uint8_t *p) { return (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]); }
static inline int16_t  s16le(const uint8_t *p) { return (int16_t)u16le(p); }

/* Load dig_T*, dig_P* (0x88..0xA1) and dig_H* (0xE1..0xE7). The humidity block
   is the awkward one: H4/H5 are 12-bit values sharing the nibbles of 0xE5. */
static env_status_t bme280_load_calib(bme280_t *dev)
{
  uint8_t c[26];
  uint8_t h[7];

  if (bme280_rd(dev, BME280_REG_CALIB00, c, sizeof(c)) != ENV_OK) return ENV_ERR;
  if (bme280_rd(dev, BME280_REG_CALIB26, h, sizeof(h)) != ENV_OK) return ENV_ERR;

  dev->dig_T1 = u16le(&c[0]);
  dev->dig_T2 = s16le(&c[2]);
  dev->dig_T3 = s16le(&c[4]);
  dev->dig_P1 = u16le(&c[6]);
  dev->dig_P2 = s16le(&c[8]);
  dev->dig_P3 = s16le(&c[10]);
  dev->dig_P4 = s16le(&c[12]);
  dev->dig_P5 = s16le(&c[14]);
  dev->dig_P6 = s16le(&c[16]);
  dev->dig_P7 = s16le(&c[18]);
  dev->dig_P8 = s16le(&c[20]);
  dev->dig_P9 = s16le(&c[22]);
  /* c[24] is reserved (0xA0); dig_H1 lives at 0xA1. */
  dev->dig_H1 = c[25];

  dev->dig_H2 = s16le(&h[0]);                                                             /* 0xE1/0xE2      */
  dev->dig_H3 = h[2];                                                                     /* 0xE3           */
  dev->dig_H4 = (int16_t)(((int16_t)(int8_t)h[3] * 16) | (int16_t)(h[4] & 0x0Fu));         /* 0xE4/0xE5[3:0] */
  dev->dig_H5 = (int16_t)(((int16_t)(int8_t)h[5] * 16) | (int16_t)((h[4] >> 4) & 0x0Fu));  /* 0xE6/0xE5[7:4] */
  dev->dig_H6 = (int8_t)h[6];                                                             /* 0xE7           */

  /* dig_T1 / dig_P1 reading as 0 means the chip never answered properly. */
  if (dev->dig_T1 == 0u || dev->dig_P1 == 0u) return ENV_ERR;
  return ENV_OK;
}

/* --- Bosch compensation (float form, datasheet 4.2.3) --------------------- */
static float bme280_comp_t(bme280_t *dev, int32_t adc_T)
{
  float var1 = (((float)adc_T) / 16384.0f - ((float)dev->dig_T1) / 1024.0f) * ((float)dev->dig_T2);
  float var2 = ((float)adc_T) / 131072.0f - ((float)dev->dig_T1) / 8192.0f;
  var2 = (var2 * var2) * ((float)dev->dig_T3);
  dev->t_fine = (int32_t)(var1 + var2);
  return (var1 + var2) / 5120.0f;                       /* °C */
}

static float bme280_comp_p(bme280_t *dev, int32_t adc_P)
{
  float var1 = ((float)dev->t_fine / 2.0f) - 64000.0f;
  float var2 = var1 * var1 * ((float)dev->dig_P6) / 32768.0f;
  var2 = var2 + var1 * ((float)dev->dig_P5) * 2.0f;
  var2 = (var2 / 4.0f) + (((float)dev->dig_P4) * 65536.0f);
  var1 = (((float)dev->dig_P3) * var1 * var1 / 524288.0f + ((float)dev->dig_P2) * var1) / 524288.0f;
  var1 = (1.0f + var1 / 32768.0f) * ((float)dev->dig_P1);
  if (var1 == 0.0f) return 0.0f;                        /* avoid div-by-zero  */

  float p = 1048576.0f - (float)adc_P;
  p = (p - (var2 / 4096.0f)) * 6250.0f / var1;
  var1 = ((float)dev->dig_P9) * p * p / 2147483648.0f;
  var2 = p * ((float)dev->dig_P8) / 32768.0f;
  p = p + (var1 + var2 + ((float)dev->dig_P7)) / 16.0f;
  return p;                                             /* Pa */
}

static float bme280_comp_h(bme280_t *dev, int32_t adc_H)
{
  float var_H = ((float)dev->t_fine) - 76800.0f;
  var_H = ((float)adc_H - (((float)dev->dig_H4) * 64.0f + ((float)dev->dig_H5) / 16384.0f * var_H)) *
          (((float)dev->dig_H2) / 65536.0f *
           (1.0f + ((float)dev->dig_H6) / 67108864.0f * var_H *
            (1.0f + ((float)dev->dig_H3) / 67108864.0f * var_H)));
  var_H = var_H * (1.0f - ((float)dev->dig_H1) * var_H / 524288.0f);
  if (var_H > 100.0f)     var_H = 100.0f;
  else if (var_H < 0.0f)  var_H = 0.0f;
  return var_H;                                         /* %RH */
}

env_status_t bme280_init(bme280_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
  if (dev == NULL || hi2c == NULL) return ENV_ERR;
  memset(dev, 0, sizeof(*dev));
  dev->hi2c = hi2c; dev->addr7 = addr7;

  uint8_t id = 0;
  if (bme280_rd(dev, BME280_REG_ID, &id, 1) != ENV_OK) return ENV_ERR;
  dev->chip_id = id;                       /* kept for the console diagnostics */
  if (id != BME280_CHIP_ID) return ENV_ERR;

  /* Soft reset, then wait for the NVM copy to finish (status bit0 = im_update). */
  if (bme280_wr(dev, BME280_REG_RESET, BME280_RESET_WORD) != ENV_OK) return ENV_ERR;
  HAL_Delay(5);
  for (uint32_t i = 0; i < 10u; ++i) {
    uint8_t st = 0;
    if (bme280_rd(dev, BME280_REG_STATUS, &st, 1) != ENV_OK) return ENV_ERR;
    if ((st & 0x01u) == 0u) break;
    HAL_Delay(2);
  }

  if (bme280_load_calib(dev) != ENV_OK) return ENV_ERR;

  /* ctrl_hum must be written BEFORE ctrl_meas for it to latch (datasheet 5.4.3);
     ctrl_meas is then written per read to trigger forced mode, which also
     latches the humidity setting. The chip stays asleep in between. */
  if (bme280_wr(dev, BME280_REG_CTRL_HUM,  BME280_CTRL_HUM_X1)    != ENV_OK) return ENV_ERR;
  if (bme280_wr(dev, BME280_REG_CONFIG,    BME280_CONFIG_FILTOFF) != ENV_OK) return ENV_ERR;
  if (bme280_wr(dev, BME280_REG_CTRL_MEAS, 0x00u)                 != ENV_OK) return ENV_ERR; /* sleep */

  dev->ready = 1u;
  return ENV_OK;
}

env_status_t bme280_init_autoaddr(bme280_t *dev, I2C_HandleTypeDef *hi2c)
{
  if (bme280_init(dev, hi2c, BME280_ADDR_PRIMARY) == ENV_OK) return ENV_OK;
  return bme280_init(dev, hi2c, BME280_ADDR_SECONDARY);
}

env_status_t bme280_read(bme280_t *dev, float *t_c, float *rh_pct, float *p_hpa)
{
  if (dev == NULL || !dev->ready || t_c == NULL || rh_pct == NULL || p_hpa == NULL) return ENV_ERR;

  /* ctrl_hum is re-applied here so a chip that browned out and reset still gets
     humidity oversampling before the forced conversion starts. */
  if (bme280_wr(dev, BME280_REG_CTRL_HUM,  BME280_CTRL_HUM_X1)     != ENV_OK) return ENV_ERR;
  if (bme280_wr(dev, BME280_REG_CTRL_MEAS, BME280_CTRL_MEAS_FORCE) != ENV_OK) return ENV_ERR;

  /* Poll until the conversion clears STATUS.measuring (or give up). */
  uint32_t t0 = HAL_GetTick();
  for (;;) {
    uint8_t st = 0;
    if (bme280_rd(dev, BME280_REG_STATUS, &st, 1) != ENV_OK) return ENV_ERR;
    if ((st & BME280_STATUS_MEASURING) == 0u) break;
    if ((HAL_GetTick() - t0) > BME280_MEAS_TIMEOUT_MS) return ENV_ERR;
    HAL_Delay(1);
  }

  uint8_t d[8];
  if (bme280_rd(dev, BME280_REG_DATA, d, sizeof(d)) != ENV_OK) return ENV_ERR;

  int32_t adc_P = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4));
  int32_t adc_T = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4));
  int32_t adc_H = (int32_t)(((uint32_t)d[6] << 8)  |  (uint32_t)d[7]);

  /* 0x80000 / 0x8000 are the chip's "measurement skipped" sentinels. */
  if (adc_T == 0x80000 || adc_P == 0x80000 || adc_H == 0x8000) return ENV_ERR;

  float temp = bme280_comp_t(dev, adc_T);          /* also refreshes t_fine   */
  float pres = bme280_comp_p(dev, adc_P);          /* Pa                      */
  float hum  = bme280_comp_h(dev, adc_H);          /* %RH                     */

  /* Sanity gate: outside the sensor's own operating range the reading is a bus
     glitch, not weather — report failure so the frame's OK-bit stays clear. */
  if (temp < -45.0f || temp > 90.0f)       return ENV_ERR;
  if (pres < 30000.0f || pres > 120000.0f) return ENV_ERR;

  *t_c    = temp;
  *rh_pct = hum;
  *p_hpa  = pres / 100.0f;
  return ENV_OK;
}
