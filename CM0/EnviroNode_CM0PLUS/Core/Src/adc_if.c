/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc_if.c
  * @brief   KoreroNet CM0+ stub for the LoRaWAN ADC interface.
  *
  * The on-chip ADC belongs to the CM4 application core (battery sensing via
  * INA219 + ADC). The CM0+ radio core must NOT touch the shared ADC, so this
  * provides fixed values for the two readings the LoRaWAN stack asks for
  * (battery level for DevStatusReq, MCU temperature). If you ever want a real
  * battery reading in the uplink, have CM4 push it to CM0+ over the inter-core
  * mailbox instead of sampling the ADC from here.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "adc_if.h"
#include "sys_app.h"

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Initialize the measurement back-end. No-op: the ADC is owned by CM4.
  */
void SYS_InitMeasurement(void)
{
  /* nothing to do — CM0+ does not drive the ADC */
}

/**
  * @brief  De-initialize the measurement back-end. No-op.
  */
void SYS_DeInitMeasurement(void)
{
  /* nothing to do */
}

/**
  * @brief  Return the battery level in millivolts.
  * @note   Stubbed: the ADC is owned by CM4. Report a healthy supply so the
  *         LoRaWAN DevStatusAns advertises "externally powered / full".
  */
uint16_t SYS_GetBatteryLevel(void)
{
  return 3300U; /* mV — placeholder; CM0+ does not own the ADC */
}

/**
  * @brief  Return the MCU/junction temperature in degrees Celsius.
  * @note   Stubbed constant; not used for MAC decisions, only telemetry.
  */
int16_t SYS_GetTemperatureLevel(void)
{
  return 20; /* deg C — placeholder */
}
