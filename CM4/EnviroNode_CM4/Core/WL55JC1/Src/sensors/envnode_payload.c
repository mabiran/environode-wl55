/**
  ******************************************************************************
  * @file    envnode_payload.c
  * @brief   EnviroNode-WL55 LoRaWAN payload codec — implementation.
  *
  *          Uplink packer and the FPort-10 downlink command table are both
  *          implemented per docs/PAYLOAD.md (v1). Commands that change
  *          configuration write it through to the flash config page, so a
  *          remotely-set interval or calibration survives a power cut.
  ******************************************************************************
  */

#include "sensors/envnode_payload.h"
#include "sensors/analog_sensors.h"
#include "sensors/pulse_counter.h"
#include "envnode_config.h"
#include "envnode_sensorset.h"      /* {LW,T1,...} config-string grammar */
#include "stm32wlxx_hal.h"          /* NVIC_SystemReset */

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

/* --- sensor-set configuration string -------------------------------------- */

/** Append a NUL-terminated note to a reply buffer; silently truncates. */
static void reply_append(char *reply, size_t reply_sz, const char *s)
{
  if (reply == NULL || reply_sz == 0u) return;
  size_t n = 0u;
  while (n < reply_sz && reply[n] != '\0') ++n;
  for (size_t i = 0u; s[i] != '\0' && (n + 1u) < reply_sz; ++i) reply[n++] = s[i];
  if (n < reply_sz) reply[n] = '\0';
}

int envnode_apply_config_string(const char *text, size_t len,
                                char *reply, size_t reply_sz)
{
  uint8_t  new_mask     = 0u;
  uint16_t new_interval = 0u;
  char     err[ENVSET_ERR_MAX];

  if (reply != NULL && reply_sz > 0u) reply[0] = '\0';
  err[0] = '\0';

  const uint8_t  cur_mask     = envnode_config_get_sensor_mask();
  const uint16_t cur_interval = envnode_config_get_interval_min();

  envset_result_t rc = envnode_sensorset_parse_n(text, len, cur_mask, cur_interval,
                                                 &new_mask, &new_interval,
                                                 err, sizeof(err));
  if (rc == ENVSET_REJECTED) {
    reply_append(reply, reply_sz, err);
    return ENV_ERR;                    /* nothing applied, no flash written    */
  }

  if (rc == ENVSET_ACCEPTED) {
    const int changed = (new_mask != cur_mask) || (new_interval != cur_interval);
    if (changed) {
      envnode_config_set_sensor_mask(new_mask);
      envnode_config_set_interval_min(new_interval);
    }
    /* Render from the config, not from the parse result, so the echo is proof
       of what the node actually holds. */
    if (reply != NULL && reply_sz > 0u) {
      (void)envnode_sensorset_format(envnode_config_get_sensor_mask(),
                                     envnode_config_get_interval_min(),
                                     reply, reply_sz);
    }
    if (changed) {
      reply_append(reply, reply_sz, envnode_config_save() ? " saved" : " NOT SAVED");
    } else {
      reply_append(reply, reply_sz, " unchanged");
    }
    return ENV_OK;
  }

  /* ENVSET_QUERY — "{?}" reports and touches nothing. */
  if (reply != NULL && reply_sz > 0u) {
    (void)envnode_sensorset_format(cur_mask, cur_interval, reply, reply_sz);
  }
  return ENV_OK;
}

int envnode_downlink_apply(uint8_t fport, const uint8_t *buf, size_t len)
{
  if (buf == NULL || len < 1u) return ENV_ERR;

  /* A config string is legal on ANY FPort: it is self-describing, so there is
     no reason to make the operator pick the right port to reconfigure a node
     that may be a long walk away. Checked first — the binary table below has no
     command id 0x7B, so nothing is shadowed. */
  if (buf[0] == (uint8_t)ENVNODE_CFGSTR_MARKER) {
    return envnode_apply_config_string((const char *)buf, len, NULL, 0u);
  }

  if (fport != ENVNODE_DOWNLINK_FPORT) return ENV_ERR;

  const uint8_t cmd = buf[0];
  const uint8_t *arg = &buf[1];
  const size_t   arglen = len - 1u;

  /* Little-endian argument helper (matches the uplink encoding). */
  #define ARG_U16(i)  ((uint16_t)((uint16_t)arg[(i)] | ((uint16_t)arg[(i) + 1u] << 8)))

  switch (cmd) {
    case ENVNODE_CMD_SET_INTERVAL: {     /* u16 minutes  -> set sample period  */
      if (arglen < 2u) return ENV_ERR;
      /* Compare after the setter's clamp, and only then erase: a gateway that
         re-sends the same interval must not cost a flash page each time. */
      const uint16_t prev = envnode_config_get_interval_min();
      envnode_config_set_interval_min(ARG_U16(0));
      if (envnode_config_get_interval_min() != prev) (void)envnode_config_save();
      return ENV_OK;
    }

    case ENVNODE_CMD_UPLINK_NOW:         /* trigger an immediate sample+uplink */
      /* Deferred to the main loop: an uplink must not run inside the downlink
         drain, which itself runs from the mailbox service path. */
      envnode_config_request_uplink();
      return ENV_OK;

    case ENVNODE_CMD_RESET_RAIN:         /* zero the rain-tip accumulator      */
      pulse_reset_rain();
      return ENV_OK;

    case ENVNODE_CMD_SET_CAL: {          /* u8 sensor_id, i16 offset x100      */
      if (arglen < 3u) return ENV_ERR;
      uint8_t sensor_id = arg[0];
      int16_t offset    = (int16_t)ARG_U16(1);
      if (sensor_id == 0u || sensor_id >= ENVCFG_CAL_SLOTS) return ENV_ERR;
      envnode_config_set_cal(sensor_id, offset);
      (void)envnode_config_save();
      return ENV_OK;
    }

    case ENVNODE_CMD_SET_WINDDIR_OFFSET: /* u16 deg x10                        */
      if (arglen < 2u) return ENV_ERR;
      envnode_config_set_winddir_offset_deg10(ARG_U16(0));
      analog_set_winddir_offset((float)envnode_config_get_winddir_offset_deg10() / 10.0f);
      (void)envnode_config_save();
      return ENV_OK;

    case ENVNODE_CMD_SET_ENABLE: {       /* u8 SENSOR_* selection mask         */
      /* The mask is now 8 bits (SENSOR_LW..SENSOR_R, envnode_sensorset.h), one
         per sensor. Senders that still emit the old little-endian u16 keep
         working: the selection is the low byte either way, and the high byte
         was always discarded. */
      if (arglen < 1u) return ENV_ERR;
      if (arg[0] == envnode_config_get_sensor_mask()) return ENV_OK;  /* no write */
      envnode_config_set_sensor_mask(arg[0]);
      (void)envnode_config_save();
      return ENV_OK;
    }

    case ENVNODE_CMD_REBOOT:             /* software reset                     */
      NVIC_SystemReset();
      return ENV_OK;                     /* not reached                        */

    case ENVNODE_CMD_GET_CONFIG:         /* echo config in next diag uplink    */
      /* The config is reported by the console `info` command today; a FPort-2
         diagnostic uplink is still to come (docs/PAYLOAD.md, "Auxiliary"). */
      return ENV_NOTIMPL;

    default:
      return ENV_ERR;                    /* unknown command id                 */
  }
  #undef ARG_U16
}
