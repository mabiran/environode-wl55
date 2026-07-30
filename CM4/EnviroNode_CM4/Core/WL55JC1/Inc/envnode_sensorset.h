/**
  ******************************************************************************
  * @file    envnode_sensorset.h
  * @brief   Sensor-set configuration string — "what to measure, how often".
  *
  *          One ASCII string in braces tells the node which sensors to sample
  *          and at what cadence. It arrives either as a LoRaWAN downlink (any
  *          FPort, first payload byte '{') or as a console line. This module is
  *          the single parser both transports share, so a string that is legal
  *          over the air is legal on the wire and renders identically.
  *
  *          Grammar (case-insensitive, ALL whitespace ignored):
  *              {TOKEN[,TOKEN...]}
  *
  *          Sensor keys : LW T1 T2 SM ST WS WD R
  *          Set aliases : ALL (every sensor), NONE (no sensors)
  *          Interval    : a bare integer 1..999 = minutes between cycles
  *          Incremental : +KEY adds a sensor, -KEY removes one   e.g. {+R} {-LW}
  *          Query       : ?   report the current configuration, change nothing
  *
  *          Semantics:
  *            - a frame carrying any plain sensor key (or ALL/NONE) REPLACES the
  *              sensor set; +/- tokens in the same frame are applied on top,
  *            - a frame carrying only +/- and/or an interval EDITS the current
  *              set,
  *            - any bad token rejects the WHOLE frame and applies nothing — a
  *              half-applied remote config is worse than a rejected one.
  *
  *          Deliberately free of HAL, malloc and printf: it is pure text logic
  *          so it can be reasoned about (and unit-tested off-target) on its own.
  ******************************************************************************
  * @attention
  *   This replaces the inherited KoreroNet "timetable" entirely. The canonical
  *   rendering ({LW,T1,T2,SM,ST,WS,WD,R,15}) is what `info` prints and what an
  *   accepted frame echoes back — keep the two in step by always going through
  *   envnode_sensorset_format().
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef ENVNODE_SENSORSET_H
#define ENVNODE_SENSORSET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stddef.h>
#include <stdint.h>
#include "sensors/envnode_sensors.h"   /* SENS_OK_* — the on-air status bits   */

/* Exported constants --------------------------------------------------------*/

/**
 * @name Sensor-selection bits
 * NOTE these are NOT the SENS_OK_* status bits: the on-air status byte has one
 * shared bit for wind, while the selection distinguishes speed from direction.
 * Use envnode_sensorset_to_okmask() to cross between the two worlds — never
 * assume they are interchangeable.
 * @{
 */
#define SENSOR_LW     (1u << 0)   /*!< leaf wetness            (ADC)           */
#define SENSOR_T1     (1u << 1)   /*!< air T/RH/P #1  BME280 on I2C2 (shield)  */
#define SENSOR_T2     (1u << 2)   /*!< air T/RH/P #2  BME280 on I2C1 (board)   */
#define SENSOR_SM     (1u << 3)   /*!< soil moisture           (ADC)           */
#define SENSOR_ST     (1u << 4)   /*!< soil temperature PT1000 (SPI1)          */
#define SENSOR_WS     (1u << 5)   /*!< wind speed              (EXTI5)         */
#define SENSOR_WD     (1u << 6)   /*!< wind direction, vane    (ADC)           */
#define SENSOR_R      (1u << 7)   /*!< rainfall, tipping bucket (EXTI3)        */

#define SENSOR_ALL    (0xFFu)     /*!< the ALL alias                           */
#define SENSOR_NONE   (0x00u)     /*!< the NONE alias                          */

/** Edge-counted sensors: while either is selected the node must stay awake. */
#define SENSOR_EXTI_COUNTED  (SENSOR_WS | SENSOR_R)
/** @} */

/** @name Interval bounds (minutes) — the SPEC's 1..999 @{ */
#define ENVSET_INTERVAL_MIN   (1u)
#define ENVSET_INTERVAL_MAX   (999u)
/* Warm-test default: 1 minute. Short on purpose so a bench operator sees a
   frame land on the gateway quickly and can watch the sleep/wake cycle.
   ⚠️ Raise it before any real deployment — 1-minute uplinks exceed the TTN fair
   use policy (30 s of airtime per day). See docs/LOGBOOK.md §11. */
#define ENVSET_INTERVAL_DEF   (1u)
/** @} */

/** Buffer size that always holds the canonical rendering + NUL (28 + slack). */
#define ENVSET_STR_MAX        (32u)
/** Buffer size for the rejection reason (offending token, possibly truncated).*/
#define ENVSET_ERR_MAX        (24u)

/* Exported types ------------------------------------------------------------*/

/**
 * @brief Outcome of parsing one configuration string.
 */
typedef enum {
  ENVSET_ACCEPTED = 0,  /*!< frame is legal; *out_mask / *out_interval are the
                             configuration to adopt (may equal the current one) */
  ENVSET_QUERY    = 1,  /*!< frame was "{?}": report only, change nothing.
                             *out_* are copies of the current configuration.    */
  ENVSET_REJECTED = 2   /*!< frame is illegal; NOTHING was changed, @p err holds
                             the offending token.                               */
} envset_result_t;

/**
 * @brief One row of the canonical key table.
 */
