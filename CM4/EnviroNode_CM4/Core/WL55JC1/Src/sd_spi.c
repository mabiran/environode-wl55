/**
  ******************************************************************************
  * @file    sd_spi.c
  * @brief   SD card SPI-mode driver (see sd_spi.h for status and wiring).
  *
  *          Implements the standard SPI-mode bring-up from the SD Physical
  *          Layer Simplified Specification: native mode is left via CMD0 with a
  *          valid CRC while CS is low, CMD8 distinguishes v2 cards (and its CRC
  *          must also be right — the two hard-coded CRCs 0x95/0x87 are the only
  *          ones SPI mode checks), ACMD41 polls the card's power-up state, and
  *          CMD58's CCS bit says whether addressing is by block or by byte.
  ******************************************************************************
  */
#include <string.h>
#include <stdio.h>
#include "sd_spi.h"
#include "spi.h"                 /* hspi1 */
#include "stm32wlxx_hal.h"

#define SD_CS_PORT        GPIOB
#define SD_CS_PIN         GPIO_PIN_8           /* Arduino D5 — the CS wire was
  soldered one mirror-count off from D2; found electrically (cshunt) and the
  firmware moved to the wire rather than the wire to the firmware. */

#define SD_INIT_PRESCALER SPI_BAUDRATEPRESCALER_64   /* 16 MHz/64 = 250 kHz    */
#define SD_DATA_PRESCALER SPI_BAUDRATEPRESCALER_8    /* the bus's normal 2 MHz */

#define SD_CMD_TIMEOUT    (10u)     /*!< response bytes to wait for R1         */
#define SD_ACMD41_MS      (1000u)   /*!< card power-up ceiling per spec        */
#define SD_TOKEN_MS       (200u)    /*!< data-token / busy wait                */

static uint8_t s_sdhc;              /*!< 1 = block addressing                  */
static uint8_t s_ready;             /*!< probe succeeded this power cycle      */

static inline void cs_low(void)  { HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET); }

/* CS is configured here, not in gpio.c: while no breakout is fitted the pin
   stays untouched (analog/reset state, no drive), so the dormant driver costs
   the free pin nothing until the first `nucleo sd`. */
static void cs_init(void)
{
  GPIO_InitTypeDef g = {0};
  cs_high();
  g.Pin = SD_CS_PIN;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SD_CS_PORT, &g);
}

/* SD cards speak SPI MODE 0 (CPOL0/CPHA0); the MAX31865 owns the bus's normal
   mode 1. Every SD entry point must switch phase in, and restore on the way
   out — running the card in mode 1 garbles CMD0 and it never answers (found on
   hardware: correct wiring, silent card). */
static void spi_cfg(uint32_t presc, uint32_t phase)
{
  hspi1.Init.BaudRatePrescaler = presc;
  hspi1.Init.CLKPhase = phase;
  (void)HAL_SPI_Init(&hspi1);
}
static void sd_bus_acquire(uint32_t presc) { spi_cfg(presc, SPI_PHASE_1EDGE); }
static void sd_bus_release(void)           { spi_cfg(SD_DATA_PRESCALER, SPI_PHASE_2EDGE); }
static void spi_set_prescaler(uint32_t presc) { sd_bus_acquire(presc); }

static uint8_t xfer(uint8_t out)
{
  uint8_t in = 0xFFu;
  (void)HAL_SPI_TransmitReceive(&hspi1, &out, &in, 1, 50);
  return in;
}

/* Send a command frame and return R1 (bit7 clear). 0xFF = no response. */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
  (void)xfer(0xFFu);                       /* one idle byte between commands   */
  (void)xfer((uint8_t)(0x40u | cmd));
  (void)xfer((uint8_t)(arg >> 24));
  (void)xfer((uint8_t)(arg >> 16));
  (void)xfer((uint8_t)(arg >> 8));
  (void)xfer((uint8_t)arg);
  (void)xfer(crc);

  for (uint32_t i = 0; i < SD_CMD_TIMEOUT; ++i) {
    uint8_t r = xfer(0xFFu);
    if ((r & 0x80u) == 0u) return r;
  }
  return 0xFFu;
}

