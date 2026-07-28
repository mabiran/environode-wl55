/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart_if.c
  * @author  MCD Application Team
  * @brief   Configuration of UART driver interface for hyperterminal communication
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
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
#include "usart_if.h"

/* USER CODE BEGIN Includes */
#include "korero_mailbox.h"
/* USER CODE END Includes */

/* External variables ---------------------------------------------------------*/
/**
  * @brief DMA handle
  */
extern DMA_HandleTypeDef hdma_usart2_tx;

/**
  * @brief UART handle
  */
extern UART_HandleTypeDef huart2;

/**
  * @brief buffer to receive 1 character
  */
uint8_t charRx;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Private typedef -----------------------------------------------------------*/
/**
  * @brief Trace driver callbacks handler
  */
const UTIL_ADV_TRACE_Driver_s UTIL_TraceDriver =
{
  vcom_Init,
  vcom_DeInit,
  vcom_ReceiveInit,
  vcom_Trace_DMA,
};

/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/**
  * @brief  TX complete callback
  * @return none
  */
static void (*TxCpltCallback)(void *);
/**
  * @brief  RX complete callback
  * @param  rxChar ptr of chars buffer sent by user
  * @param  size buffer size
  * @param  error errorcode
  * @return none
  */
static void (*RxCpltCallback)(uint8_t *rxChar, uint16_t size, uint8_t error);

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Exported functions --------------------------------------------------------*/

/* KoreroNet: the ST-Link VCP (USART2/PA2-PA3) is owned by the CM4 application
   core. The CM0+ radio core therefore does NOT touch USART2 — its LoRaWAN trace
   is pushed into the shared mailbox ring and CM4 prints it on the VCP. */

UTIL_ADV_TRACE_Status_t vcom_Init(void (*cb)(void *))
{
  TxCpltCallback = cb;
  return UTIL_ADV_TRACE_OK;
}

UTIL_ADV_TRACE_Status_t vcom_DeInit(void)
{
  return UTIL_ADV_TRACE_OK;
}

void vcom_Trace(uint8_t *p_data, uint16_t size)
{
  (void)vcom_Trace_DMA(p_data, size);
}

UTIL_ADV_TRACE_Status_t vcom_Trace_DMA(uint8_t *p_data, uint16_t size)
{
  /* Push bytes into the CM0+ -> CM4 trace ring (drop on overflow), then signal
     completion synchronously (the write is a fast memory copy, never blocks). */
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint32_t head = mb->trace_head & KORERO_TRACE_MASK;
  uint32_t tail = mb->trace_tail & KORERO_TRACE_MASK;

  for (uint16_t i = 0; i < size; i++)
  {
    uint32_t next = (head + 1U) & KORERO_TRACE_MASK;
    if (next == tail)
    {
      break;   /* ring full: drop the remainder */
    }
    mb->trace[head] = p_data[i];
    head = next;
  }
  mb->trace_head = head;

  if (TxCpltCallback != NULL)
  {
    TxCpltCallback(NULL);
  }
  return UTIL_ADV_TRACE_OK;
}

UTIL_ADV_TRACE_Status_t vcom_ReceiveInit(void (*RxCb)(uint8_t *rxChar, uint16_t size, uint8_t error))
{
  /* CM0+ does not receive on the VCP (CM4 owns it). */
  RxCpltCallback = RxCb;
  return UTIL_ADV_TRACE_OK;
}

void vcom_Resume(void)
{
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  /* USER CODE BEGIN HAL_UART_TxCpltCallback_1 */

  /* USER CODE END HAL_UART_TxCpltCallback_1 */
  /* buffer transmission complete*/
  if (huart->Instance == USART2)
  {
    TxCpltCallback(NULL);
  }
  /* USER CODE BEGIN HAL_UART_TxCpltCallback_2 */

  /* USER CODE END HAL_UART_TxCpltCallback_2 */
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  /* USER CODE BEGIN HAL_UART_RxCpltCallback_1 */

  /* USER CODE END HAL_UART_RxCpltCallback_1 */
  if (huart->Instance == USART2)
  {
    if ((NULL != RxCpltCallback) && (HAL_UART_ERROR_NONE == huart->ErrorCode))
    {
      RxCpltCallback(&charRx, 1, 0);
    }
    HAL_UART_Receive_IT(huart, &charRx, 1);
  }
  /* USER CODE BEGIN HAL_UART_RxCpltCallback_2 */

  /* USER CODE END HAL_UART_RxCpltCallback_2 */
}

/* USER CODE BEGIN EF */

/* USER CODE END EF */

/* Private Functions Definition -----------------------------------------------*/

/* USER CODE BEGIN PrFD */

/* USER CODE END PrFD */
