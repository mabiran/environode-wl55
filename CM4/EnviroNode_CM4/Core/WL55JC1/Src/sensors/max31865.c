/**
  ******************************************************************************
  * @file    max31865.c
  * @brief   MAX31865 PT1000 soil-temperature driver (SPI1 + software CS).
  *
  *          Power-aware sequence per reading: bias on -> settle -> one-shot ->
  *          read -> bias off. Continuous auto-conversion would keep VBIAS (and
  *          the RTD self-heating current) on permanently, which a solar node
  *          cannot afford and which would warm the probe it is measuring.
  *
  *          Conversion: 15-bit ratio -> R_rtd = ratio/32768 * RREF -> °C via
  *          Callendar-Van Dusen, with the standard inverse polynomial below 0 °C
  *          (frost matters for soil).
  ******************************************************************************
  */
#include <math.h>
#include "sensors/max31865.h"

/* Callendar–Van Dusen coefficients (IEC 60751, same for PT100/PT1000). */
#define CVD_A   (3.9083e-3f)
#define CVD_B   (-5.775e-7f)

#define MAX31865_SPI_TIMEOUT_MS   (20u)
#define MAX31865_BIAS_SETTLE_MS   (10u)   /*!< RC settle before a conversion.  */
#define MAX31865_CONV_MS          (70u)   /*!< 1-shot, 50 Hz filter: ~62.5 ms. */

static SPI_HandleTypeDef *s_hspi;
static GPIO_TypeDef      *s_cs_port;
static uint16_t           s_cs_pin;
static uint8_t            s_ready;
static uint8_t            s_cfg_base;     /*!< wire mode + filter, bias off.   */

static inline void cs_low (void) { if (s_cs_port) HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { if (s_cs_port) HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET); }

/* SPI mode 1 (CPOL 0 / CPHA 1) — see spi.c. Register read: address with bit7
   clear, then clock out the bytes; the chip auto-increments. */
static uint8_t max_read8(uint8_t reg)
{
  uint8_t tx = (uint8_t)(reg & 0x7Fu), rx = 0;
  cs_low();
  HAL_SPI_Transmit(s_hspi, &tx, 1, MAX31865_SPI_TIMEOUT_MS);
  HAL_SPI_Receive (s_hspi, &rx, 1, MAX31865_SPI_TIMEOUT_MS);
  cs_high();
  return rx;
}

static void max_read_buf(uint8_t reg, uint8_t *buf, uint16_t len)
{
  uint8_t tx = (uint8_t)(reg & 0x7Fu);
  cs_low();
  HAL_SPI_Transmit(s_hspi, &tx, 1, MAX31865_SPI_TIMEOUT_MS);
  HAL_SPI_Receive (s_hspi, buf, len, MAX31865_SPI_TIMEOUT_MS);
  cs_high();
}

static void max_write8(uint8_t reg, uint8_t val)
{
  uint8_t tx[2] = { (uint8_t)(reg | 0x80u), val };
  cs_low();
  HAL_SPI_Transmit(s_hspi, tx, 2, MAX31865_SPI_TIMEOUT_MS);
  cs_high();
}

env_status_t max31865_init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin, max31865_wires_t wires)
{
  if (hspi == NULL) return ENV_ERR;
  s_hspi = hspi; s_cs_port = cs_port; s_cs_pin = cs_pin; s_ready = 0u;
  cs_high();

  /* Base config: bias off, no auto-convert, 50 Hz mains rejection (NZ), plus
     the 3-wire bit when the probe needs it. The notch filter must only be
     changed while auto-conversion is off — which is always, here. */
  s_cfg_base = MAX31865_CFG_50HZ;
  if (wires == MAX31865_WIRES_3) s_cfg_base |= MAX31865_CFG_3WIRE;

  max_write8(MAX31865_REG_CONFIG, (uint8_t)(s_cfg_base | MAX31865_CFG_FAULTCLR));

  /* Read it back: a floating MISO gives 0x00/0xFF, so this catches "no chip". */
  uint8_t cfg = max_read8(MAX31865_REG_CONFIG);
  if (cfg == 0xFFu) return ENV_ERR;
  if ((cfg & (MAX31865_CFG_50HZ | MAX31865_CFG_3WIRE)) !=
      (s_cfg_base & (MAX31865_CFG_50HZ | MAX31865_CFG_3WIRE))) {
    return ENV_ERR;
  }

  s_ready = 1u;
  return ENV_OK;
}

