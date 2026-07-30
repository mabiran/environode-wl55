/**
  ******************************************************************************
  * @file    envnode_keystore.h
  * @brief   Non-volatile LoRaWAN identity store (internal flash).
  *
  *          The inherited KoreroNet code keeps the OTAA identity in RTC backup
  *          registers. Those survive a reset, and a power cut only while VBAT is
  *          held up — on this board VBAT rides VDD, so a flat battery or an
  *          unplugged node loses the AppKey and the node stops re-joining until
  *          someone re-provisions it over the console.
  *
  *          This module mirrors the identity into the LAST 2 KB page of the CM4
  *          flash region (0x0801F800), which the linker script excludes from the
  *          image, so it survives any power loss and a re-flash of the
  *          application only if that page is not mass-erased.
  *
  *          Cost note: erasing/programming flash stalls instruction fetches for
  *          the OTHER core too, so only write when the user provisions keys —
  *          never on a timer, and never in the middle of a LoRaWAN RX window.
  ******************************************************************************
  */
#ifndef ENVNODE_KEYSTORE_H
#define ENVNODE_KEYSTORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Flags describing which fields of the record are meaningful. */
#define ENV_KEY_FLAG_APPKEY   (1u << 0)
#define ENV_KEY_FLAG_DEVEUI   (1u << 1)
#define ENV_KEY_FLAG_JOINEUI  (1u << 2)

/**
 * @brief  Persist the OTAA identity to flash (erase + rewrite the key page).
 * @param  app_key   16 bytes, big-endian as entered (required).
 * @param  dev_eui   8 bytes, or NULL / all-zero to keep the chip DevEUI.
 * @param  join_eui  8 bytes, or NULL / all-zero for the TTN default.
 * @retval 1 on success, 0 if the flash operation failed.
 */
int env_keystore_save(const uint8_t app_key[16], const uint8_t dev_eui[8], const uint8_t join_eui[8]);

/**
 * @brief  Read the stored identity back.
 * @param  app_key   [out] 16 bytes (written only when the record is valid).
 * @param  dev_eui   [out] 8 bytes (zeroed when the record has no DevEUI).
 * @param  join_eui  [out] 8 bytes (zeroed when the record has no JoinEUI).
 * @param  flags     [out] ENV_KEY_FLAG_* bitmask; may be NULL.
 * @retval 1 if a valid record was found, 0 otherwise.
 */
int env_keystore_load(uint8_t app_key[16], uint8_t dev_eui[8], uint8_t join_eui[8], uint32_t *flags);

/** @brief  1 if the key page currently holds a valid record. */
int env_keystore_valid(void);

/** @brief  Erase the key page (factory reset of the stored identity). */
int env_keystore_erase(void);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_KEYSTORE_H */
