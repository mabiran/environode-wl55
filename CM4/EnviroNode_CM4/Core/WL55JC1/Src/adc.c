/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.c
  * @brief   This file provides code for the configuration
  *          of the ADC instances.
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
#include "adc.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

ADC_HandleTypeDef hadc;

/* ADC init function */
void MX_ADC_Init(void)
{

  /* USER CODE BEGIN ADC_Init 0 */

  /* USER CODE END ADC_Init 0 */

  /* USER CODE BEGIN ADC_Init 1 */

  /* USER CODE END ADC_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc.Instance = ADC;
  /* PCLK is 16 MHz (HSI) and the core runs at voltage scale 2, so divide the
     ADC clock to 4 MHz — in spec for range 2 and still far faster than needed
     for four slow environmental channels. */
  hadc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc.Init.Resolution = ADC_RESOLUTION_12B;
  hadc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc.Init.LowPowerAutoWait = DISABLE;
  hadc.Init.LowPowerAutoPowerOff = DISABLE;
  hadc.Init.ContinuousConvMode = DISABLE;
  hadc.Init.NbrOfConversion = 1;
  hadc.Init.DiscontinuousConvMode = DISABLE;
  hadc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc.Init.DMAContinuousRequests = DISABLE;
  hadc.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  /* 160.5 cycles @ 4 MHz = ~40 us of sampling: needed because the soil-moisture
     and leaf-wetness probes are high-impedance sources. */
  hadc.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_160CYCLES_5;
  hadc.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_160CYCLES_5;
  hadc.Init.OversamplingMode = DISABLE;
  hadc.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&hadc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC_Init 2 */
  /* Run the self-calibration once while the ADC is still disabled — worth a few
     LSB of accuracy on the battery divider and the vane potentiometer. */
  if (HAL_ADCEx_Calibration_Start(&hadc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END ADC_Init 2 */

}

void HAL_ADC_MspInit(ADC_HandleTypeDef* adcHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(adcHandle->Instance==ADC)
  {
  /* USER CODE BEGIN ADC_MspInit 0 */

  /* USER CODE END ADC_MspInit 0 */
    /* ADC clock enable */
    __HAL_RCC_ADC_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**ADC GPIO Configuration — EnviroNode analog block (docs/PINOUT.md)
    PB1      ------> ADC_IN5   leaf wetness    (Arduino A0, Decagon LWS)
    PB2      ------> ADC_IN4   soil moisture   (Arduino A1)
    PB4      ------> ADC_IN3   wind direction  (Arduino A3, Davis 7911 wiper)
    PB14     ------> ADC_IN1   wind speed      (Arduino A4, Davis 7911 contact)
    PB13     ------> ADC_IN0   battery divider (Arduino A5)
    The speed contact is sampled, not interrupt-counted: analog_sensors.c bursts
    this channel at ~1 kHz for a few seconds and counts transitions, which lets
    the node sleep between cycles (docs/SENSORS.md §9).
    */
    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_4|GPIO_PIN_13|GPIO_PIN_14;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN ADC_MspInit 1 */

  /* USER CODE END ADC_MspInit 1 */
  }
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef* adcHandle)
{

  if(adcHandle->Instance==ADC)
  {
  /* USER CODE BEGIN ADC_MspDeInit 0 */

  /* USER CODE END ADC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_ADC_CLK_DISABLE();

    /**ADC GPIO Configuration
    PB1 / PB2 / PB4 / PB13 / PB14 ---> ADC_IN5 / IN4 / IN3 / IN0 / IN1
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_4|GPIO_PIN_13|GPIO_PIN_14);

  /* USER CODE BEGIN ADC_MspDeInit 1 */

  /* USER CODE END ADC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

#define ADC_CONV_TIMEOUT_MS   (10u)   /*!< 40 us of sampling — 10 ms is plenty. */

HAL_StatusTypeDef ADC_ReadChannel(uint32_t channel, uint16_t *raw)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  HAL_StatusTypeDef st;

  if (raw == NULL) return HAL_ERROR;

  /* Point sequencer rank 1 at this channel (sequencer is "fully configurable,
     rank 1 only", so this replaces whatever was selected before). */
  sConfig.Channel      = channel;
  sConfig.Rank         = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  st = HAL_ADC_ConfigChannel(&hadc, &sConfig);
  if (st != HAL_OK) return st;

  st = HAL_ADC_Start(&hadc);
  if (st != HAL_OK) return st;

  st = HAL_ADC_PollForConversion(&hadc, ADC_CONV_TIMEOUT_MS);
  if (st == HAL_OK) {
    *raw = (uint16_t)HAL_ADC_GetValue(&hadc);
  }
  (void)HAL_ADC_Stop(&hadc);
  return st;
}

HAL_StatusTypeDef ADC_ReadChannelAvg(uint32_t channel, uint16_t samples, uint16_t *raw)
{
  if (raw == NULL) return HAL_ERROR;
  if (samples == 0u) samples = 1u;
  if (samples > 64u) samples = 64u;

  uint32_t sum = 0u;
  for (uint16_t i = 0u; i < samples; ++i) {
    uint16_t v = 0u;
    HAL_StatusTypeDef st = ADC_ReadChannel(channel, &v);
    if (st != HAL_OK) return st;
    sum += v;
  }
  *raw = (uint16_t)((sum + (samples / 2u)) / samples);
  return HAL_OK;
}

/* USER CODE END 1 */
