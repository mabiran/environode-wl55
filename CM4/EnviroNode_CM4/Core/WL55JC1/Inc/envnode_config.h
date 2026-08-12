/**
  ******************************************************************************
  * @file    envnode_config.h
  * @brief   Runtime configuration of the node — the state the FPort-10 downlink
  *          commands write (docs/PAYLOAD.md) and the sampler reads.
  *
  *          Everything here is persisted in its own flash page (page 62, kept
  *          separate from the key page so a config write can never endanger the
  *          OTAA identity) and reloaded at boot, so a node keeps its interval
  *          and calibration through a power cut.
  ******************************************************************************
  */
#ifndef ENVNODE_CONFIG_H
#define ENVNODE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "envnode_sensorset.h"   /* SENSOR_* bits + the 1..999 interval bounds */

/* The interval bounds are the sensor-set SPEC's, not a second opinion: the
   console, the downlink command table and the brace parser must all accept
   exactly the same range or a value set by one path would be clamped by
   another. */
#define ENVCFG_INTERVAL_MIN_DEFAULT   (ENVSET_INTERVAL_DEF)  /*!< 1 min (bench)*/
#define ENVCFG_INTERVAL_MIN_MIN       (ENVSET_INTERVAL_MIN)  /*!< 1 min.       */
#define ENVCFG_INTERVAL_MIN_MAX       (ENVSET_INTERVAL_MAX)  /*!< 999 min.     */
#define ENVCFG_CAL_SLOTS              (9u)    /*!< sensor_id 1..8 (index 0 unused) */

/** @name sensor_id values for set_cal (docs/PAYLOAD.md) @{ */
#define ENVCFG_CAL_AIR1_T    (1u)
#define ENVCFG_CAL_AIR1_RH   (2u)
#define ENVCFG_CAL_AIR2_T    (3u)
#define ENVCFG_CAL_AIR2_RH   (4u)
#define ENVCFG_CAL_SOIL      (5u)
#define ENVCFG_CAL_LEAF      (6u)
#define ENVCFG_CAL_SOIL_T    (7u)
#define ENVCFG_CAL_WIND_DIR  (8u)
/** @} */

/** Default selection: measure everything (SENSOR_* bits, envnode_sensorset.h). */
/* Default = the sensors actually fitted to the first node: the two BME280s,
   the Decagon LWS leaf-wetness sensor on A0 and the Decagon 10HS soil-moisture
   probe on A1. A default of SENSOR_ALL would raise a fault bit for every probe
   that is not wired yet, which makes the fault bit meaningless. Widen it with
   `{ALL,15}` as the rest of the harness is built (docs/CONFIG.md). */
#define ENVCFG_SENSOR_MASK_DEFAULT   (SENSOR_T1 | SENSOR_T2 | SENSOR_LW | SENSOR_SM)

/** @brief Load config from flash, or apply defaults when nothing is stored. */
void envnode_config_init(void);

/** @brief Write the current config back to its flash page. 1 = success. */
int  envnode_config_save(void);

/** @brief Restore defaults in RAM and erase the stored copy. */
void envnode_config_reset(void);

/* --- uplink interval ----------------------------------------------------- */
uint16_t envnode_config_get_interval_min(void);
void     envnode_config_set_interval_min(uint16_t minutes);   /* clamped */

/* --- one-shot uplink request (downlink cmd 0x02 / console) --------------- */
void envnode_config_request_uplink(void);
int  envnode_config_take_uplink_request(void);   /* 1 if one was pending */

/* --- per-sensor calibration offsets (cmd 0x04), in x100 units ------------ */
int16_t envnode_config_get_cal(uint8_t sensor_id);
void    envnode_config_set_cal(uint8_t sensor_id, int16_t offset_x100);

/* --- sensor selection mask (cmd 0x06 / the {…} config string) ------------
   8-bit SENSOR_* selection from envnode_sensorset.h. This replaced the old
   u16 "enable mask", whose bits were the SENS_OK_* status bits — they are NOT
   the same numbering, which is why the flash record magic was bumped rather
   than reused (a stale record must fall back to defaults, never be reinterpreted
   as a different sensor set). */
uint8_t  envnode_config_get_sensor_mask(void);
void     envnode_config_set_sensor_mask(uint8_t mask);

/* --- wind-vane north offset (cmd 0x05), degrees x10 --------------------- */
uint16_t envnode_config_get_winddir_offset_deg10(void);
void     envnode_config_set_winddir_offset_deg10(uint16_t deg10);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_CONFIG_H */
