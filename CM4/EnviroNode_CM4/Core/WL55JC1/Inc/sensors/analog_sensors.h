/**
  ******************************************************************************
  * @file    analog_sensors.h
  * @brief   ADC-based sensors: soil moisture, leaf wetness, battery, wind dir.
  *
  *          Four single-ended channels, read one at a time through
  *          ADC_ReadChannelAvg() (adc.c) with a 160.5-cycle sampling time — the
  *          soil and leaf probes are high-impedance sources.
  *
  *          Raw counts are returned for soil/leaf (the calibration curve is
  *          applied off-node), battery is scaled by the measured divider ratio
  *          from pins_config.h, and wind direction maps the vane potentiometer
  *          0..Vref onto 0..360° plus the configurable north offset.
  ******************************************************************************
  */
#ifndef ANALOG_SENSORS_H
#define ANALOG_SENSORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sensors/envnode_sensors.h"
#include "pins_config.h"      /* ADC_VREF_VOLT, ADC_FULL_SCALE, VBAT_DIVIDER_GAIN */

/* Front-end constants — the divider ratio is the one measured on the board. */
#define ANALOG_OVERSAMPLES   (8u)                 /*!< reads averaged per channel */
#define BATT_DIVIDER_RATIO   (VBAT_DIVIDER_GAIN)  /*!< ~4.748 (56.06k / 14.711k)  */
#define WINDDIR_DEG_MAX      (360.0f)

/**
 * @brief  Prepare the analog block. The ADC itself is configured and calibrated
 *         by MX_ADC_Init(); this only resets the driver state.
 */
env_status_t analog_init(void);

/**
 * @brief  One pass over all four channels.
 * @param[out] soil_raw     soil-moisture ADC counts (0..4095)
 * @param[out] leaf_raw     leaf-wetness ADC counts (0..4095)
 * @param[out] batt_v       battery volts (= Vadc * BATT_DIVIDER_RATIO)
 * @param[out] winddir_deg  wind direction 0..359.9° (vane pot + north offset)
 * @retval ENV_OK if every channel converted, ENV_ERR otherwise (the channels
 *         that did succeed are still written).
 */
env_status_t analog_read_all(uint16_t *soil_raw, uint16_t *leaf_raw, float *batt_v, float *winddir_deg);

/**
 * @brief  Set/get the wind-vane north offset in degrees (downlink cmd 0x05).
 *         Added to the raw vane angle, then wrapped into 0..360.
 */
void  analog_set_winddir_offset(float deg);
float analog_get_winddir_offset(void);

#ifdef __cplusplus
}
#endif
#endif /* ANALOG_SENSORS_H */
