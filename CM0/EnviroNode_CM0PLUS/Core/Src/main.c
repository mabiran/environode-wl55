/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : KoreroNet CM0+ radio core — LoRaWAN end node.
  *
  * Runs the LoRaWAN stack + SubGHz radio on the Cortex-M0+ core. Joins TTN via
  * OTAA (AU915 / sub-band 2) and sends a "HELLO" uplink periodically; the CM4
  * application core can also trigger an on-demand send (see lora_app.c).
  *
  * IMPORTANT (dual-core): the system clock tree is configured by CM4 (CPU1),
  * which boots this core via HAL_PWREx_ReleaseCore(PWR_CORE_CPU2). This core
  * must NOT call SystemClock_Config() — doing so would fight CM4 over the
  * shared RCC. We only bring up the peripherals this core owns (SubGHz radio,
  * LPTIM1 timer, USART2 trace). Timing uses LPTIM1/LSE — never the RTC, which
  * belongs entirely to the CM4 application (calendar + backup registers).
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics / KoreroNet.
  * Licensed per the ST LICENSE file in the repository root; provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "app_lorawan.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include "sys_app.h"
/* USER CODE END Includes */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* NOTE: no SystemClock_Config() here — CM4 owns the shared clock tree. */
  /* USER CODE END Init */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LoRaWAN_Init();   /* brings up trace, LPTIM timer, radio, MAC, OTAA join */

  /* USER CODE BEGIN 2 */
  APP_LOG(TS_OFF, VLEVEL_M, "\r\n###### KoreroNet CM0+ radio core: LoRaWAN node up ######\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  while (1)
  {
    MX_LoRaWAN_Process();
    /* USER CODE BEGIN 3 */
    /* USER CODE END 3 */
  }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  UNUSED(file);
  UNUSED(line);
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
