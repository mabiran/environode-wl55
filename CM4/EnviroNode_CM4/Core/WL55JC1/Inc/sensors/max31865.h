/**
  ******************************************************************************
  * @file    max31865.h
  * @brief   MAX31865 RTD-to-digital front-end for a PT1000 soil-temp probe (SPI).
  *
  *          PT1000: Rnominal = 1000 Ω, so the reference resistor MUST be ~4×,
  *          i.e. Rref = 4020 Ω (4.02 kΩ, 0.1%). (PT100 would use 430 Ω.)
  *          Supports 2/3/4-wire. CS on a GPIO; optional DRDY on an EXTI line.
  ******************************************************************************
  */
#ifndef MAX31865_H
#define MAX31865_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32wlxx_hal.h"
#include "sensors/envnode_sensors.h"

/* PT1000 constants. */
#define MAX31865_RTD_NOMINAL   (1000.0f)   /*!< PT1000 R0 (Ω).                  */
#define MAX31865_RREF          (4020.0f)   /*!< reference resistor (Ω), PT1000. */

/* Register map. */
#define MAX31865_REG_CONFIG    (0x00u)   /*!< read; +0x80 to write             */
#define MAX31865_REG_RTD_MSB   (0x01u)   /*!< 15-bit RTD ratio + fault bit0    */
#define MAX31865_REG_HFT_MSB   (0x03u)   /*!< high fault threshold             */
#define MAX31865_REG_LFT_MSB   (0x05u)   /*!< low  fault threshold             */
#define MAX31865_REG_FAULT     (0x07u)   /*!< fault status                     */

/* CONFIG bits. */
#define MAX31865_CFG_VBIAS     (0x80u)
#define MAX31865_CFG_AUTO      (0x40u)   /*!< continuous conversion            */
#define MAX31865_CFG_1SHOT     (0x20u)
#define MAX31865_CFG_3WIRE     (0x10u)   /*!< set = 3-wire; clear = 2/4-wire   */
#define MAX31865_CFG_FAULTCLR  (0x02u)
#define MAX31865_CFG_50HZ      (0x01u)   /*!< set = 50 Hz reject (NZ mains)    */

typedef enum { MAX31865_WIRES_2 = 2, MAX31865_WIRES_3 = 3, MAX31865_WIRES_4 = 4 } max31865_wires_t;

/**
 * @brief  Bind SPI + CS, configure bias / wires / mains filter (50 Hz for NZ).
 */
env_status_t max31865_init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin, max31865_wires_t wires);

/**
 * @brief  Read the RTD ratio, convert to resistance then °C (Callendar–Van Dusen
 *         above 0 °C, inverse polynomial below it).
 *
 * Each call runs one bias-on → settle → one-shot → bias-off cycle (~80 ms), so
 * neither VBIAS nor the RTD excitation current is left on between samples.
 *
 * @param[out] t_c  soil temperature °C
 * @retval ENV_OK / ENV_ERR (bus error, fault bit set — then call
 *         max31865_read_fault() — or an out-of-range result)
 */
env_status_t max31865_read_celsius(float *t_c);

/**
 * @brief  Raw RTD resistance in ohms (same conversion cycle as above).
 *         Useful on the bench: a PT1000 at 0 °C reads 1000 Ω, ~1039 Ω at 10 °C.
 */
env_status_t max31865_read_ohms(float *ohms);

/** @brief  Read + clear the fault-status register (0 = no fault). */
uint8_t max31865_read_fault(void);

#ifdef __cplusplus
}
#endif
#endif /* MAX31865_H */
