/**
  ******************************************************************************
  * @file    envnode_payload.h
  * @brief   EnviroNode-WL55 LoRaWAN payload codec (uplink packer + downlink
  *          command dispatch).
  *
  *          Implements docs/PAYLOAD.md v1:
  *            - FPort 1  : UPLINK   — fixed 30-byte sensor frame, fmt = 0x01,
  *                                    little-endian scaled integers (no floats
  *                                    on-air). See the offset table below.
  *            - FPort 10 : DOWNLINK — config/command frames; byte 0 = command
  *                                    id, followed by that command's args.
  *
  *          Uplink frame layout (all multi-byte fields little-endian):
  *            Off  Field        Type  Encoding   Governing OK-bit
  *              0  fmt          u8    0x01       (always)
  *              1  status       u8    bitfield   (always)
  *              2  batt_mV      u16   raw mV     (always — no OK-bit)
  *              4  air1_temp    i16   x100 C     SENS_OK_AIR1
  *              6  air1_rh      u8    x2 %RH     SENS_OK_AIR1
  *              7  air1_press   u16   x10 hPa    SENS_OK_AIR1
  *              9  air2_temp    i16   x100 C     SENS_OK_AIR2
  *             11  air2_rh      u8    x2 %RH     SENS_OK_AIR2
  *             12  air2_press   u16   x10 hPa    SENS_OK_AIR2
  *             14  soil_moist   u16   raw / ‰    SENS_OK_SOIL
  *             16  leaf_wet     u16   raw / ‰    SENS_OK_LEAF
  *             18  soil_temp    i16   x100 C     SENS_OK_PT1000
  *             20  wind_speed   u16   x100 m/s   SENS_OK_WIND
  *             22  wind_dir     u16   x10 deg    SENS_OK_WIND
  *             24  wind_gust    u16   x100 m/s   SENS_OK_WIND
  *             26  rain_tips    u16   raw        SENS_OK_RAIN
  *             28  rain_mm      u16   x100 mm    SENS_OK_RAIN
  *             30  batt_i       i16   mA, discharge positive — always sent
  *                                    (sentinel 0x7FFF when no INA219)
  *                                    (total = 32 bytes, fmt 0x02)
  ******************************************************************************
  * @attention
  *   Ground truth: docs/PAYLOAD.md. Do not change offsets/scalings here without
  *   updating that spec AND the TTN JavaScript decoder alongside it.
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef ENVNODE_PAYLOAD_H
#define ENVNODE_PAYLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stddef.h>
#include <stdint.h>
#include "sensors/envnode_sensors.h"   /* sensor_readings_t, env_status_t     */

/* Exported constants --------------------------------------------------------*/

#define ENVNODE_UPLINK_FPORT    (1u)     /*!< FPort for the sensor frame.      */
#define ENVNODE_DOWNLINK_FPORT  (10u)    /*!< FPort for config/commands.       */
#define ENVNODE_FMT_UPLINK      (0x02u)  /*!< Uplink frame-format id (byte 0).
                                              0x01 = 30 B, no battery current;
                                              0x02 = 32 B, i16 batt mA @30.    */
#define ENVNODE_UPLINK_LEN      (32u)    /*!< Fixed uplink frame size (bytes). */

/**
 * @name Not-OK sentinels (docs/PAYLOAD.md)
 * Substituted for a field whose governing status OK-bit is clear, so the
 * decoder renders "no data" instead of a misleading 0.
 * @{
 */
#define ENVNODE_SENTINEL_I16    ((int16_t)0x7FFF)  /*!< signed  no-data.       */
#define ENVNODE_SENTINEL_U16    ((uint16_t)0xFFFF) /*!< unsigned no-data.      */
#define ENVNODE_SENTINEL_U8     ((uint8_t)0xFF)    /*!< u8 (RH) no-data.       */
/** @} */

/**
 * @name Downlink command ids (FPort 10, byte 0) — docs/PAYLOAD.md
 * Unknown ids are ignored (and, when a diag uplink is enabled, NAK'd).
 * @{
 */
