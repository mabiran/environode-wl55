/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */
#include "pins_config.h"    /* EnviroNode pin aliases (docs/PINOUT.md) */
/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* PB10 (D6) and PC1 (D7) are deliberately NOT configured here: the inherited
     Pi_Wake / Pi-5V-enable outputs that drove them are gone, so leaving both at
     their reset state (analog, no drive) keeps them free for a future sensor —
     see docs/PINOUT.md "Free pins". */

  /* --- EnviroNode: MAX31865 chip-select (PA4 / Arduino D10), idle HIGH ------ */
  HAL_GPIO_WritePin(ENV_RTD_CS_Port, ENV_RTD_CS_Pin, GPIO_PIN_SET);
  GPIO_InitStruct.Pin = ENV_RTD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ENV_RTD_CS_Port, &GPIO_InitStruct);

  /* --- EnviroNode: pulse inputs -------------------------------------------
     Rain tipping bucket (PB3 / D3) and the Davis 7911 wind-speed contact
     (PB14 / A4) are dry contacts to GND: internal pull-up, interrupt on the
     falling edge, and the bounce is filtered in software (pulse_counter.c).
     An external 10 k pull-up is recommended on the anemometer line because its
     cable is 12 m long — the internal ~40 k works but is less immune to noise
     (docs/PINOUT.md). */
  GPIO_InitStruct.Pin = ENV_RAIN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(ENV_RAIN_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = ENV_WIND_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(ENV_WIND_Port, &GPIO_InitStruct);

  /* EXTI3 = rain (PB3), EXTI15_10 = wind (PB14). Low priority: the handlers only
     bump a counter, and they must never delay the UART console. */
  HAL_NVIC_SetPriority(ENV_RAIN_EXTI_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(ENV_RAIN_EXTI_IRQn);
  HAL_NVIC_SetPriority(ENV_WIND_EXTI_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(ENV_WIND_EXTI_IRQn);

  /* Status LED output on PC2 (Arduino D8, free Grove "D8" socket).
     Start LOW = off. Driven with brief low-duty pulses by Status_Led_Tick(). */
  HAL_GPIO_WritePin(STATUS_LED_Port, STATUS_LED_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = STATUS_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(STATUS_LED_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