typedef struct {
  const char *key;    /*!< two/one-letter key, upper case ("LW", "R", ...)      */
  uint8_t     bit;    /*!< the SENSOR_* bit it selects                          */
  const char *label;  /*!< human-readable name, for console listings            */
} envset_key_t;

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  The canonical key table, in canonical rendering order.
 *
 * Iterating this table is the only supported way to enumerate keys, so a new
 * sensor is added in exactly one place.
 *
 * @param[out] count  Receives the number of rows (may be NULL).
 * @return     Pointer to the (static, const) table.
 */
const envset_key_t *envnode_sensorset_keys(size_t *count);

/**
 * @brief  Parse a configuration string held in a length-delimited buffer.
 *
 * Safe on a raw LoRaWAN payload: @p text is never assumed to be NUL-terminated
 * and is never scanned past @p len. Trailing whitespace and trailing NUL padding
 * after the closing brace are tolerated (a gateway may pad); any other trailing
 * character is a rejection, as is an unknown token, an unbalanced or missing
 * brace, an empty token, empty braces, or an interval outside 1..999.
 *
 * All-or-nothing: on ENVSET_REJECTED the outputs are left untouched and the
 * caller must not write flash.
 *
 * @param[in]  text          Start of the string (should point at '{').
 * @param[in]  len           Number of readable bytes at @p text.
 * @param[in]  cur_mask      Current sensor selection (the edit baseline).
 * @param[in]  cur_interval  Current interval in minutes (the edit baseline).
 * @param[out] out_mask      Resulting selection (written unless rejected).
 * @param[out] out_interval  Resulting interval  (written unless rejected).
 * @param[out] err           Offending token on rejection (may be NULL).
 *                           Recommended size ENVSET_ERR_MAX.
 * @param[in]  err_sz        Capacity of @p err.
 * @return     ENVSET_ACCEPTED / ENVSET_QUERY / ENVSET_REJECTED.
 */
envset_result_t envnode_sensorset_parse_n(const char *text, size_t len,
                                          uint8_t cur_mask, uint16_t cur_interval,
                                          uint8_t *out_mask, uint16_t *out_interval,
                                          char *err, size_t err_sz);

/**
 * @brief  Parse a NUL-terminated configuration string (console path).
 *
 * Thin wrapper over envnode_sensorset_parse_n(). Only use it when the buffer is
 * genuinely NUL-terminated — for radio payloads call the _n form with the
 * frame length.
 */
envset_result_t envnode_sensorset_parse(const char *text,
                                        uint8_t cur_mask, uint16_t cur_interval,
                                        uint8_t *out_mask, uint16_t *out_interval,
                                        char *err, size_t err_sz);

/**
 * @brief  Render a selection + interval in canonical form.
 *
 * Fixed key order then the interval, e.g. "{LW,T1,T2,SM,ST,WS,WD,R,15}".
 * An empty selection renders as "{NONE,15}". The interval is clamped into
 * 1..999 so the output is always at most 28 characters plus NUL.
 *
 * @param[in]  mask      Selection bits (SENSOR_*).
 * @param[in]  interval  Interval in minutes.
 * @param[out] buf       Destination (must be non-NULL).
 * @param[in]  n         Capacity of @p buf; ENVSET_STR_MAX always suffices.
 * @return     Characters written excluding the NUL, or 0 if @p buf is too small
 *             (in which case @p buf is left as an empty string when n > 0).
 */
size_t envnode_sensorset_format(uint8_t mask, uint16_t interval, char *buf, size_t n);

/**
 * @brief  Would this selection prevent the node from sleeping between cycles?
 *
 * Rain and wind speed are counted from EXTI edges, which only arrive while the
 * core is running, so selecting either pins the node awake. Reported only —
 * actual STOP2 sleep is Phase 5 and is deliberately not implemented yet.
 *
 * @param[in] mask  Selection bits.
 * @retval 1  must stay awake between cycles.
 * @retval 0  may sleep between cycles.
 */
int envnode_sensorset_requires_awake(uint8_t mask);

/**
 * @brief  Human-readable justification for envnode_sensorset_requires_awake().
 * @param[in] mask  Selection bits.
 * @return    Static string, never NULL, safe to print directly.
 */
const char *envnode_sensorset_awake_reason(uint8_t mask);

/**
 * @brief  Translate a selection mask into the on-air SENS_OK_* gate mask.
 *
 * The sampler uses the result to decide which measurement blocks to run:
 *   T1->SENS_OK_AIR1  T2->SENS_OK_AIR2  SM->SENS_OK_SOIL  LW->SENS_OK_LEAF
 *   ST->SENS_OK_PT1000  (WS or WD)->SENS_OK_WIND  R->SENS_OK_RAIN
 * Wind speed and direction collapse onto one bit because the uplink status byte
 * has only one (docs/PAYLOAD.md, unchanged on air).
 *
 * @param[in] mask  Selection bits (SENSOR_*).
 * @return    Mask of SENS_OK_* bits that are permitted to be set this frame.
 */
uint8_t envnode_sensorset_to_okmask(uint8_t mask);

/**
 * @brief  Locate the configuration string inside a raw console line.
 *
 * main.c's normalize_cmd() strips commas and punctuation, so a brace string
 * MUST be parsed from the RAW line. Use this on the raw line to find the '{'
 * for both "nucleo set {...}" and a bare "{...}".
 *
 * @param[in] raw  NUL-terminated raw console line (may be NULL).
 * @return    Pointer to the first '{', or NULL if the line carries none.
 */
const char *envnode_sensorset_find_brace(const char *raw);

#ifdef __cplusplus
}
#endif

#endif /* ENVNODE_SENSORSET_H */