#define ENVNODE_CMD_SET_INTERVAL        (0x01u) /*!< u16 minutes: sample period*/
#define ENVNODE_CMD_UPLINK_NOW          (0x02u) /*!< sample + uplink now.      */
#define ENVNODE_CMD_RESET_RAIN          (0x03u) /*!< zero rain-tip accumulator.*/
#define ENVNODE_CMD_SET_CAL             (0x04u) /*!< u8 sensor_id, i16 off x100*/
#define ENVNODE_CMD_SET_WINDDIR_OFFSET  (0x05u) /*!< u16 deg x10 vane offset.  */
#define ENVNODE_CMD_SET_ENABLE          (0x06u) /*!< u8 SENSOR_* selection mask*/
#define ENVNODE_CMD_REBOOT              (0x07u) /*!< software reset.           */
#define ENVNODE_CMD_GET_CONFIG          (0x08u) /*!< echo config in diag uplink*/
/** @} */

/**
 * @brief  Marker byte that makes a downlink a sensor-set config string.
 *
 * Checked on ANY FPort and before the binary command table, so a node can be
 * reconfigured in plain text from any application port while the byte-efficient
 * FPort-10 table stays available. No binary command id is 0x7B, so the two
 * encodings cannot collide.
 */
#define ENVNODE_CFGSTR_MARKER   ('{')

/** Recommended size for the envnode_apply_config_string() reply buffer. */
#define ENVNODE_CFGSTR_REPLY_MAX  (64u)

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  Pack a reading struct into the fixed 30-byte FPort-1 uplink frame.
 *
 * Encodes every field little-endian with the scalings in docs/PAYLOAD.md.
 * Battery is always encoded. For every other field, if its governing status
 * OK-bit is clear the appropriate sentinel (0x7FFF / 0xFFFF / 0xFF) is written
 * instead of a scaled value. Values are rounded to nearest and saturated to
 * their field range.
 *
 * @param[in]  r       Source readings (must be non-NULL).
 * @param[out] buf     Destination buffer (must be non-NULL).
 * @param[in]  buflen  Capacity of @p buf; must be >= ENVNODE_UPLINK_LEN.
 * @return     Number of bytes written: ENVNODE_UPLINK_LEN (30) on success,
 *             or 0 on a NULL argument or a buffer that is too small.
 */
size_t envnode_payload_pack(const sensor_readings_t *r, uint8_t *buf, size_t buflen);

/**
 * @brief  Apply a sensor-set configuration string and persist the result.
 *
 * The one place where the brace grammar meets the flash config, shared by the
 * radio path and the console so both behave identically. Parsing is
 * all-or-nothing: a rejected frame changes nothing and writes no flash. An
 * accepted frame is written to flash ONLY if it actually differs from the
 * running configuration — a flash erase stalls the CM0+ core for ~20-40 ms, so
 * a repeated "{15}" must not keep paying that.
 *
 * @param[in]  text      Configuration string, starting at '{'. Never read past
 *                       @p len, so a raw LoRaWAN payload is safe to pass.
 * @param[in]  len       Readable bytes at @p text.
 * @param[out] reply     Human-readable result: the canonical rendering plus a
 *                       "saved"/"unchanged"/"NOT SAVED" note when accepted, or
 *                       the offending token when rejected. May be NULL.
 *                       Recommended size ENVNODE_CFGSTR_REPLY_MAX.
 * @param[in]  reply_sz  Capacity of @p reply.
 * @retval ENV_OK   accepted (or a "{?}" query); @p reply describes the outcome.
 * @retval ENV_ERR  rejected; @p reply holds the offending token, nothing changed.
 */
int envnode_apply_config_string(const char *text, size_t len,
                                char *reply, size_t reply_sz);

/**
 * @brief  Apply a received downlink frame.
 *
 * Two encodings share this entry point:
 *   - if byte 0 is '{' the payload is a sensor-set config string, accepted on
 *     ANY FPort (see envnode_apply_config_string()),
 *   - otherwise byte 0 is an FPort-10 command id and the args follow it per
 *     docs/PAYLOAD.md.
 * Configuration written by either path is persisted in the flash config page.
 *
 * @param[in] fport  LoRaWAN FPort the frame arrived on.
 * @param[in] buf    Frame bytes.
 * @param[in] len    Length of @p buf in bytes.
 * @retval ENV_OK       frame recognised and applied.
 * @retval ENV_ERR      wrong FPort / malformed / unknown command.
 * @retval ENV_NOTIMPL  recognised but not yet implemented.
 */
int envnode_downlink_apply(uint8_t fport, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ENVNODE_PAYLOAD_H */
