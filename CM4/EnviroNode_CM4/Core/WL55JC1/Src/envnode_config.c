/**
  ******************************************************************************
  * @file    envnode_config.c
  * @brief   Node configuration + its flash-backed persistence.
  *
  *          Record layout (56 bytes = 7 double-words) at ENVCFG_ADDR:
  *            +0   u32   magic 'ENVD'
  *            +4   u16   interval_min
  *            +6   u16   winddir_offset_deg10
  *            +8   u8    sensor_mask   (SENSOR_* bits, envnode_sensorset.h)
  *            +9   u8    pad
  *            +10  u16   pad
  *            +12  i16[9] cal offsets (x100), index = sensor_id
  *            +30  u16   pad
  *            +32  u32   checksum over [0..31]
  *            +36  u8[20] pad to a double-word multiple
  *
  *          The magic moved 'ENVC' -> 'ENVD' when the u16 enable mask (whose
  *          bits were SENS_OK_* status bits) became the u8 SENSOR_* selection
  *          mask. The two numberings differ, so a v1 record must be discarded
  *          rather than reinterpreted: reading an old 0x7F as a SENSOR_* mask
  *          would silently switch rainfall off and leaf wetness on.
  *
  *          Config lives on its own page so that saving it can never disturb the
  *          OTAA identity page — losing the AppKey to an interrupted config
  *          write would strand a deployed node.
  ******************************************************************************
  */
#include <stddef.h>
#include <string.h>
#include "envnode_config.h"
#include "stm32wlxx_hal.h"

/* Page 62 of the 128 KB CM4 region; the key store owns page 63. Both are held
   outside the image by STM32WL55JCIX_FLASH.ld. */
#define ENVCFG_ADDR     (0x0801F000UL)
#define ENVCFG_PAGE     (62u)
#define ENVCFG_MAGIC    (0x454E5644UL)      /* 'ENVD' — v2, u8 sensor mask */
#define ENVCFG_DWORDS   (7u)                /* 56 bytes */

/* 8-byte aligned: programmed as double-words through a uint64_t pointer. */
typedef struct __attribute__((aligned(8))) {
  uint32_t magic;
  uint16_t interval_min;
  uint16_t winddir_offset_deg10;
  uint8_t  sensor_mask;
  uint8_t  pad0;
  uint16_t pad1;
  int16_t  cal[ENVCFG_CAL_SLOTS];
  uint16_t pad2;
  uint32_t sum;
  uint8_t  pad3[20];
} envcfg_record_t;                          /* 56 bytes */

static envcfg_record_t s_cfg;
static volatile uint8_t s_uplink_request;

static uint32_t cfg_sum(const uint8_t *data, size_t len)
{
  uint32_t s = 0x12345678u;
  for (size_t i = 0; i < len; ++i) {
    s += data[i];
    s = (s << 5) | (s >> 27);
  }
  return s;
}

static void cfg_defaults(void)
{
  memset(&s_cfg, 0, sizeof(s_cfg));
  s_cfg.magic                = ENVCFG_MAGIC;
  s_cfg.interval_min         = ENVCFG_INTERVAL_MIN_DEFAULT;
  s_cfg.winddir_offset_deg10 = 0u;
  s_cfg.sensor_mask          = ENVCFG_SENSOR_MASK_DEFAULT;
}

void envnode_config_init(void)
{
  envcfg_record_t rec;
  memcpy(&rec, (const void *)ENVCFG_ADDR, sizeof(rec));

  if (rec.magic == ENVCFG_MAGIC &&
      rec.sum == cfg_sum((const uint8_t *)&rec, offsetof(envcfg_record_t, sum)) &&
      rec.interval_min >= ENVCFG_INTERVAL_MIN_MIN &&
      rec.interval_min <= ENVCFG_INTERVAL_MIN_MAX) {
    s_cfg = rec;
  } else {
    cfg_defaults();
  }
  s_uplink_request = 0u;
}

int envnode_config_save(void)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t page_error = 0u;

  s_cfg.magic = ENVCFG_MAGIC;
  s_cfg.sum   = cfg_sum((const uint8_t *)&s_cfg, offsetof(envcfg_record_t, sum));

  if (HAL_FLASH_Unlock() != HAL_OK) return 0;
  er.TypeErase = FLASH_TYPEERASE_PAGES;
  er.Page      = ENVCFG_PAGE;
  er.NbPages   = 1u;
  if (HAL_FLASHEx_Erase(&er, &page_error) != HAL_OK) {
    (void)HAL_FLASH_Lock();
    return 0;
  }

  const uint64_t *src = (const uint64_t *)(const void *)&s_cfg;
  int ok = 1;
  for (uint32_t i = 0; i < ENVCFG_DWORDS; ++i) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ENVCFG_ADDR + (i * 8u), src[i]) != HAL_OK) {
      ok = 0;
      break;
    }
  }
  (void)HAL_FLASH_Lock();
  return ok;
}

void envnode_config_reset(void)
{
  cfg_defaults();
  (void)envnode_config_save();
}

uint16_t envnode_config_get_interval_min(void)
{
  return s_cfg.interval_min;
}

void envnode_config_set_interval_min(uint16_t minutes)
{
  if (minutes < ENVCFG_INTERVAL_MIN_MIN) minutes = ENVCFG_INTERVAL_MIN_MIN;
  if (minutes > ENVCFG_INTERVAL_MIN_MAX) minutes = ENVCFG_INTERVAL_MIN_MAX;
  s_cfg.interval_min = minutes;
}

void envnode_config_request_uplink(void)
{
  s_uplink_request = 1u;
}

int envnode_config_take_uplink_request(void)
{
  if (!s_uplink_request) return 0;
  s_uplink_request = 0u;
  return 1;
}

int16_t envnode_config_get_cal(uint8_t sensor_id)
{
  if (sensor_id == 0u || sensor_id >= ENVCFG_CAL_SLOTS) return 0;
  return s_cfg.cal[sensor_id];
}

void envnode_config_set_cal(uint8_t sensor_id, int16_t offset_x100)
{
  if (sensor_id == 0u || sensor_id >= ENVCFG_CAL_SLOTS) return;
  s_cfg.cal[sensor_id] = offset_x100;
}

uint8_t envnode_config_get_sensor_mask(void)
{
  return s_cfg.sensor_mask;
}

void envnode_config_set_sensor_mask(uint8_t mask)
{
  /* Every one of the 8 bits is a real sensor now (SENSOR_LW..SENSOR_R), so
     there is nothing left to mask off. */
  s_cfg.sensor_mask = mask;
}

uint16_t envnode_config_get_winddir_offset_deg10(void)
{
  return s_cfg.winddir_offset_deg10;
}

void envnode_config_set_winddir_offset_deg10(uint16_t deg10)
{
  s_cfg.winddir_offset_deg10 = (uint16_t)(deg10 % 3600u);
}
