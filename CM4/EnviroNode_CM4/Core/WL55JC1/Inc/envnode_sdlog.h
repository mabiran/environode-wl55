/**
  ******************************************************************************
  * @file    envnode_sdlog.h
  * @brief   SD-card CSV logging + CONFIG.INI credential provisioning.
  *
  *          If a FAT card is present at boot:
  *            - every measurement appends one CSV row to `YYYYMMDD.CSV`
  *              (daily rotation, header written once per file, f_sync per row
  *              so a power cut costs at most one row);
  *            - `CONFIG.INI` in the root, if present, supplies LoRaWAN
  *              credentials that OUTRANK every stored identity — inserting a
  *              prepared card is the zero-console field-provisioning path.
  *
  *          CONFIG.INI format (case-insensitive keys, `;`/`#` comments):
  *              appkey  = 00112233445566778899AABBCCDDEEFF   ; 32 hex, required
  *              deveui  = 0080E115061BF803                   ; 16 hex, optional
  *              joineui = 0000000000000000                   ; 16 hex, optional
  *                                                            (appeui accepted)
  *          ⚠️ The AppKey sits in PLAINTEXT on removable media. Acceptable for
  *          this fleet; remove the file after provisioning if that ever stops
  *          being true.
  *
  *          No card / no reader: everything degrades to the flash ring
  *          (envnode_log.c) exactly as before — init just reports "no card".
  ******************************************************************************
  */
#ifndef ENVNODE_SDLOG_H
#define ENVNODE_SDLOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint8_t app_key[16];
  uint8_t dev_eui[8];
  uint8_t join_eui[8];
  uint8_t has_appkey, has_deveui, has_joineui;
} sdlog_creds_t;

/**
 * @brief  Probe the card, mount the filesystem, read CONFIG.INI.
 * @param  creds  [out] parsed credentials (has_* flags say which were present).
 * @retval 1 if a filesystem mounted (logging active), 0 otherwise.
 */
int envnode_sdlog_init(sdlog_creds_t *creds);

/**
 * @brief  ERASE the card and lay down a fresh FAT32 (`nucleo sd format`).
 *         Blocks for up to a few minutes on big cards (IWDG fed in diskio);
 *         also the on-node fix for factory-exFAT ≥64 GB cards.
 * @retval 1 on success (call envnode_sdlog_init() after to remount);
 *         negative -FRESULT on failure.
 */
int envnode_sdlog_format(void);

/** @brief 1 while the card is mounted and writable. */
int envnode_sdlog_active(void);

/**
 * @brief  Append one CSV row to today's file. Opens/rotates by @p epoch2000's
 *         date; writes @p header first on a fresh file; f_syncs before return.
 * @retval 1 written, 0 failed (logging disables itself until re-init).
 */
int envnode_sdlog_append(uint32_t epoch2000, const char *header, const char *row);

/** @brief One status line for the console (`nucleo sd`). */
void envnode_sdlog_status(char *buf, size_t len);

/**
 * @brief  Pure CONFIG.INI parser — separated so the self-test can exercise it
 *         from a RAM string with no card present.
 * @retval number of recognised keys.
 */
int envnode_ini_parse(const char *text, size_t len, sdlog_creds_t *creds);

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_SDLOG_H */
