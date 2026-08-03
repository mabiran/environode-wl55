/**
  ******************************************************************************
  * @file    envnode_sensors.c
  * @brief   EnviroNode-WL55 sensor subsystem — top-level init / sample fan-out.
  *
  *          Owns the driver instances and merges their results into one
  *          sensor_readings_t. Every block is independent: a driver that fails
  *          clears its own OK-bit and raises SENS_FAULT, but never aborts the
  *          frame, so one unplugged probe still yields a valid uplink.
  *
  *          Bus map (docs/PINOUT.md):
  *            I2C2 (PA12/PA11, Grove shield) : BME280 #1 "air1" + INA219
  *            I2C1 (PA9/PA10, board pins)    : BME280 #2 "air2"
  *            SPI1 (PA5/6/7, CS PA4)         : MAX31865 PT1000
  *            ADC  (PB1/PB2/PB4/PB14)        : soil, leaf, vane, battery
  *            EXTI (PB3, PB5)                : rain tips, anemometer
  ******************************************************************************
  */

#include <string.h>
#include "sensors/envnode_sensors.h"
#include "sensors/bme280.h"
#include "sensors/max31865.h"
#include "sensors/analog_sensors.h"
#include "sensors/pulse_counter.h"

#include "i2c.h"          /* hi2c1, hi2c2 */
#include "spi.h"          /* hspi1        */
#include "pins_config.h"  /* ENV_RTD_CS_*, INA219_I2C_ADDR_7B, SHUNT_OHMS */
#include "ina219.h"
#include "envnode_config.h"      /* sensor selection + calibration offsets */
#include "envnode_sensorset.h"   /* SENSOR_* bits -> SENS_OK_* gate mask   */

/* Wire count of the PT1000 probe — change to match the harness. */
#ifndef ENVNODE_RTD_WIRES
#define ENVNODE_RTD_WIRES   MAX31865_WIRES_3
#endif

/* Driver instances (the two BME280s share one driver, different bus/addr). */
static bme280_t s_air1;   /* I2C2 — Grove shield socket */
static bme280_t s_air2;   /* I2C1 — board pins          */
static INA219_t s_ina;    /* I2C2 — battery V/I monitor */

/* Which drivers came up (mirrors the SENS_OK_* bit layout where it overlaps). */
static uint8_t s_present;
#define PRESENT_AIR1    (0x01u)
#define PRESENT_AIR2    (0x02u)
#define PRESENT_RTD     (0x04u)
#define PRESENT_ANALOG  (0x08u)
#define PRESENT_PULSE   (0x10u)
#define PRESENT_INA219  (0x20u)

env_status_t envnode_sensors_init(void)
{
  env_status_t rc = ENV_OK;
  s_present = 0u;

  /* Air block A — BME280 on the shield's Grove I2C socket (I2C2). */
  if (bme280_init_autoaddr(&s_air1, &hi2c2) == ENV_OK) s_present |= PRESENT_AIR1;
  else rc = ENV_ERR;

  /* Air block B — BME280 hand-wired to the board pins (I2C1). */
  if (bme280_init_autoaddr(&s_air2, &hi2c1) == ENV_OK) s_present |= PRESENT_AIR2;
  else rc = ENV_ERR;

  /* Soil temperature — PT1000 through the MAX31865 on SPI1. */
  if (max31865_init(&hspi1, ENV_RTD_CS_Port, ENV_RTD_CS_Pin, ENVNODE_RTD_WIRES) == ENV_OK) s_present |= PRESENT_RTD;
  else rc = ENV_ERR;

  /* Analog block — soil / leaf / vane / battery divider. */
  if (analog_init() == ENV_OK) s_present |= PRESENT_ANALOG;
  else rc = ENV_ERR;

  /* Rain + wind pulse counters (EXTI lines are armed in MX_GPIO_Init). */
  pulse_counter_init();
  s_present |= PRESENT_PULSE;

  /* Battery monitor — optional; the ADC divider is the fallback. */
  if (INA219_Init(&s_ina, &hi2c2, INA219_I2C_ADDR_7B, SHUNT_OHMS)) s_present |= PRESENT_INA219;

  return rc;
}

uint8_t envnode_sensors_present(void)
{
  return s_present;
}

/* Calibration offsets arrive as engineering units x100 (docs/PAYLOAD.md 0x04). */
static inline float cal_f(uint8_t sensor_id) { return (float)envnode_config_get_cal(sensor_id) / 100.0f; }

/* Additive offset for a raw-count channel, in counts, saturated to 0..4095. */
static inline uint16_t cal_counts(uint16_t raw, uint8_t sensor_id)
{
  int32_t v = (int32_t)raw + (envnode_config_get_cal(sensor_id) / 100);
  if (v < 0)    v = 0;
  if (v > 4095) v = 4095;
  return (uint16_t)v;
}

static env_status_t envnode_sample_common(sensor_readings_t *out, int consume_pulses);

env_status_t envnode_sensors_sample(sensor_readings_t *out)
{
  return envnode_sample_common(out, 1);
}

env_status_t envnode_sensors_peek(sensor_readings_t *out)
{
  return envnode_sample_common(out, 0);
}

