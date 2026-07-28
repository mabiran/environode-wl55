/**
  ******************************************************************************
  * @file    analog_sensors.c
  * @brief   ADC sensors driver — skeleton (Phase 2 wires the ADC scan).
  ******************************************************************************
  */
#include "sensors/analog_sensors.h"

/* Phase 1: CubeMX exposes hadc via "adc.h". Enable 4 channels (soil, leaf,
 * battery, wind-dir) in a scan, or read them sequentially. */
/* #include "adc.h" */

static uint8_t s_ready;

/* Convert raw counts to volts at the pin. */
static inline float counts_to_v(uint16_t counts) { return ((float)counts / ADC_FULL_SCALE) * ADC_VREF_V; }

env_status_t analog_init(void)
{
  /* TODO(Phase1/2): HAL_ADCEx_Calibration_Start(&hadc, ...); configure the
   * channel sequence + sampling time (long sampling for the high-impedance
   * soil/leaf probes). */
  s_ready = 1u;
  return ENV_NOTIMPL;
}

env_status_t analog_read_all(uint16_t *soil_raw, uint16_t *leaf_raw, float *batt_v, float *winddir_deg)
{
  if (!s_ready || soil_raw == NULL || leaf_raw == NULL || batt_v == NULL || winddir_deg == NULL) return ENV_ERR;

  /* TODO(Phase2):
   *  - trigger the scan / read each channel (HAL_ADC_Start, poll, HAL_ADC_GetValue)
   *  - soil/leaf: return raw counts (curve applied off-node or via set_cal)
   *  - battery : *batt_v = counts_to_v(batt_counts) * BATT_DIVIDER_RATIO
   *  - wind dir: *winddir_deg = (counts / 4095) * 360 + vane_offset, wrapped 0..360
   */
  (void)counts_to_v;
  *soil_raw = 0u; *leaf_raw = 0u; *batt_v = 0.0f; *winddir_deg = 0.0f;
  return ENV_NOTIMPL;
}
