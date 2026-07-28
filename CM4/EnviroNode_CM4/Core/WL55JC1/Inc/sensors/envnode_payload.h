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
  *                                    (total = 30 bytes)
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
#define ENVNODE_FMT_UPLINK      (0x01u)  /*!< Uplink frame-format id (byte 0). */
#define ENVNODE_UPLINK_LEN      (30u)    /*!< Fixed uplink frame size (bytes). */

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
#define ENVNODE_CMD_SET_ENABLE          (0x06u) /*!< u16 sensor enable mask.   */
#define ENVNODE_CMD_REBOOT              (0x07u) /*!< software reset.           */
#define ENVNODE_CMD_GET_CONFIG          (0x08u) /*!< echo config in diag uplink*/
/** @} */

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
 * @brief  Apply a received downlink command (FPort 10 command table).
 *
 * Parses byte 0 as a command id and applies its args per docs/PAYLOAD.md,
 * persisting anything that must survive a reset in the RTC backup registers.
 *
 * @param[in] fport  LoRaWAN FPort the frame arrived on (expected: 10).
 * @param[in] buf    Command bytes (buf[0] = command id, then args).
 * @param[in] len    Length of @p buf in bytes.
 * @retval ENV_OK       command recognised and applied.
 * @retval ENV_ERR      wrong FPort / malformed / unknown command.
 * @retval ENV_NOTIMPL  recognised but not yet implemented (skeleton).
 */
int envnode_downlink_apply(uint8_t fport, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ENVNODE_PAYLOAD_H */
