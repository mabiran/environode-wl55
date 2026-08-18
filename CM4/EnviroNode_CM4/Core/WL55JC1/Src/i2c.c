/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    i2c.c
  * @brief   This file provides code for the configuration
  *          of the I2C instances.
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
#include "i2c.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

/* Fast-mode (400 kHz) TIMINGR for PCLK1 = 16 MHz (HSI, AHB/APB1 div 1).
   PRESC=0 SCLDEL=0 SDADEL=1 SCLH=13 SCLL=20 -> ~400 kHz. */
#define ENVNODE_I2C_TIMING_400K   (0x00100D14u)

/* Standard-mode (100 kHz) TIMINGR for the same 16 MHz kernel clock — ST's
   canonical value (PRESC=3, SCLDEL=4, SDADEL=2, SCLH=0x0F, SCLL=0x13).
   I2C2 runs at this since r23: its wiring proved marginal enough to wedge the
   peripheral repeatedly at 400 kHz (BUSY lockups — see I2C2_BusRecover), and
   4× more timing slack is the difference between every-other-read failing and
   a solid bus. Nothing on I2C2 needs speed: the INA219 and BME280 exchange a
   few bytes per cycle. */
#define ENVNODE_I2C_TIMING_100K   (0x30420F13u)

/* I2C1 init function — EnviroNode BME280 #2 on the board pins (PA9/PA10). */
void MX_I2C1_Init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = ENVNODE_I2C_TIMING_400K;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* I2C2 init function */
void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = ENVNODE_I2C_TIMING_100K;   /* slowed from 400 k, r23 */
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(i2cHandle->Instance==I2C1)
  {
  /** Initializes the peripherals clocks
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
    PeriphClkInitStruct.I2c1ClockSelection = RCC_I2C1CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**I2C1 GPIO Configuration
    PA9      ------> I2C1_SCL   (Arduino D9)
    PA10     ------> I2C1_SDA   (Arduino A2)
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_NOPULL;          /* external 4k7 pull-ups */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* I2C1 clock enable */
    __HAL_RCC_I2C1_CLK_ENABLE();
  }
  else if(i2cHandle->Instance==I2C2)
  {
  /* USER CODE BEGIN I2C2_MspInit 0 */

  /* USER CODE END I2C2_MspInit 0 */

  /** Initializes the peripherals clocks
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2C2;
    PeriphClkInitStruct.I2c2ClockSelection = RCC_I2C2CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**I2C2 GPIO Configuration
    PA12     ------> I2C2_SCL
    PA11     ------> I2C2_SDA
    */
    GPIO_InitStruct.Pin = GPIO_PIN_12|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* I2C2 clock enable */
    __HAL_RCC_I2C2_CLK_ENABLE();
  /* USER CODE BEGIN I2C2_MspInit 1 */

  /* USER CODE END I2C2_MspInit 1 */
  }
}

/**
 * @brief  Unwedge I2C2 (PA12 SCL / PA11 SDA) and re-initialise the peripheral.
 *
 * The classic STM32 I2C lockup: a glitch on a marginal SDA/SCL contact makes
 * the peripheral latch its BUSY flag (it believes another master owns the
 * bus), after which every transfer times out until the peripheral is reset —
 * the whole bus appears empty even though the slaves are fine. Seen live on
 * this bench (LOGBOOK r23): the INA219 answered at boot, wedged minutes
 * later, and a core reset revived it — the signature of a peripheral-side,
 * not slave-side, hang.
 *
 * Recovery: drop the peripheral, drive the pins manually — up to 9 SCL
 * clocks releases a slave stuck mid-byte, then a STOP condition — and bring
 * the peripheral back up. Cheap (<25 ms), safe to call on a healthy bus.
 *
 * @retval 1 if SDA reads high (bus free) after recovery, 0 if still stuck
 *         (genuinely shorted SDA — a hardware problem no clocking fixes).
 */
int I2C2_BusRecover(void)
{
  GPIO_InitTypeDef g = {0};

  (void)HAL_I2C_DeInit(&hi2c2);

  /* Manual open-drain control of both lines, released (high) first. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12 | GPIO_PIN_11, GPIO_PIN_SET);
  g.Pin   = GPIO_PIN_12 | GPIO_PIN_11;
  g.Mode  = GPIO_MODE_OUTPUT_OD;
  g.Pull  = GPIO_NOPULL;              /* the modules carry the pull-ups      */
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &g);
  HAL_Delay(1);

  /* Clock SCL until a slave stuck mid-byte lets go of SDA (max 9 bits). */
  for (int i = 0; i < 9 && HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11) == GPIO_PIN_RESET; ++i) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET); HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);   HAL_Delay(1);
  }

  /* STOP condition: SDA low -> high while SCL is high. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_RESET); HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);   HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_11, GPIO_PIN_SET);   HAL_Delay(1);

  int sda_free = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11) == GPIO_PIN_SET);

  /* Back to the peripheral: MX init re-runs the Msp GPIO/clock config. */
  MX_I2C2_Init();
  return sda_free;
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{

  if(i2cHandle->Instance==I2C1)
  {
    /* Peripheral clock disable */
    __HAL_RCC_I2C1_CLK_DISABLE();

    /**I2C1 GPIO Configuration
    PA9      ------> I2C1_SCL
    PA10     ------> I2C1_SDA
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_10);
  }
  else if(i2cHandle->Instance==I2C2)
  {
  /* USER CODE BEGIN I2C2_MspDeInit 0 */

  /* USER CODE END I2C2_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_I2C2_CLK_DISABLE();

    /**I2C2 GPIO Configuration
    PA12     ------> I2C2_SCL
    PA11     ------> I2C2_SDA
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_12);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11);

  /* USER CODE BEGIN I2C2_MspDeInit 1 */

  /* USER CODE END I2C2_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
