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
#include "sensors/pulse_counter.h"   /* ANEMO_MS_PER_HZ, WIND_DEBOUNCE_MS */
#include "sensors/max31865.h"        /* pt1000_ohms_to_celsius() — math only */
#include "adc.h"

static uint8_t s_ready;
static float   s_winddir_offset_deg;   /*!< vane north alignment (cmd 0x05).  */

/**
 * @brief  Switch the sensor excitation rail (VSENS) on or off.
 *
 * The Decagon LWS is explicitly intended for loggers that "provide short
 * excitation pulses" — it has no internal regulator and draws ~4 mA, so leaving
 * it powered costs ~91 mAh/day, several times the rest of the node. The Davis
 * vane pot adds another 165 uA. Both hang off this one rail (docs/PINOUT.md).
 *
 * If no switch is fitted yet, the sensors are simply powered from the Grove
 * socket's permanent VCC and this call is a harmless no-op as far as they are
 * concerned — so firmware and hardware can be updated in either order.
 */
static void analog_rail(int on)
{
#if ENV_SENSPWR_ACTIVE_HIGH
  HAL_GPIO_WritePin(ENV_SENSPWR_Port, ENV_SENSPWR_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(ENV_SENSPWR_Port, ENV_SENSPWR_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

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

  /* Power the excitation rail and let it settle before converting anything.
     The LWS and the 10HS both specify a 10 ms minimum excitation/measurement
     time; sampling early returns a partially-settled value that looks like a
     plausible dry reading, which is the worst kind of wrong.
     ANALOG_RAIL_SETTLE_MS carries margin over that. */
  analog_rail(1);
  HAL_Delay(ANALOG_RAIL_SETTLE_MS);

  /* Soil moisture — Decagon 10HS on A1. Its onboard regulator makes the
     output (300..1250 mV) independent of excitation, so raw counts convert to
     mV directly (mV = counts*3300/4095); Decagon's mineral-soil polynomial is
     applied off-node. NOTE it draws ~12 mA while excited — a bare GPIO rail
     sags under that, so the probe's supply must come from the high-side
     switch or 3V3 direct, never from the PB5 pin itself (docs/SENSORS.md). */
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

  /* Excitation off again — the whole point of the rail. */
  analog_rail(0);

  return rc;
}

env_status_t analog_wind_burst(float *speed_ms, float *gust_ms)
{
  if (!s_ready || speed_ms == NULL || gust_ms == NULL) return ENV_ERR;

  const uint32_t t0 = HAL_GetTick();
  uint32_t closures = 0u;
  uint32_t last_edge_ms = 0u;
  uint8_t  state_high = 1u;          /* pull-up holds the line high when open */
  int      first = 1;
  env_status_t rc = ENV_OK;

  /* Sample the contact for the whole window, counting high->low transitions —
     one per revolution. Hysteresis rejects the ~1 ms of mechanical bounce that a
     bare threshold would count several times, and the debounce floor rejects the
     rest. Nothing here touches the excitation rail: the contact is a passive
     switch with a pull-up at the connector. */
  while ((uint32_t)(HAL_GetTick() - t0) < ANEMO_BURST_MS) {
    uint16_t raw = 0u;
    if (ADC_ReadChannel(ENVNODE_ADC_CH_WINDSPD, &raw) != HAL_OK) {
      rc = ENV_ERR;
      break;
    }

    const uint32_t now = HAL_GetTick();
    if (first) {                      /* adopt the starting level, do not count it */
      state_high = (raw > ANEMO_LO_COUNTS) ? 1u : 0u;
      first = 0;
    } else if (state_high && raw < ANEMO_LO_COUNTS) {
      /* falling edge = contact closed */
      if (closures == 0u || (uint32_t)(now - last_edge_ms) >= WIND_DEBOUNCE_MS) {
        closures++;
        last_edge_ms = now;
      }
      state_high = 0u;
    } else if (!state_high && raw > ANEMO_HI_COUNTS) {
      state_high = 1u;                /* released — arms the next closure */
    }

    /* ~1 kHz. The conversion itself is ~40 us, so this delay dominates. */
    uint32_t tick = HAL_GetTick();
    while ((HAL_GetTick() - tick) < (ANEMO_SAMPLE_US / 1000u)) { /* 1 ms */ }
  }

  const float window_s = (float)ANEMO_BURST_MS / 1000.0f;
  const float hz       = (float)closures / window_s;

  *speed_ms = hz * ANEMO_MS_PER_HZ;
  /* The burst window IS the 3-second gust window, so the two coincide. Kept as a
     separate output so the frame layout is unchanged and a future EXTI-based
     implementation can report a real peak. */
  *gust_ms  = *speed_ms;

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

env_status_t analog_rtd_a2_celsius(float *t_c)
{
  if (!s_ready || t_c == NULL) return ENV_ERR;

  /* A2 (PA10) normally belongs to I2C1 as SDA. Borrow it: analog mode has no
     output driver, so the swap is glitch-free for the (idle, single-threaded)
     bus, and the AF is restored below exactly as i2c.c configured it. */
  GPIO_InitTypeDef g = {0};
  g.Pin  = GPIO_PIN_10;
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &g);

  uint16_t raw = 0u;
  HAL_StatusTypeDef hrc = ADC_ReadChannelAvg(ENVNODE_ADC_CH_SOILTEMP, ANALOG_OVERSAMPLES, &raw);

  g.Mode      = GPIO_MODE_AF_OD;      /* hand the pin back to I2C1 (i2c.c)     */
  g.Speed     = GPIO_SPEED_FREQ_LOW;
  g.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(GPIOA, &g);

  if (hrc != HAL_OK) return ENV_ERR;

  /* Open probe: the series resistor pulls the pin to the rail. Short: to GND.
     Both would divide by ~0 or produce nonsense — reject before the math. */
  if (raw >= 4050u || raw <= 50u) return ENV_ERR;

  /* Ratiometric: the divider's top rail is the ADC reference, so the rail
     voltage cancels and only the series resistor's value matters. */
  const float r_rtd = ANALOG_RTD_SERIES_OHMS * (float)raw / (float)(ADC_FULL_SCALE - (float)raw);

  return pt1000_ohms_to_celsius(r_rtd, t_c);
}
