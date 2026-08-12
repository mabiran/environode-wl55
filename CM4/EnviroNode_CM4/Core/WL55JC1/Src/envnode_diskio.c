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

static DSTATUS s_stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv)
{
  sd_info_t info;
  if (pdrv != 0) return STA_NOINIT;
  s_stat = (sd_spi_probe(&info) && info.type != SD_TYPE_NONE) ? 0 : STA_NOINIT;
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
    if (!sd_spi_read_block(sector + i, buff + (i * SD_BLOCK_SIZE))) return RES_ERROR;
  }
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv != 0 || s_stat) return RES_NOTRDY;
  for (UINT i = 0; i < count; ++i) {
    if (!sd_spi_write_block(sector + i, buff + (i * SD_BLOCK_SIZE))) return RES_ERROR;
  }
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  (void)buff;
  if (pdrv != 0) return RES_PARERR;
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;   /* sd_spi waits out busy per write */
    case GET_SECTOR_SIZE:  *(WORD *)buff = SD_BLOCK_SIZE;  return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1;             return RES_OK;
    default:               return RES_PARERR;
  }
}