/* Wait for a data token (0xFE) ahead of a block read. */
static int wait_token(void)
{
  uint32_t t0 = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - t0) < SD_TOKEN_MS) {
    if (xfer(0xFFu) == 0xFEu) return 1;
  }
  return 0;
}

/* Wait while the card holds MISO low (busy after a write). */
static int wait_not_busy(void)
{
  uint32_t t0 = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - t0) < SD_TOKEN_MS) {
    if (xfer(0xFFu) == 0xFFu) return 1;
  }
  return 0;
}

int sd_spi_probe(sd_info_t *info)
{
  uint8_t r;
  int ok = 0;
  sd_type_t type = SD_TYPE_NONE;
  uint32_t cap_mb = 0u;

  if (info == NULL) return 0;
  s_ready = 0u;
  s_sdhc  = 0u;

  cs_init();
  spi_set_prescaler(SD_INIT_PRESCALER);

  /* >= 74 clocks with CS and MOSI high puts the card into a known state. */
  cs_high();
  for (int i = 0; i < 10; ++i) (void)xfer(0xFFu);

  cs_low();

  /* CMD0: software reset into idle state. The only response a card can give
     here is 0x01; anything else (or silence) means no card / no breakout. */
  r = sd_cmd(0, 0u, 0x95u);
  if (r != 0x01u) goto done;

  /* CMD8: v2 cards echo the check pattern; v1 cards reject the command. */
  r = sd_cmd(8, 0x000001AAu, 0x87u);
  if (r == 0x01u) {
    uint8_t r7[4];
    for (int i = 0; i < 4; ++i) r7[i] = xfer(0xFFu);
    if (r7[2] != 0x01u || r7[3] != 0xAAu) goto done;   /* voltage mismatch     */
    type = SD_TYPE_V2;
  } else {
    type = SD_TYPE_V1;
  }

  /* ACMD41 until the card leaves idle. HCS set for v2 so an SDHC can say so. */
  {
    uint32_t t0 = HAL_GetTick();
    const uint32_t acmd_arg = (type == SD_TYPE_V2) ? 0x40000000u : 0u;
    for (;;) {
      (void)sd_cmd(55, 0u, 0x01u);
      r = sd_cmd(41, acmd_arg, 0x01u);
      if (r == 0x00u) break;
      if (r > 0x01u) goto done;                         /* illegal — not SD    */
      if ((uint32_t)(HAL_GetTick() - t0) > SD_ACMD41_MS) goto done;
    }
  }

  /* CMD58: OCR — the CCS bit separates SDHC (block) from SDSC (byte) address. */
  if (type == SD_TYPE_V2) {
    r = sd_cmd(58, 0u, 0x01u);
    if (r != 0x00u) goto done;
    uint8_t ocr[4];
    for (int i = 0; i < 4; ++i) ocr[i] = xfer(0xFFu);
    if (ocr[0] & 0x40u) { type = SD_TYPE_SDHC; s_sdhc = 1u; }
  }
  if (!s_sdhc) {
    if (sd_cmd(16, SD_BLOCK_SIZE, 0x01u) != 0x00u) goto done;  /* 512 B blocks */
  }

  /* CMD9: CSD, for the capacity figure the console reports. */
  if (sd_cmd(9, 0u, 0x01u) == 0x00u && wait_token()) {
    uint8_t csd[16];
    for (int i = 0; i < 16; ++i) csd[i] = xfer(0xFFu);
    (void)xfer(0xFFu); (void)xfer(0xFFu);              /* discard CRC          */

    if ((csd[0] >> 6) == 1u) {
      /* CSD v2 (SDHC): capacity = (C_SIZE+1) * 512 KiB */
      uint32_t c_size = (((uint32_t)csd[7] & 0x3Fu) << 16) |
                        ((uint32_t)csd[8] << 8) | csd[9];
      cap_mb = (c_size + 1u) / 2u;
    } else {
      /* CSD v1: capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN */
      uint32_t c_size = (((uint32_t)csd[6] & 0x03u) << 10) |
                        ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
      uint32_t mult   = (((uint32_t)csd[9] & 0x03u) << 1) | (csd[10] >> 7);
      uint32_t bl_len = csd[5] & 0x0Fu;
      cap_mb = ((c_size + 1u) << (mult + 2u + bl_len)) >> 20;
    }
  }

  ok = 1;
  s_ready = 1u;

done:
  cs_high();
  (void)xfer(0xFFu);                       /* release MISO                     */
  sd_bus_release();                        /* MAX31865 bus: mode 1, 2 MHz     */

  info->type = ok ? type : SD_TYPE_NONE;
  info->capacity_mb = cap_mb;
  return ok;
}

