/**
  ******************************************************************************
  * @file    analog_sensors.h
  * @brief   ADC-based sensors: soil moisture, leaf wetness, battery, wind dir.
  *
  *          Four single-ended ADC channels read in one scan. Raw counts are
  *          returned for soil/leaf (calibration curve applied later / off-node);
  *          battery is scaled by the divider ratio; wind direction maps the vane
  *          potentiometer 0..Vref -> 0..360°.
  ******************************************************************************
  */
#ifndef ANALOG_SENSORS_H
#define ANALOG_SENSORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sensors/envnode_sensors.h"

/* Front-end constants — tune to the board. */
#define ADC_VREF_V         (3.30f)     /*!< ADC reference (V).                  */
#define ADC_FULL_SCALE     (4095.0f)   /*!< 12-bit.                             */
#define BATT_DIVIDER_RATIO (2.0f)      /*!< Vbatt = Vadc * ratio (set to your R's)*/
#define WINDDIR_DEG_MAX    (360.0f)

/**
 * @brief  Configure/calibrate the ADC block (channels, sampling time).
 */
env_status_t analog_init(void);

/**
 * @brief  One scan of all four channels.
 * @param[out] soil_raw     soil-moisture ADC counts (0..4095)
 * @param[out] leaf_raw     leaf-wetness ADC counts (0..4095)
 * @param[out] batt_v       battery volts (= Vadc * BATT_DIVIDER_RATIO)
 * @param[out] winddir_deg  wind direction 0..359.9° (vane pot; add vane offset)
 * @retval ENV_OK / ENV_ERR / ENV_NOTIMPL
 */
env_status_t analog_read_all(uint16_t *soil_raw, uint16_t *leaf_raw, float *batt_v, float *winddir_deg);

#ifdef __cplusplus
}
#endif
#endif /* ANALOG_SENSORS_H */
