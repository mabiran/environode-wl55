/**
  ******************************************************************************
  * @file    sd_spi.h
  * @brief   SD card in SPI mode on SPI1 — low-level driver.
  *
  *          **Status: in service** — envnode_diskio.c mounts FatFs on these
  *          block routines and envnode_sdlog.c writes the daily CSV
  *          (docs/LOGBOOK.md "SD-card mass logging").
  *
  *          Bus: the card is currently the only SPI1 device (the MAX31865 was
  *          dropped from the build). CS is **PB8 (Arduino D5)** — the pigtail's
  *          CS wire landed there (one mirror-count off from the intended D2),
  *          found electrically with `nucleo cshunt` and adopted rather than
  *          re-soldered. The SD wants SPI mode 0, so sd_bus_acquire()/release()
  *          set CPOL0/CPHA0 for the transaction and restore the bus after.
  *          Init runs at ~250 kHz (SD spec caps the init phase at 400 kHz),
  *          data at 2 MHz.
  *
  *          Wiring (docs/LOGBOOK.md): SCK D13, MISO D12, MOSI D11, CS D5,
  *          VCC 3V3 direct (NOT VSENS), ~10 k pull-up on CS, 100 nF+10 µF at
  *          the breakout. 3.3 V-native breakout only — the HW-125's 5 V
  *          regulator browns out at 3.3 V. Card: FAT32 (exFAT is compiled out,
  *          so ≤32 GB or reformat).
  ******************************************************************************
  */
#ifndef SD_SPI_H
#define SD_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
  SD_TYPE_NONE = 0,   /*!< no card answered                                    */
  SD_TYPE_V1,         /*!< SD v1.x — byte addressed                            */
  SD_TYPE_V2,         /*!< SD v2 standard capacity — byte addressed            */
  SD_TYPE_SDHC        /*!< SD v2 high capacity — block addressed               */
} sd_type_t;

typedef struct {
  sd_type_t type;
  uint32_t  capacity_mb;   /*!< from the CSD register                          */
} sd_info_t;

#define SD_BLOCK_SIZE  (512u)

/**
 * @brief  Full SPI-mode initialisation: 74+ warm-up clocks, CMD0 (idle), CMD8
 *         (voltage check / v1-v2 split), ACMD41 until ready, CMD58 (SDHC bit),
 *         CMD9 (capacity). Safe with no hardware attached — CMD0 times out and
 *         the function reports SD_TYPE_NONE in ~100 ms.
 * @param  info  [out] card type and capacity.
 * @retval 1 if a card initialised, 0 otherwise. SPI1 speed is restored either way.
 */
int sd_spi_probe(sd_info_t *info);

/**
 * @brief  Read / write one 512-byte block. Compiled and tested by the probe
 *         path; the future FatFs diskio layer calls these.
 * @param  lba  logical block address (the driver handles SDHC vs byte addressing).
 * @retval 1 on success, 0 on error/timeout.
 */
int sd_spi_diag(char *out, unsigned n);   /* raw CMD0 R1 trace for bring-up */
int sd_spi_read_block(uint32_t lba, uint8_t buf[SD_BLOCK_SIZE]);
int sd_spi_write_block(uint32_t lba, const uint8_t buf[SD_BLOCK_SIZE]);

#ifdef __cplusplus
}
#endif
#endif /* SD_SPI_H */
