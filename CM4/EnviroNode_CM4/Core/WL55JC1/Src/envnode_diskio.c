/**
  ******************************************************************************
  * @file    envnode_diskio.c
  * @brief   FatFs disk-I/O glue — one physical drive, backed by sd_spi.c.
  *
  *          ST's ff_gen_drv multi-driver layer is deliberately skipped: this
  *          node has exactly one storage device, and the five functions FatFs
  *          actually requires map 1:1 onto the SD driver.
  ******************************************************************************
  */
#include "diskio.h"
#include "sd_spi.h"
#include "stm32wlxx.h"   /* IWDG feed during long transfers (mkfs) */

static DSTATUS s_stat = STA_NOINIT;
static DWORD   s_sectors;        /* from the CSD at probe — for f_mkfs */

DSTATUS disk_initialize(BYTE pdrv)
{
  sd_info_t info;
  if (pdrv != 0) return STA_NOINIT;
  if (sd_spi_probe(&info) && info.type != SD_TYPE_NONE) {
    s_stat = 0;
    s_sectors = (DWORD)info.capacity_mb * 2048u;   /* 1 MB = 2048 x 512 B */
  } else {
    s_stat = STA_NOINIT;
    s_sectors = 0;
  }
  return s_stat;
}

DSTATUS disk_status(BYTE pdrv)
{
  return (pdrv == 0) ? s_stat : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv != 0 || s_stat) return RES_NOTRDY;
  for (UINT i = 0; i < count; ++i) {
    IWDG->KR = 0x0000AAAAu;   /* mkfs sweeps megabytes; keep the dog fed */
    if (!sd_spi_read_block(sector + i, buff + (i * SD_BLOCK_SIZE))) return RES_ERROR;
  }
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv != 0 || s_stat) return RES_NOTRDY;
  for (UINT i = 0; i < count; ++i) {
    IWDG->KR = 0x0000AAAAu;
    if (!sd_spi_write_block(sector + i, buff + (i * SD_BLOCK_SIZE))) return RES_ERROR;
  }
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  if (pdrv != 0) return RES_PARERR;
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;   /* sd_spi waits out busy per write */
    case GET_SECTOR_SIZE:  *(WORD *)buff  = SD_BLOCK_SIZE;  return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1;              return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD *)buff = s_sectors;      return RES_OK;  /* f_mkfs */
    default:               return RES_PARERR;
  }
}