/**
 * @brief  Read the raw 15-bit RTD ratio with a single one-shot conversion.
 * @param  ratio  [out] 15-bit ratio (0..32767)
 * @param  fault  [out] 1 if the RTD register's fault bit was set
 */
static env_status_t max31865_oneshot(uint16_t *ratio, uint8_t *fault)
{
  /* Bias on and let the input RC settle before starting the conversion. */
  max_write8(MAX31865_REG_CONFIG, (uint8_t)(s_cfg_base | MAX31865_CFG_VBIAS));
  HAL_Delay(MAX31865_BIAS_SETTLE_MS);

  /* Trigger the one-shot (self-clearing) and wait out the conversion. */
  max_write8(MAX31865_REG_CONFIG, (uint8_t)(s_cfg_base | MAX31865_CFG_VBIAS | MAX31865_CFG_1SHOT));
  HAL_Delay(MAX31865_CONV_MS);

  uint8_t rtd[2] = {0, 0};
  max_read_buf(MAX31865_REG_RTD_MSB, rtd, 2);

  /* Bias off again — keeps both the quiescent draw and RTD self-heating down. */
  max_write8(MAX31865_REG_CONFIG, s_cfg_base);

  uint16_t word = (uint16_t)(((uint16_t)rtd[0] << 8) | rtd[1]);
  *fault = (uint8_t)(word & 0x0001u);
  *ratio = (uint16_t)(word >> 1);
  return ENV_OK;
}

env_status_t max31865_read_ohms(float *ohms)
{
  if (!s_ready || ohms == NULL) return ENV_ERR;

  uint16_t ratio = 0; uint8_t fault = 0;
  if (max31865_oneshot(&ratio, &fault) != ENV_OK) return ENV_ERR;
  if (fault) return ENV_ERR;                 /* caller may read the fault reg  */
  if (ratio == 0u || ratio >= 0x7FFFu) return ENV_ERR;  /* open / short probe  */

  *ohms = ((float)ratio * MAX31865_RREF) / 32768.0f;
  return ENV_OK;
}

env_status_t max31865_read_celsius(float *t_c)
{
  if (!s_ready || t_c == NULL) return ENV_ERR;

  float rt = 0.0f;
  if (max31865_read_ohms(&rt) != ENV_OK) return ENV_ERR;

  /* Positive branch: invert R = R0(1 + A.T + B.T^2) exactly. */
  const float z1 = -CVD_A;
  const float z2 = CVD_A * CVD_A - (4.0f * CVD_B);
  const float z3 = (4.0f * CVD_B) / MAX31865_RTD_NOMINAL;
  const float z4 = 2.0f * CVD_B;

  float disc = z2 + (z3 * rt);
  if (disc < 0.0f) return ENV_ERR;                 /* nonsense resistance      */
  float temp = (sqrtf(disc) + z1) / z4;

  if (temp < 0.0f) {
    /* Below 0 °C the CVD form needs the C coefficient; use the standard
       inverse polynomial instead, on a PT100-normalised resistance. */
    float rp = (rt / MAX31865_RTD_NOMINAL) * 100.0f;
    float poly = rp;
    temp  = -242.02f;
    temp += 2.2228f * poly;
    poly *= rp; temp += 2.5859e-3f * poly;
    poly *= rp; temp -= 4.8260e-6f * poly;
    poly *= rp; temp -= 2.8183e-8f * poly;
    poly *= rp; temp += 1.5243e-10f * poly;
  }

  if (temp < -60.0f || temp > 120.0f) return ENV_ERR;   /* soil probe sanity   */

  *t_c = temp;
  return ENV_OK;
}

uint8_t max31865_read_fault(void)
{
  if (!s_ready) return 0xFFu;
  uint8_t f = max_read8(MAX31865_REG_FAULT);
  max_write8(MAX31865_REG_CONFIG, (uint8_t)(s_cfg_base | MAX31865_CFG_FAULTCLR)); /* clear */
  return f;
}
