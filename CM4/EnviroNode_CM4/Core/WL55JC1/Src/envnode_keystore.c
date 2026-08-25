/**
  ******************************************************************************
  * @file    envnode_keystore.c
  * @brief   LoRaWAN identity stored in the last CM4 flash page.
  *
  *          Record layout (48 bytes = 6 double-words, the STM32WL flash write
  *          granularity), at ENV_KEYSTORE_ADDR:
  *
  *            +0   u32   magic  'ENVK'
  *            +4   u32   flags  (appkey / deveui / joineui present)
  *            +8   u8[16] app_key
  *            +24  u8[8]  dev_eui
  *            +32  u8[8]  join_eui
  *            +40  u32   checksum over bytes [0..39]
  *            +44  u32   pad
  *
  *          The page is reserved in STM32WL55JCIX_FLASH.ld (FLASH is declared as
  *          126K instead of 128K), so the linker can never place code there.
  ******************************************************************************
  */
#include <stddef.h>
#include <string.h>
#include "envnode_keystore.h"
#include "stm32wlxx_hal.h"

/* Last 2 KB page of the 128 KB CM4 region (CM0+ starts at 0x08020000). */
#define ENV_KEYSTORE_ADDR    (0x0801F800UL)
#define ENV_KEYSTORE_PAGE    (63u)                 /* 0x1F800 / 0x800          */
#define ENV_KEYSTORE_MAGIC   (0x454E564BUL)        /* 'ENVK'                   */
#define ENV_KEYSTORE_WORDS   (6u)                  /* 6 double-words = 48 B    */

/* 8-byte aligned: the record is programmed as double-words, and the loop below
   dereferences it through a uint64_t pointer. */
typedef struct __attribute__((aligned(8))) {
  uint32_t magic;
  uint32_t flags;
  uint8_t  app_key[16];
  uint8_t  dev_eui[8];
  uint8_t  join_eui[8];
  uint32_t sum;
  uint32_t pad;
} env_key_record_t;                                 /* exactly 48 bytes        */

/* Same rolling checksum style KoreroNet used for its persisted records — enough
   to reject a half-written or erased (0xFF) page. */
static uint32_t key_sum(const uint8_t *data, size_t len)
{
  uint32_t s = 0x12345678u;
  for (size_t i = 0; i < len; ++i) {
    s += data[i];
    s = (s << 5) | (s >> 27);
  }
  return s;
}

static int record_read(env_key_record_t *out)
{
  memcpy(out, (const void *)ENV_KEYSTORE_ADDR, sizeof(*out));
  if (out->magic != ENV_KEYSTORE_MAGIC) return 0;
  if (out->sum != key_sum((const uint8_t *)out, offsetof(env_key_record_t, sum))) return 0;
  if ((out->flags & ENV_KEY_FLAG_APPKEY) == 0u) return 0;
  return 1;
}

int env_keystore_valid(void)
{
  env_key_record_t rec;
  return record_read(&rec);
}

int env_keystore_load(uint8_t app_key[16], uint8_t dev_eui[8], uint8_t join_eui[8], uint32_t *flags)
{
  env_key_record_t rec;
  if (!record_read(&rec)) return 0;

  if (app_key)  memcpy(app_key, rec.app_key, 16);
  if (dev_eui)  { if (rec.flags & ENV_KEY_FLAG_DEVEUI)  memcpy(dev_eui,  rec.dev_eui,  8); else memset(dev_eui,  0, 8); }
  if (join_eui) { if (rec.flags & ENV_KEY_FLAG_JOINEUI) memcpy(join_eui, rec.join_eui, 8); else memset(join_eui, 0, 8); }
  if (flags)    *flags = rec.flags;
  return 1;
}

int env_keystore_erase(void)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t page_error = 0u;

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;
  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.Page      = ENV_KEYSTORE_PAGE;
  er.NbPages   = 1u;
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &page_error);
  (void)HAL_FLASH_Lock();
  return (st == HAL_OK) ? 1 : 0;
}

int env_keystore_save(const uint8_t app_key[16], const uint8_t dev_eui[8], const uint8_t join_eui[8])
{
  env_key_record_t rec;

  if (app_key == NULL) return 0;

  /* Refuse to store an all-zero AppKey: that is the "no key" sentinel. */
  uint8_t nz = 0u;
  for (int i = 0; i < 16; ++i) nz |= app_key[i];
  if (!nz) return 0;

  memset(&rec, 0, sizeof(rec));
  rec.magic = ENV_KEYSTORE_MAGIC;
  rec.flags = ENV_KEY_FLAG_APPKEY;
  memcpy(rec.app_key, app_key, 16);

  if (dev_eui) {
    uint8_t d = 0u; for (int i = 0; i < 8; ++i) d |= dev_eui[i];
    if (d) { memcpy(rec.dev_eui, dev_eui, 8); rec.flags |= ENV_KEY_FLAG_DEVEUI; }
  }
  if (join_eui) {
    uint8_t j = 0u; for (int i = 0; i < 8; ++i) j |= join_eui[i];
    if (j) { memcpy(rec.join_eui, join_eui, 8); rec.flags |= ENV_KEY_FLAG_JOINEUI; }
  }
  rec.sum = key_sum((const uint8_t *)&rec, offsetof(env_key_record_t, sum));

  if (!env_keystore_erase()) return 0;

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;

  const uint64_t *src = (const uint64_t *)(const void *)&rec;
  int ok = 1;
  for (uint32_t i = 0; i < ENV_KEYSTORE_WORDS; ++i) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          ENV_KEYSTORE_ADDR + (i * 8u), src[i]) != HAL_OK) {
      ok = 0;
      break;
    }
  }
  (void)HAL_FLASH_Lock();

  /* Verify by reading the record back through the normal path. */
  return ok ? env_keystore_valid() : 0;
}
