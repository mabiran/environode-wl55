/**
  ******************************************************************************
  * @file    sd_spi.h
  * @brief   SD card in SPI mode on SPI1 — low-level driver.
  *
  *          **Status: programmed, not in service.** The driver is compiled and
  *          `nucleo sd` will probe a card the day a breakout is wired, but
  *          nothing logs to SD yet — the FAT filesystem layer is future work
  *          (docs/LOGBOOK.md "SD-card mass logging"). Until then the flash ring
  *          (envnode_log.c) is the offline store.
  *
  *          Bus sharing: SPI1 also carries the MAX31865 (CS = PA4). The card
  *          gets its own CS on **PB12 (Arduino D2)**; both CS lines idle high,
  *          and the single-threaded main loop guarantees the two never overlap.
  *          Init temporarily drops SPI1 to ~250 kHz (the SD spec caps the init
  *          phase at 400 kHz) and always restores the 2 MHz data rate — the
  *          MAX31865 sees the bus exactly as before.
  *
  *          Wiring (docs/LOGBOOK.md): SCK D13, MISO D12, MOSI D11 (shared),
  *          CS D2, VCC 3V3 direct (NOT VSENS), 10 k pull-up on CS, 100 nF+10 µF
  *          at the breakout. 3.3 V-native breakout only — the HW-125's 5 V
  *          regulator browns out at 3.3 V.
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
int sd_spi_read_block(uint32_t lba, uint8_t buf[SD_BLOCK_SIZE]);
int sd_spi_write_block(uint32_t lba, const uint8_t buf[SD_BLOCK_SIZE]);

#ifdef __cplusplus
}
#endif
#endif /* SD_SPI_H */
