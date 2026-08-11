/**
  ******************************************************************************
  * @file    envnode_log.c
  * @brief   Flash ring for timestamped sensor records.
  *
  *          Record layout (40 bytes = 5 double-words, the flash write width):
  *            +0   u32    epoch2000   0xFFFFFFFF = empty slot
  *            +4   u8[30] frame       the FPort-1 bytes, verbatim
  *            +34  u16    sum         checksum over epoch+frame
  *            +36  u32    pad         stays 0xFFFFFFFF
  *
  *          The frame is stored exactly as transmitted so the log and the radio
  *          can never disagree — decoding happens at dump time, in one place.
  *
  *          Ring discipline: append sequentially; before writing the first slot
  *          of a page, erase that page. On boot the head is found by locating
  *          the record with the newest epoch and pointing past it. A page erase
  *          therefore reclaims the oldest 51 records exactly when space runs
  *          out, and a torn write (power lost mid-append) fails its checksum
  *          and is skipped.
  ******************************************************************************
  */
#include <string.h>
#include "envnode_log.h"
#include "stm32wlxx_hal.h"

#define LOG_ADDR        (0x0801B800UL)          /* page 55                     */
#define LOG_FIRST_PAGE  (55u)
#define LOG_PAGES       (7u)
#define PAGE_SIZE       (2048u)
#define REC_SIZE        (40u)
#define RECS_PER_PAGE   (PAGE_SIZE / REC_SIZE)  /* 51 (8 B/page wasted)        */
#define REC_TOTAL       (LOG_PAGES * RECS_PER_PAGE)   /* 357                   */

typedef struct __attribute__((aligned(8))) {
  uint32_t epoch;
  uint8_t  frame[ENVLOG_FRAME_LEN];
  uint16_t sum;
  uint32_t pad;
} log_rec_t;                                    /* 40 bytes                    */

static uint32_t s_head;    /*!< next slot to write, 0..REC_TOTAL-1             */
static uint32_t s_count;   /*!< valid records                                  */

/* Same rolling-checksum family the config/key records use, truncated to 16 bit
   — plenty to reject a torn write or an erased slot. */
static uint16_t rec_sum(uint32_t epoch, const uint8_t *frame)
{
  uint32_t s = 0x12345678u;
  for (int i = 0; i < 4; ++i)  { s += (uint8_t)(epoch >> (8 * i)); s = (s << 5) | (s >> 27); }
  for (int i = 0; i < (int)ENVLOG_FRAME_LEN; ++i) { s += frame[i]; s = (s << 5) | (s >> 27); }
  return (uint16_t)(s ^ (s >> 16));
}

static const log_rec_t *slot(uint32_t idx)
{
  return (const log_rec_t *)(LOG_ADDR + ((idx % REC_TOTAL) * REC_SIZE));
}

static int slot_valid(uint32_t idx)
{
  const log_rec_t *r = slot(idx);
  if (r->epoch == 0xFFFFFFFFu) return 0;
  return (r->sum == rec_sum(r->epoch, r->frame)) ? 1 : 0;
}

static int erase_page_of(uint32_t idx)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t page_error = 0u;

  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.Page      = LOG_FIRST_PAGE + ((idx % REC_TOTAL) / RECS_PER_PAGE);
  er.NbPages   = 1u;

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &page_error);
  (void)HAL_FLASH_Lock();
  return (st == HAL_OK) ? 1 : 0;
}

uint32_t envnode_log_init(void)
{
  uint32_t newest_idx = 0u;
  uint32_t newest_epoch = 0u;
  int any = 0;

  s_count = 0u;
  for (uint32_t i = 0; i < REC_TOTAL; ++i) {
    if (slot_valid(i)) {
      s_count++;
      if (slot(i)->epoch >= newest_epoch) { newest_epoch = slot(i)->epoch; newest_idx = i; }
      any = 1;
    }
  }
  s_head = any ? ((newest_idx + 1u) % REC_TOTAL) : 0u;
  return s_count;
}

int envnode_log_append(uint32_t epoch2000, const uint8_t frame[ENVLOG_FRAME_LEN])
{
  if (frame == NULL) return 0;

  /* Entering a fresh page: reclaim it (erases the oldest 51 records once the
     ring has wrapped; on a blank ring it erases already-blank flash, harmless). */
  if ((s_head % RECS_PER_PAGE) == 0u) {
    uint32_t reclaimed = 0u;
    for (uint32_t i = 0; i < RECS_PER_PAGE; ++i) {
      if (slot_valid(s_head + i)) reclaimed++;
    }
    if (!erase_page_of(s_head)) return 0;
    s_count -= reclaimed;
  }

  log_rec_t rec;
  memset(&rec, 0xFF, sizeof(rec));
  rec.epoch = epoch2000;
  memcpy(rec.frame, frame, ENVLOG_FRAME_LEN);
  rec.sum = rec_sum(rec.epoch, rec.frame);

  const uint32_t dst = LOG_ADDR + (s_head * REC_SIZE);
  const uint64_t *src = (const uint64_t *)(const void *)&rec;

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;
  int ok = 1;
  for (uint32_t i = 0; i < (REC_SIZE / 8u); ++i) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, dst + (i * 8u), src[i]) != HAL_OK) {
      ok = 0;
      break;
    }
  }
  (void)HAL_FLASH_Lock();

  if (ok) {
    s_head = (s_head + 1u) % REC_TOTAL;
    s_count++;
  }
  return ok;
}

uint32_t envnode_log_count(void)
{
  return s_count;
}

int envnode_log_get(uint32_t back, uint32_t *epoch2000, uint8_t frame[ENVLOG_FRAME_LEN])
{
  if (back >= s_count || epoch2000 == NULL || frame == NULL) return 0;

  /* Walk backwards from the head, skipping invalid slots (torn writes). */
  uint32_t idx = s_head;
  uint32_t seen = 0u;
  for (uint32_t hops = 0; hops < REC_TOTAL; ++hops) {
    idx = (idx + REC_TOTAL - 1u) % REC_TOTAL;
    if (!slot_valid(idx)) continue;
    if (seen == back) {
      const log_rec_t *r = slot(idx);
      *epoch2000 = r->epoch;
      memcpy(frame, r->frame, ENVLOG_FRAME_LEN);
      return 1;
    }
    seen++;
  }
  return 0;
}

int envnode_log_erase_all(void)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t page_error = 0u;

  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.Page      = LOG_FIRST_PAGE;
  er.NbPages   = LOG_PAGES;

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&er, &page_error);
  (void)HAL_FLASH_Lock();

  if (st == HAL_OK) { s_head = 0u; s_count = 0u; return 1; }
  return 0;
}