int sd_spi_diag(char *out, unsigned n)
{
  unsigned w = 0;

  /* Net-level test first: PA6 (MISO) as a plain input with the internal
     pull-up. A free/driven-high net reads 1; a net shorted to ground reads 0
     even against the pull-up — card protocol not involved at all. */
  {
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_6; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &g);
    HAL_Delay(2);
    w += snprintf(out + w, n - w, "MISO idle w/pull-up: %s | ",
                  HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) ? "HIGH (net free)"
                                                      : "LOW (NET GROUNDED!)");
    g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH; g.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &g);                     /* back to SPI duty */
  }

  cs_init();

  /* Functional CS test: hold the pin LOW for 8 s so a meter can verify the
     level AT THE CARD PAD — a beep test passes through a cold joint that this
     won't. IWDG is fed through the wait. */
  cs_low();
  for (int s = 0; s < 8; ++s) { IWDG->KR = 0x0000AAAAu; HAL_Delay(1000); }
  cs_high();

  sd_bus_acquire(SPI_BAUDRATEPRESCALER_128);     /* 125 kHz — extra margin */
  cs_high();
  for (int i = 0; i < 25; ++i) (void)xfer(0xFFu); /* 200 warm-up clocks     */
  cs_low();
  w += snprintf(out + w, n - w, "SD DIAG (125 kHz, mode 0) CMD0 R1 x8:");
  for (int a = 0; a < 8 && w + 6 < n; ++a) {
    uint8_t r = sd_cmd(0, 0u, 0x95u);
    w += snprintf(out + w, n - w, " %02X", r);
    if (r == 0x01u) break;
  }
  cs_high(); (void)xfer(0xFFu);
  sd_bus_release();
  snprintf(out + w, n - w, "\r\n  (01=card OK; FF=no answer at all; anything else=marginal wiring)");
  return 0;
}

int sd_spi_read_block(uint32_t lba, uint8_t buf[SD_BLOCK_SIZE])
{
  if (!s_ready || buf == NULL) return 0;
  sd_bus_acquire(SD_DATA_PRESCALER);
  const uint32_t addr = s_sdhc ? lba : (lba * SD_BLOCK_SIZE);
  int ok = 0;

  cs_low();
  if (sd_cmd(17, addr, 0x01u) == 0x00u && wait_token()) {
    for (uint32_t i = 0; i < SD_BLOCK_SIZE; ++i) buf[i] = xfer(0xFFu);
    (void)xfer(0xFFu); (void)xfer(0xFFu);  /* CRC, ignored in SPI mode         */
    ok = 1;
  }
  cs_high();
  (void)xfer(0xFFu);
  sd_bus_release();
  return ok;
}

int sd_spi_write_block(uint32_t lba, const uint8_t buf[SD_BLOCK_SIZE])
{
  if (!s_ready || buf == NULL) return 0;
  sd_bus_acquire(SD_DATA_PRESCALER);
  const uint32_t addr = s_sdhc ? lba : (lba * SD_BLOCK_SIZE);
  int ok = 0;

  cs_low();
  if (sd_cmd(24, addr, 0x01u) == 0x00u) {
    (void)xfer(0xFFu);
    (void)xfer(0xFEu);                     /* single-block data token          */
    for (uint32_t i = 0; i < SD_BLOCK_SIZE; ++i) (void)xfer(buf[i]);
    (void)xfer(0xFFu); (void)xfer(0xFFu);  /* dummy CRC                        */
    uint8_t resp = xfer(0xFFu);
    if ((resp & 0x1Fu) == 0x05u && wait_not_busy()) ok = 1;  /* data accepted  */
  }
  cs_high();
  (void)xfer(0xFFu);
  sd_bus_release();
  return ok;
}
