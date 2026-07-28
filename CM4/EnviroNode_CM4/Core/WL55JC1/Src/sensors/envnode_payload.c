/**
  ******************************************************************************
  * @file    envnode_payload.c
  * @brief   EnviroNode-WL55 LoRaWAN payload codec — implementation.
  *
  *          Uplink packer is FULLY implemented per docs/PAYLOAD.md (v1).
  *          Downlink dispatch is a skeleton switch over the FPort-10 command
  *          table (Phase 4 wires each command to config + backup-register
  *          persistence).
  ******************************************************************************
  */

#include "sensors/envnode_payload.h"

/* --- little-endian byte helpers ------------------------------------------- */
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static inline void put_i16(uint8_t *p, int16_t v)  { put_u16(p, (uint16_t)v); }

/* --- saturating round from float to fixed-width --------------------------- */
static inline uint16_t sat_u16(float x) {
  if (x <= 0.0f) return 0u;
  if (x >= 65535.0f) return 65535u;
  return (uint16_t)(x + 0.5f);
}
static inline uint8_t sat_u8(float x) {
  if (x <= 0.0f) return 0u;
  if (x >= 255.0f) return 255u;
  return (uint8_t)(x + 0.5f);
}
static inline int16_t sat_i16(float x) {
  if (x <= -32768.0f) return (int16_t)-32768;
  if (x >=  32767.0f) return (int16_t) 32767;
  return (int16_t)(x < 0.0f ? (x - 0.5f) : (x + 0.5f));
}

size_t envnode_payload_pack(const sensor_readings_t *r, uint8_t *buf, size_t buflen)
{
  if (r == NULL || buf == NULL || buflen < ENVNODE_UPLINK_LEN) {
    return 0u;
  }
  const uint8_t st = r->status;

  buf[0] = ENVNODE_FMT_UPLINK;          /* off 0  : fmt                        */
  buf[1] = st;                          /* off 1  : status bitfield            */
  put_u16(&buf[2], sat_u16(r->batt_v * 1000.0f)); /* off 2 : batt mV (always)  */

  /* off 4  : air block A (BME280 #1) */
  if (st & SENS_OK_AIR1) {
    put_i16(&buf[4], sat_i16(r->air1_temp_c   * 100.0f));
    buf[6] =         sat_u8 (r->air1_rh_pct   *   2.0f);
    put_u16(&buf[7], sat_u16(r->air1_press_hpa*  10.0f));
  } else {
    put_i16(&buf[4], ENVNODE_SENTINEL_I16); buf[6] = ENVNODE_SENTINEL_U8; put_u16(&buf[7], ENVNODE_SENTINEL_U16);
  }

  /* off 9  : air block B (BME280 #2) */
  if (st & SENS_OK_AIR2) {
    put_i16(&buf[9],  sat_i16(r->air2_temp_c   * 100.0f));
    buf[11] =         sat_u8 (r->air2_rh_pct   *   2.0f);
    put_u16(&buf[12], sat_u16(r->air2_press_hpa*  10.0f));
  } else {
    put_i16(&buf[9], ENVNODE_SENTINEL_I16); buf[11] = ENVNODE_SENTINEL_U8; put_u16(&buf[12], ENVNODE_SENTINEL_U16);
  }

  /* off 14 : soil moisture / off 16 : leaf wetness (raw ADC / permille) */
  put_u16(&buf[14], (st & SENS_OK_SOIL) ? r->soil_moist_raw : ENVNODE_SENTINEL_U16);
  put_u16(&buf[16], (st & SENS_OK_LEAF) ? r->leaf_wet_raw   : ENVNODE_SENTINEL_U16);

  /* off 18 : soil temperature (PT1000) */
  put_i16(&buf[18], (st & SENS_OK_PT1000) ? sat_i16(r->soil_temp_c * 100.0f) : ENVNODE_SENTINEL_I16);

  /* off 20 : wind speed / dir / gust */
  if (st & SENS_OK_WIND) {
    put_u16(&buf[20], sat_u16(r->wind_speed_ms * 100.0f));
    put_u16(&buf[22], sat_u16(r->wind_dir_deg  *  10.0f));
    put_u16(&buf[24], sat_u16(r->wind_gust_ms  * 100.0f));
  } else {
    put_u16(&buf[20], ENVNODE_SENTINEL_U16); put_u16(&buf[22], ENVNODE_SENTINEL_U16); put_u16(&buf[24], ENVNODE_SENTINEL_U16);
  }

  /* off 26 : rain tips / off 28 : rain mm */
  if (st & SENS_OK_RAIN) {
    put_u16(&buf[26], r->rain_tips);
    put_u16(&buf[28], sat_u16(r->rain_mm * 100.0f));
  } else {
    put_u16(&buf[26], ENVNODE_SENTINEL_U16); put_u16(&buf[28], ENVNODE_SENTINEL_U16);
  }

  return ENVNODE_UPLINK_LEN;            /* always 30 on success                */
}

int envnode_downlink_apply(uint8_t fport, const uint8_t *buf, size_t len)
{
  if (fport != ENVNODE_DOWNLINK_FPORT) return ENV_ERR;
  if (buf == NULL || len < 1u)         return ENV_ERR;

  const uint8_t cmd = buf[0];
  const uint8_t *arg = &buf[1];
  const size_t   arglen = len - 1u;
  (void)arg; (void)arglen;

  switch (cmd) {
    case ENVNODE_CMD_SET_INTERVAL:       /* u16 minutes  -> set sample period  */
      /* TODO(Phase4): if (arglen >= 2) set_interval_min(arg[0] | arg[1]<<8); persist BKP. */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_UPLINK_NOW:         /* trigger an immediate sample+uplink */
      /* TODO(Phase4): request_uplink_now(); */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_RESET_RAIN:         /* zero the rain-tip accumulator      */
      /* TODO(Phase4): pulse_reset_rain(); */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_SET_CAL:            /* u8 sensor_id, i16 offset x100      */
      /* TODO(Phase4): apply + persist calibration offset. */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_SET_WINDDIR_OFFSET: /* u16 deg x10                        */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_SET_ENABLE:         /* u16 sensor enable mask             */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_REBOOT:             /* software reset                     */
      /* TODO(Phase4): NVIC_SystemReset(); */
      return ENV_NOTIMPL;
    case ENVNODE_CMD_GET_CONFIG:         /* echo config in next diag uplink    */
      return ENV_NOTIMPL;
    default:
      return ENV_ERR;                    /* unknown command id                 */
  }
}