static env_status_t envnode_sample_common(sensor_readings_t *out, int consume_pulses)
{
  if (out == NULL) return ENV_ERR;
  memset(out, 0, sizeof(*out));

  /* A sensor left out of the selection ({…} config string or downlink 0x06) is
     skipped entirely: its OK-bit stays clear (the decoder shows "no data") but
     it raises no fault, because nothing is broken.
     `sel` is the SENSOR_* selection; `en` is that same selection translated
     into the on-air SENS_OK_* bits the frame is allowed to set. */
  const uint8_t sel = envnode_config_get_sensor_mask();
  const uint8_t en  = envnode_sensorset_to_okmask(sel);

  /* --- air block A (BME280 #1, I2C2 / shield) --- */
  if (en & SENS_OK_AIR1) {
    if ((s_present & PRESENT_AIR1) &&
        bme280_read(&s_air1, &out->air1_temp_c, &out->air1_rh_pct, &out->air1_press_hpa) == ENV_OK) {
      out->air1_temp_c += cal_f(ENVCFG_CAL_AIR1_T);
      out->air1_rh_pct += cal_f(ENVCFG_CAL_AIR1_RH);
      out->status |= SENS_OK_AIR1;
    } else {
      out->status |= SENS_FAULT;
    }
  }

  /* --- air block B (BME280 #2, I2C1 / board pins) --- */
  if (en & SENS_OK_AIR2) {
    if ((s_present & PRESENT_AIR2) &&
        bme280_read(&s_air2, &out->air2_temp_c, &out->air2_rh_pct, &out->air2_press_hpa) == ENV_OK) {
      out->air2_temp_c += cal_f(ENVCFG_CAL_AIR2_T);
      out->air2_rh_pct += cal_f(ENVCFG_CAL_AIR2_RH);
      out->status |= SENS_OK_AIR2;
    } else {
      out->status |= SENS_FAULT;
    }
  }

  /* --- soil temperature (PT1000 / MAX31865) --- */
  if (en & SENS_OK_PT1000) {
    if ((s_present & PRESENT_RTD) && max31865_read_celsius(&out->soil_temp_c) == ENV_OK) {
      out->soil_temp_c += cal_f(ENVCFG_CAL_SOIL_T);
      out->status |= SENS_OK_PT1000;
    } else {
      out->status |= SENS_FAULT;
    }
  }

  /* --- analog block (soil / leaf / battery / wind-dir) ---
     Always converted, whatever the selection, because battery voltage has no
     OK-bit and is on every frame. analog_read_all() writes every channel it
     managed to convert even when it reports ENV_ERR, so the soil/leaf bits are
     set from the values themselves. */
  int analog_ok = 0;
  {
    uint16_t soil = 0u, leaf = 0u;
    float batt = 0.0f, winddir = 0.0f;
    env_status_t arc = analog_read_all(&soil, &leaf, &batt, &winddir);
    analog_ok = (arc == ENV_OK);

    out->soil_moist_raw = cal_counts(soil, ENVCFG_CAL_SOIL);
    out->leaf_wet_raw   = cal_counts(leaf, ENVCFG_CAL_LEAF);
    out->batt_v         = batt;
    out->wind_dir_deg   = winddir;   /* vane offset already applied in analog */

    if (arc == ENV_OK) {
      if (en & SENS_OK_SOIL) out->status |= SENS_OK_SOIL;
      if (en & SENS_OK_LEAF) out->status |= SENS_OK_LEAF;
    } else if ((sel & (SENSOR_SM | SENSOR_LW | SENSOR_WD)) ||
               !(s_present & PRESENT_INA219)) {
      /* Only a fault if the failure actually cost us data: some analog sensor
         was selected, or the ADC was our only way to read the battery. */
      out->status |= SENS_FAULT;
    }
  }

  /* --- battery: prefer the INA219 bus voltage over the divider --- */
  if (s_present & PRESENT_INA219) {
    float v_bus = 0.0f;
    if (INA219_ReadBusVoltage_V(&s_ina, &v_bus) && v_bus > 0.5f) {
      out->batt_v = v_bus;
    }
  }

  /* --- wind speed: ADC burst on the contact input -------------------------
     Only run when WS is actually selected: the burst blocks for ANEMO_BURST_MS
     (3 s), which is a real slice of the awake window and pure waste if nobody
     asked for wind. Left at zero otherwise. */
  int wind_ok = 1;
  if (sel & SENSOR_WS) {
    if (analog_wind_burst(&out->wind_speed_ms, &out->wind_gust_ms) != ENV_OK) {
      wind_ok = 0;
    }
  }

  /* --- pulse block (rain only) ---
     Always drained, even when rain is deselected, so the accumulator does not
     grow without bound; the OK-bits are what the selection gates. */
  if (pulse_read_stats(&out->rain_tips, &out->rain_mm, consume_pulses) == ENV_OK) {
    if (en & SENS_OK_RAIN) out->status |= SENS_OK_RAIN;
    /* Speed, direction and gust share ONE on-air OK-bit (docs/PAYLOAD.md), so
       selecting either WS or WD publishes the whole wind triplet. The vane is
       only required to have read when WD is actually selected — otherwise a
       WS-only node would be silenced by an unplugged vane, and a WD node would
       report a direction of 0° that nobody measured. Keyed on the analog
       block's own result, not on the soil OK-bit, so switching soil moisture
       off does not take wind direction down with it. */
    if ((en & SENS_OK_WIND) && wind_ok &&
        (!(sel & SENSOR_WD) || analog_ok)) {
      out->status |= SENS_OK_WIND;
    }
  } else if (en & (SENS_OK_RAIN | SENS_OK_WIND)) {
    out->status |= SENS_FAULT;   /* only a fault if we were meant to report it */
  }

  return ENV_OK;
}
