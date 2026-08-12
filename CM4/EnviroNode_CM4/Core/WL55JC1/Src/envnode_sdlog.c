/**
  ******************************************************************************
  * @file    envnode_sdlog.c
  * @brief   SD CSV logging + CONFIG.INI provisioning (see header).
  ******************************************************************************
  */
#include <string.h>
#include <stdio.h>
#include "envnode_sdlog.h"
#include "ff.h"
#include "stm32wlxx_hal.h"

/* Provided by main.c — RTC seconds since 2000-01-01. */
extern uint32_t EnvNode_EpochNow(void);

static FATFS   s_fs;
static FIL     s_file;
static uint8_t s_mounted;      /* filesystem mounted                          */
static uint8_t s_file_open;    /* s_file is open on s_fname                   */
static char    s_fname[16];    /* "20260804.CSV"                              */
static uint8_t s_had_ini;      /* CONFIG.INI was found at init                */

/* FatFs timestamp hook: epoch2000 -> FAT-packed local time. */
DWORD get_fattime(void)
{
  uint32_t ep = EnvNode_EpochNow();
  uint32_t days = ep / 86400u, rem = ep % 86400u;
  uint32_t y = 2000u;
  static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  for (;;) {
    uint32_t dy = ((y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u) ? 366u : 365u;
    if (days < dy) break;
    days -= dy; y++;
  }
  uint32_t mo = 0u;
  for (; mo < 12u; ++mo) {
    uint32_t dm = dim[mo] + ((mo == 1u &&
      ((y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u)) ? 1u : 0u);
    if (days < dm) break;
    days -= dm;
  }
  if (y < 1980u) y = 1980u;                       /* FAT epoch floor          */
  return ((DWORD)(y - 1980u) << 25) | ((DWORD)(mo + 1u) << 21) | ((DWORD)(days + 1u) << 16) |
         ((rem / 3600u) << 11) | (((rem % 3600u) / 60u) << 5) | ((rem % 60u) / 2u);
}

/* --- CONFIG.INI parser (pure — self-testable) ------------------------------ */

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parse exactly n bytes of hex from s (ignoring nothing — creds are strict). */
static int parse_hex_n(const char *s, size_t avail, uint8_t *out, size_t n)
{
  if (avail < 2u * n) return 0;
  for (size_t i = 0; i < n; ++i) {
    int hi = hexval(s[2u * i]), lo = hexval(s[2u * i + 1u]);
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  /* trailing junk on the value (beyond whitespace/comment) rejects the key */
  for (size_t i = 2u * n; i < avail; ++i) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '#') break;
    return 0;
  }
  return 1;
}

int envnode_ini_parse(const char *text, size_t len, sdlog_creds_t *creds)
{
  int found = 0;
  size_t i = 0;

  if (text == NULL || creds == NULL) return 0;
  memset(creds, 0, sizeof(*creds));

  while (i < len) {
    /* find end of line */
    size_t start = i;
    while (i < len && text[i] != '\n') i++;
    size_t end = i;                                /* [start, end) = line     */
    if (i < len) i++;                              /* skip '\n'               */

    /* trim leading space; skip blank/comment lines */
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    if (start >= end || text[start] == ';' || text[start] == '#') continue;

    /* key = value */
    size_t eq = start;
    while (eq < end && text[eq] != '=') eq++;
    if (eq >= end) continue;

    /* lowercase key, stripped of spaces */
    char key[12]; size_t kn = 0;
    for (size_t k = start; k < eq && kn + 1u < sizeof(key); ++k) {
      char c = text[k];
      if (c == ' ' || c == '\t') continue;
      key[kn++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    key[kn] = '\0';

    size_t v = eq + 1u;
    while (v < end && (text[v] == ' ' || text[v] == '\t')) v++;

    if (strcmp(key, "appkey") == 0) {
      if (parse_hex_n(&text[v], end - v, creds->app_key, 16)) { creds->has_appkey = 1u; found++; }
    } else if (strcmp(key, "deveui") == 0) {
      if (parse_hex_n(&text[v], end - v, creds->dev_eui, 8)) { creds->has_deveui = 1u; found++; }
    } else if (strcmp(key, "joineui") == 0 || strcmp(key, "appeui") == 0) {
      if (parse_hex_n(&text[v], end - v, creds->join_eui, 8)) { creds->has_joineui = 1u; found++; }
    }
  }
  return found;
}

/* --- mount / config / append ---------------------------------------------- */

int envnode_sdlog_init(sdlog_creds_t *creds)
{
  s_mounted = 0u;
  s_file_open = 0u;
  s_had_ini = 0u;
  if (creds) memset(creds, 0, sizeof(*creds));

  /* Mount forces disk_initialize -> sd_spi_probe; ~100 ms and harmless when
     no card or no reader is present. */
  if (f_mount(&s_fs, "", 1) != FR_OK) return 0;
  s_mounted = 1u;

  if (creds != NULL) {
    FIL ini;
    if (f_open(&ini, "CONFIG.INI", FA_READ) == FR_OK) {
      static char buf[512];                        /* creds fit easily        */
      UINT rd = 0;
      if (f_read(&ini, buf, sizeof(buf) - 1u, &rd) == FR_OK && rd > 0u) {
        buf[rd] = '\0';
        if (envnode_ini_parse(buf, rd, creds) > 0) s_had_ini = 1u;
      }
      f_close(&ini);
    }
  }
  return 1;
}

int envnode_sdlog_active(void)
{
  return (int)s_mounted;
}

int envnode_sdlog_append(uint32_t epoch2000, const char *header, const char *row)
{
  if (!s_mounted || header == NULL || row == NULL) return 0;

  /* Daily rotation: filename from the record's own date. */
  DWORD ft = get_fattime(); (void)ft;
  uint32_t days = epoch2000 / 86400u;
  uint32_t y = 2000u;
  static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  for (;;) {
    uint32_t dy = ((y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u) ? 366u : 365u;
    if (days < dy) break;
    days -= dy; y++;
  }
  uint32_t mo = 0u;
  for (; mo < 12u; ++mo) {
    uint32_t dm = dim[mo] + ((mo == 1u &&
      ((y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u)) ? 1u : 0u);
    if (days < dm) break;
    days -= dm;
  }
  char want[16];
  snprintf(want, sizeof(want), "%04lu%02lu%02lu.CSV",
           (unsigned long)y, (unsigned long)(mo + 1u), (unsigned long)(days + 1u));

  if (s_file_open && strcmp(want, s_fname) != 0) {   /* date rolled over      */
    f_close(&s_file);
    s_file_open = 0u;
  }
  if (!s_file_open) {
    if (f_open(&s_file, want, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
      s_mounted = 0u;                                /* card gone — disable   */
      return 0;
    }
    strcpy(s_fname, want);
    s_file_open = 1u;
    if (f_size(&s_file) == 0u) {                     /* fresh file: header    */
      UINT bw;
      (void)f_write(&s_file, header, (UINT)strlen(header), &bw);
    }
  }

  UINT bw = 0;
  FRESULT fr = f_write(&s_file, row, (UINT)strlen(row), &bw);
  if (fr != FR_OK || bw != strlen(row) || f_sync(&s_file) != FR_OK) {
    f_close(&s_file);
    s_file_open = 0u;
    s_mounted = 0u;                                  /* fail loud, once       */
    return 0;
  }
  return 1;
}

void envnode_sdlog_status(char *buf, size_t len)
{
  if (!s_mounted) {
    snprintf(buf, len, "SDLOG: inactive (no card at boot, or a write failed)");
  } else {
    snprintf(buf, len, "SDLOG: active, file %s%s%s",
             s_file_open ? s_fname : "(none yet)",
             s_had_ini ? ", CONFIG.INI applied" : "",
             s_file_open ? "" : " - first row creates today's file");
  }
}
