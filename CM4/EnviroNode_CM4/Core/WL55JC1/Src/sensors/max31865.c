/**
  ******************************************************************************
  * @file    max31865.c
  * @brief   MAX31865 PT1000 driver — skeleton (Phase 2 fills SPI + CVD math).
  ******************************************************************************
  */
#include "sensors/max31865.h"

/* Callendar–Van Dusen coefficients (IEC 60751, same for PT100/PT1000). */
#define CVD_A   (3.9083e-3f)
#define CVD_B   (-5.775e-7f)

static SPI_HandleTypeDef *s_hspi;
static GPIO_TypeDef      *s_cs_port;
static uint16_t           s_cs_pin;
static uint8_t            s_ready;

static inline void cs_low (void) { if (s_cs_port) HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { if (s_cs_port) HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET); }

/* SPI mode 1 (CPOL0/CPHA1) or mode 3 — set in CubeMX. Register read/write helpers. */
static uint8_t max_read8(uint8_t reg)
{
  uint8_t tx = (uint8_t)(reg & 0x7Fu), rx = 0;
  cs_low();
  HAL_SPI_Transmit(s_hspi, &tx, 1, 20);
  HAL_SPI_Receive (s_hspi, &rx, 1, 20);
  cs_high();
  return rx;
}
static void max_write8(uint8_t reg, uint8_t val)
{
  uint8_t tx[2] = { (uint8_t)(reg | 0x80u), val };
  cs_low();
  HAL_SPI_Transmit(s_hspi, tx, 2, 20);
  cs_high();
}

env_status_t max31865_init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin, max31865_wires_t wires)
{
  if (hspi == NULL) return ENV_ERR;
  s_hspi = hspi; s_cs_port = cs_port; s_cs_pin = cs_pin; s_ready = 0u;

  uint8_t cfg = MAX31865_CFG_VBIAS | MAX31865_CFG_AUTO | MAX31865_CFG_50HZ; /* NZ = 50 Hz */
  if (wires == MAX31865_WIRES_3) cfg |= MAX31865_CFG_3WIRE;
  /* TODO(Phase2): write CONFIG, allow Vbias settle (~10 ms), optionally set fault
   * thresholds. Verify readback. */
  max_write8(MAX31865_REG_CONFIG, cfg);
  (void)max_read8;
  s_ready = 1u;
  return ENV_NOTIMPL;
}

env_status_t max31865_read_celsius(float *t_c)
{
  if (!s_ready || t_c == NULL) return ENV_ERR;
  /* TODO(Phase2):
   *  - read RTD_MSB/LSB -> 15-bit ratio (drop bit0 = fault flag)
   *  - Rrtd = (ratio / 32768) * RREF
   *  - solve CVD for t>=0: t = (-A + sqrt(A^2 - 4B(1 - Rrtd/R0))) / (2B)
   *    (use the ITS-90 poly extension for sub-zero soil temps)
   *  - if fault bit set: return ENV_ERR and let caller read max31865_read_fault()
   */
  *t_c = 0.0f;
  return ENV_NOTIMPL;
}

uint8_t max31865_read_fault(void)
{
  if (!s_ready) return 0xFFu;
  uint8_t f = max_read8(MAX31865_REG_FAULT);
  max_write8(MAX31865_REG_CONFIG, MAX31865_CFG_FAULTCLR); /* clear */
  return f;
}
