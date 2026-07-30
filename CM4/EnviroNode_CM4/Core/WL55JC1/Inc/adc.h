/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.h
  * @brief   This file contains all the function prototypes for
  *          the adc.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __ADC_H__
#define __ADC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern ADC_HandleTypeDef hadc;

/* USER CODE BEGIN Private defines */
/** @name EnviroNode ADC channel assignment (docs/PINOUT.md)
  *   leaf wetness    PB1  = ADC_IN5  (Arduino A0)  <- Decagon LWS
  *   soil moisture   PB2  = ADC_IN4  (Arduino A1)
  *   wind direction  PB4  = ADC_IN3  (Arduino A3)  <- Davis 7911 pot wiper
  *   battery divider PB13 = ADC_IN0  (Arduino A5)
  *
  * Two moves on 2026-07-30 (docs/LOGBOOK.md):
  *   - LW took A0 because that is the socket the Decagon LWS is wired into;
  *     soil moisture moved to A1.
  *   - the battery divider moved off A4 to A5, freeing A4 to be the Davis 7911's
  *     wind-speed contact input. A4 next to A3 puts the anemometer's whole
  *     4-wire cable on one Grove socket. A4 is NOT an ADC channel any more.
  * @{ */
#define ENVNODE_ADC_CH_LEAF     ADC_CHANNEL_5    /*!< PB1  / A0 — leaf wetness   */
#define ENVNODE_ADC_CH_SOIL     ADC_CHANNEL_4    /*!< PB2  / A1 — soil moisture  */
#define ENVNODE_ADC_CH_WINDDIR  ADC_CHANNEL_3    /*!< PB4  / A3 — vane pot wiper */
#define ENVNODE_ADC_CH_BATT     ADC_CHANNEL_0    /*!< PB13 / A5 — batt divider   */
/** @} */
/* USER CODE END Private defines */

void MX_ADC_Init(void);

/* USER CODE BEGIN Prototypes */

/**
 * @brief  Convert one external channel once.
 *
 * The sequencer stays "fully configurable, rank 1 only", so every read simply
 * re-points rank 1 at the wanted channel and converts. One code path for all
 * four channels, no scan/DMA setup.
 *
 * @param  channel  ADC_CHANNEL_x (use the ENVNODE_ADC_CH_* aliases).
 * @param  raw      [out] 12-bit result.
 * @retval HAL_OK on success, HAL_ERROR / HAL_TIMEOUT otherwise.
 */
HAL_StatusTypeDef ADC_ReadChannel(uint32_t channel, uint16_t *raw);

/**
 * @brief  Convert one channel @p samples times and return the mean — cheap
 *         noise rejection for the high-impedance soil/leaf probes.
 * @param  channel  ADC_CHANNEL_x.
 * @param  samples  1..64 (0 is treated as 1).
 * @param  raw      [out] averaged 12-bit result.
 */
HAL_StatusTypeDef ADC_ReadChannelAvg(uint32_t channel, uint16_t samples, uint16_t *raw);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __ADC_H__ */

