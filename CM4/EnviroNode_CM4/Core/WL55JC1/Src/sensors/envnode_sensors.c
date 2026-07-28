/**
  ******************************************************************************
  * @file    envnode_sensors.c
  * @brief   EnviroNode-WL55 sensor subsystem — top-level init / sample fan-out.
  *
  *          Owns the driver instances and merges their results into one
  *          sensor_readings_t. Skeleton: each driver call is stubbed / commented
  *          until CubeMX exposes the peripheral handles (Phase 1) and the
  *          drivers are implemented (Phase 2). See docs/ROADMAP.md.
  ******************************************************************************
  */

#include <string.h>
#include "sensors/envnode_sensors.h"
#include "sensors/bme280.h"
#include "sensors/max31865.h"
#include "sensors/analog_sensors.h"
#include "sensors/pulse_counter.h"

/* Phase 1: CubeMX will expose hi2c1/hi2c2/hspi1/hadc via i2c.h/spi.h/adc.h.
 * Uncomment the includes + handles once the .ioc is generated. */
/* #include "i2c.h"  #include "spi.h"  #include "adc.h" */

/* Driver instances (the two BME280s share one driver, different bus/addr). */
static bme280_t s_air1;   /* I2C1 */
static bme280_t s_air2;   /* I2C2 */

env_status_t envnode_sensors_init(void)
{
  /* TODO(Phase1/2): bring up every driver with its CubeMX handle, e.g.
   *   bme280_init(&s_air1, &hi2c1, BME280_ADDR_PRIMARY);
   *   bme280_init(&s_air2, &hi2c2, BME280_ADDR_PRIMARY);
   *   max31865_init(&hspi1, MAX_CS_GPIO_Port, MAX_CS_Pin, MAX31865_WIRES_3);
   *   analog_init();
   *   pulse_counter_init();
   * Return ENV_ERR if any required driver fails. */
  (void)s_air1; (void)s_air2;
  return ENV_NOTIMPL;
}

env_status_t envnode_sensors_sample(sensor_readings_t *out)
{
  if (out == NULL) return ENV_ERR;
  memset(out, 0, sizeof(*out));

  /* Each block: call the driver; on success set fields + OK-bit, else SENS_FAULT.
   * A single sensor failing must NOT abort the frame (see header contract). */

  /* --- air block A (BME280 #1, I2C1) --- */
  /* if (bme280_read(&s_air1, &out->air1_temp_c, &out->air1_rh_pct, &out->air1_press_hpa) == ENV_OK)
   *      out->status |= SENS_OK_AIR1; else out->status |= SENS_FAULT; */

  /* --- air block B (BME280 #2, I2C2) --- */
  /* if (bme280_read(&s_air2, &out->air2_temp_c, &out->air2_rh_pct, &out->air2_press_hpa) == ENV_OK)
   *      out->status |= SENS_OK_AIR2; else out->status |= SENS_FAULT; */

  /* --- soil temperature (PT1000 / MAX31865) --- */
  /* if (max31865_read_celsius(&out->soil_temp_c) == ENV_OK)
   *      out->status |= SENS_OK_PT1000; else out->status |= SENS_FAULT; */

  /* --- analog block (soil / leaf / battery / wind-dir) --- */
  /* if (analog_read_all(&out->soil_moist_raw, &out->leaf_wet_raw, &out->batt_v, &out->wind_dir_deg) == ENV_OK)
   *      out->status |= (SENS_OK_SOIL | SENS_OK_LEAF); else out->status |= SENS_FAULT;
   * (wind-dir OK-bit is folded into SENS_OK_WIND with the pulse block.) */

  /* --- pulse block (rain + wind speed/gust) --- */
  /* if (pulse_read_and_reset(&out->rain_tips, &out->rain_mm, &out->wind_speed_ms, &out->wind_gust_ms) == ENV_OK)
   *      out->status |= (SENS_OK_RAIN | SENS_OK_WIND); else out->status |= SENS_FAULT; */

  /* battery voltage is always packed even with no OK-bit (see PAYLOAD.md). */
  return ENV_NOTIMPL;   /* TODO(Phase2): return ENV_OK once drivers are wired */
}
