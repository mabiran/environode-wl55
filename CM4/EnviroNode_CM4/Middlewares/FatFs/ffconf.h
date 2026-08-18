/**
  * ffconf.h — FatFs R0.12c configuration for EnviroNode-WL55.
  *
  * Minimal-footprint write-capable profile, chosen line by line against the
  * ~12 KB flash budget (docs/LOGBOOK.md §12A):
  *   - 8.3 filenames only (_USE_LFN 0): CONFIG.INI and YYYYMMDD.CSV fit, and
  *     LFN would drag in the Unicode tables.
  *   - _FS_TINY 1: one shared 512-byte sector buffer instead of one per file.
  *   - _FS_MINIMIZE 2: keeps f_lseek (FA_OPEN_APPEND needs it), drops dirs.
  *   - get_fattime() is real (envnode_sdlog.c, from the RTC) so CSV files carry
  *     true timestamps.
  */
#define _FFCONF 68300

#define _FS_READONLY    0
#define _FS_MINIMIZE    2
#define _USE_STRFUNC    0
#define _USE_FIND       0
#define _USE_MKFS       1   /* `nucleo sd format` — on-node FAT32 mkfs (r25) */
#define _USE_FASTSEEK   0
#define _USE_EXPAND     0
#define _USE_CHMOD      0
#define _USE_LABEL      0
#define _USE_FORWARD    0

#define _CODE_PAGE      437
#define _USE_LFN        0
#define _MAX_LFN        255
#define _LFN_UNICODE    0
#define _STRF_ENCODE    3
#define _FS_RPATH       0

#define _VOLUMES        1
#define _STR_VOLUME_ID  0
#define _MULTI_PARTITION 0
#define _MIN_SS         512
#define _MAX_SS         512
#define _USE_TRIM       0
#define _FS_NOFSINFO    0

#define _FS_TINY        1
#define _FS_EXFAT       0
#define _FS_NORTC       0
#define _NORTC_MON      1
#define _NORTC_MDAY     1
#define _NORTC_YEAR     2026
#define _FS_LOCK        0
#define _FS_REENTRANT   0
#define _FS_TIMEOUT     1000
#define _SYNC_t         HANDLE
