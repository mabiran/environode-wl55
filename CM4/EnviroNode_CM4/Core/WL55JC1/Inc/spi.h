/**
  ******************************************************************************
  * @file    spi.h
  * @brief   SPI1 configuration — MAX31865 PT1000 RTD front-end.
  *
  *          SPI1 sits on the plain Arduino SPI pins of the NUCLEO-WL55JC1:
  *            PA5  SCK  (D13)   PA6  MISO (D12)   PA7  MOSI (D11)
  *          Chip-select is a plain GPIO (PA4 / D10) driven by the driver, not
  *          the hardware NSS — see gpio.c and docs/PINOUT.md.
  *
  *          Mode 1 (CPOL=0, CPHA=1) at ~2 MHz: the MAX31865 clocks data out on
  *          the falling edge and accepts up to 5 MHz.
  ******************************************************************************
  */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SPI_H__
#define __SPI_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

extern SPI_HandleTypeDef hspi1;

void MX_SPI1_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H__ */
