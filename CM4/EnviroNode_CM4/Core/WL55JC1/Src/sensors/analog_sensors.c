/**
  ******************************************************************************
  * @file    analog_sensors.c
  * @brief   ADC sensor block: soil moisture, leaf wetness, battery, wind vane.
  *
  *          Channels are converted one at a time (adc.c re-points sequencer
  *          rank 1 per read), each averaged over ANALOG_OVERSAMPLES samples.
  *          A failing channel does not abort the others — the caller gets
  *          ENV_ERR but every channel that did convert is still written, so a
  *          single unplugged probe never blanks the whole frame.
  ******************************************************************************
  */
#include "sensors/analog_sensors.h"
#include "adc.h"

static uint8_t s_ready;
static float   s_winddir_offset_deg;   /*!< vane north alignment (cmd 0x05).  */

/* Convert raw counts to volts at the pin. */
static inline float counts_to_v(uint16_t counts)
{
  return ((float)counts / ADC_FULL_SCALE) * ADC_VREF_VOLT;
}

/* Wrap any angle into [0, 360). */
static float wrap360(float deg)
{
  while (deg >= 360.0f) deg -= 360.0f;
  while (deg <    0.0f) deg += 360.0f;
  return deg;
}

env_status_t analog_init(void)
{
  /* MX_ADC_Init() already configured resolution, prescaler, sampling time and
     ran the self-calibration; the channels are selected per read. */
  s_winddir_offset_deg = 0.0f;
  s_ready = 1u;
  return ENV_OK;
}

env_status_t analog_read_all(uint16_t *soil_raw, uint16_t *leaf_raw, float *batt_v, float *winddir_deg)
{
  if (!s_ready || soil_raw == NULL || leaf_raw == NULL || batt_v == NULL || winddir_deg == NULL) return ENV_ERR;

  env_status_t rc = ENV_OK;
  uint16_t raw = 0u;

  /* Soil moisture — raw counts; the probe's curve is applied off-node. */
  if (ADC_ReadChannelAvg(ENVNODE_ADC_CH_SOIL, ANALOG_OVERSAMPLES, &raw) == HAL_OK) {
    *soil_raw = raw;
  } else {
    *soil_raw = 0u; rc = ENV_ERR;
  }

  /* Leaf wetness — raw counts (dry/wet reference calibration is off-node). */
  if (ADC_ReadChannelAvg(ENVNODE_ADC_CH_LEAF, ANALOG_OVERSAMPLES, &raw) == HAL_OK) {
    *leaf_raw = raw;
  } else {
    *leaf_raw = 0u; rc = ENV_ERR;
  }

  /* Battery through the resistor divider. */
  if (ADC_ReadChannelAvg(ENVNODE_ADC_CH_BATT, ANALOG_OVERSAMPLES, &raw) == HAL_OK) {
    *batt_v = counts_to_v(raw) * BATT_DIVIDER_RATIO;
  } else {
    *batt_v = 0.0f; rc = ENV_ERR;
  }

  /* Wind vane potentiometer: full travel maps to a full turn, then the north
     offset is applied and the result wrapped. */
  if (ADC_ReadChannelAvg(ENVNODE_ADC_CH_WINDDIR, ANALOG_OVERSAMPLES, &raw) == HAL_OK) {
    float deg = ((float)raw / ADC_FULL_SCALE) * WINDDIR_DEG_MAX;
    *winddir_deg = wrap360(deg + s_winddir_offset_deg);
  } else {
    *winddir_deg = 0.0f; rc = ENV_ERR;
  }

  return rc;
}

void analog_set_winddir_offset(float deg)
{
  s_winddir_offset_deg = wrap360(deg);
}

float analog_get_winddir_offset(void)
{
  return s_winddir_offset_deg;
}
