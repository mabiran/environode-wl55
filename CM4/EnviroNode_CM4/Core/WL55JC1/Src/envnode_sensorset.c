/**
  ******************************************************************************
  * @file    envnode_sensorset.c
  * @brief   Sensor-set configuration string parser / formatter.
  *
  *          Pure text logic — no HAL, no malloc, no printf, no locale. The
  *          tokeniser copies each token into a small fixed buffer, so nothing
  *          here can be made to read past the caller's length, which matters
  *          because one of the two callers hands us a raw radio payload.
  *
  *          Parsing is two-phase on purpose: phase 1 accumulates the effect of
  *          every token WITHOUT touching the outputs, phase 2 commits. That is
  *          what makes rejection all-or-nothing — a remote node that half-applies
  *          a corrupt downlink can lose the sensor that mattered.
  ******************************************************************************
  */

#include "envnode_sensorset.h"

/* Longest legal token is "NONE" (4). Anything longer is illegal anyway, so a
   small buffer is enough; over-long tokens are truncated for the error text. */
#define TOK_MAX   (12u)

/* Canonical order — this table drives both parsing and rendering, so the two
   can never disagree about which keys exist. */
static const envset_key_t s_keys[] = {
  { "LW", SENSOR_LW, "leaf wetness"        },
  { "T1", SENSOR_T1, "air T/RH/P #1"       },
  { "T2", SENSOR_T2, "air T/RH/P #2"       },
  { "SM", SENSOR_SM, "soil moisture"       },
  { "ST", SENSOR_ST, "soil temperature"    },
  { "WS", SENSOR_WS, "wind speed"          },
  { "WD", SENSOR_WD, "wind direction"      },
  { "R",  SENSOR_R,  "rainfall"            },
};
#define KEY_COUNT  (sizeof(s_keys) / sizeof(s_keys[0]))

const envset_key_t *envnode_sensorset_keys(size_t *count)
{
  if (count != NULL) *count = KEY_COUNT;
  return s_keys;
}

/* --- tiny ctype/string helpers (freestanding: don't drag in <ctype.h>) ----- */

static int is_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n') ||
         (c == '\v') || (c == '\f');
}

static char to_upper(char c)
{
  return ((c >= 'a') && (c <= 'z')) ? (char)(c - 'a' + 'A') : c;
}

static int str_eq(const char *a, const char *b)
{
  while ((*a != '\0') && (*a == *b)) { ++a; ++b; }
  return (*a == '\0') && (*b == '\0');
}

/** Copy the offending token out for the caller's error report. */
static void err_set(char *err, size_t err_sz, const char *tok)
{
  if ((err == NULL) || (err_sz == 0u)) return;
  size_t i = 0u;
  while ((tok[i] != '\0') && ((i + 1u) < err_sz)) { err[i] = tok[i]; ++i; }
  err[i] = '\0';
}

static uint16_t clamp_interval(uint32_t minutes)
{
  if (minutes < ENVSET_INTERVAL_MIN) return (uint16_t)ENVSET_INTERVAL_MIN;
  if (minutes > ENVSET_INTERVAL_MAX) return (uint16_t)ENVSET_INTERVAL_MAX;
  return (uint16_t)minutes;
}

/* --- parser --------------------------------------------------------------- */

/** Everything one frame asks for, accumulated before anything is committed. */
typedef struct {
  uint8_t  plain;        /* union of the plain sensor keys seen               */
  uint8_t  have_plain;   /* a plain key / ALL / NONE was seen -> REPLACE mode  */
  uint8_t  add;          /* union of +KEY                                     */
  uint8_t  del;          /* union of -KEY                                     */
  uint8_t  have_int;     /* an interval token was seen                        */
  uint8_t  query;        /* the '?' token was seen                            */
  uint16_t interval;     /* value of that interval token                      */
} parse_acc_t;

/** Look a key up in the canonical table. Returns 0 when unknown. */
static uint8_t key_bit(const char *tok)
{
  for (size_t i = 0u; i < KEY_COUNT; ++i) {
    if (str_eq(tok, s_keys[i].key)) return s_keys[i].bit;
  }
  return 0u;
}

/**
 * @brief  Fold one complete token into the accumulator.
 * @return 1 on success, 0 if the token is illegal (caller rejects the frame).
 */
static int tok_apply(parse_acc_t *acc, const char *tok)
{
  /* Query: report-only. Kept exclusive so "{?,R}" cannot mean two things. */
  if (str_eq(tok, "?")) { acc->query = 1u; return 1; }

  /* Incremental edit: +KEY / -KEY. Only real sensor keys may carry a sign —
     "+ALL" / "-NONE" would be ambiguous, so they are rejected outright. */
  if ((tok[0] == '+') || (tok[0] == '-')) {
    uint8_t bit = key_bit(&tok[1]);
    if (bit == 0u) return 0;
    if (tok[0] == '+') acc->add |= bit;
    else               acc->del |= bit;
    return 1;
  }

  /* Bare integer = interval in minutes. */
  if ((tok[0] >= '0') && (tok[0] <= '9')) {
    uint32_t v = 0u;
    for (size_t i = 0u; tok[i] != '\0'; ++i) {
      if ((tok[i] < '0') || (tok[i] > '9')) return 0;   /* e.g. "12X"         */
      v = (v * 10u) + (uint32_t)(tok[i] - '0');
      if (v > 100000u) return 0;                        /* absurd: bail early */
    }
    if ((v < ENVSET_INTERVAL_MIN) || (v > ENVSET_INTERVAL_MAX)) return 0;
    /* A repeated identical interval is harmless; two different ones are a
       contradiction the node must not silently resolve. */
    if (acc->have_int && (acc->interval != (uint16_t)v)) return 0;
    acc->interval = (uint16_t)v;
    acc->have_int = 1u;
    return 1;
  }

  /* Set aliases. NONE contributes no bits but still forces REPLACE mode, which
     is exactly what makes "{NONE}" clear the selection. */
  if (str_eq(tok, "ALL"))  { acc->plain |= SENSOR_ALL; acc->have_plain = 1u; return 1; }
  if (str_eq(tok, "NONE")) {                           acc->have_plain = 1u; return 1; }

  /* Plain sensor key. */
  {
    uint8_t bit = key_bit(tok);
    if (bit == 0u) return 0;
    acc->plain |= bit;
    acc->have_plain = 1u;
  }
  return 1;
}

envset_result_t envnode_sensorset_parse_n(const char *text, size_t len,
                                          uint8_t cur_mask, uint16_t cur_interval,
                                          uint8_t *out_mask, uint16_t *out_interval,
                                          char *err, size_t err_sz)
{
  parse_acc_t acc = {0u, 0u, 0u, 0u, 0u, 0u, 0u};
  char   tok[TOK_MAX];
  size_t tok_len = 0u;
  size_t i       = 0u;
  int    closed  = 0;
  int    any_tok = 0;      /* at least one token was completed                */

  if ((text == NULL) || (out_mask == NULL) || (out_interval == NULL)) {
    err_set(err, err_sz, "(null)");
    return ENVSET_REJECTED;
  }

  while ((i < len) && is_ws(text[i])) ++i;
  if ((i >= len) || (text[i] != '{')) {
    err_set(err, err_sz, "missing '{'");
    return ENVSET_REJECTED;
  }
  ++i;

  /* Scan tokens up to the closing brace. Whitespace is dropped everywhere, so
     "{ LW , T1 , 15 }" and "{LW,T1,15}" are the same frame. */
  for (; i < len; ++i) {
    char c = text[i];

    if (is_ws(c)) continue;

    if ((c == ',') || (c == '}')) {
      tok[tok_len] = '\0';
      if (tok_len == 0u) {                 /* "{}", "{,}", "{LW,,R}"          */
        err_set(err, err_sz, "(empty token)");
        return ENVSET_REJECTED;
      }
      if (!tok_apply(&acc, tok)) {
        err_set(err, err_sz, tok);
        return ENVSET_REJECTED;
      }
      any_tok = 1;
      tok_len = 0u;
      if (c == '}') { closed = 1; ++i; break; }
      continue;
    }

    if (tok_len < (TOK_MAX - 1u)) {
      tok[tok_len++] = to_upper(c);
    } else {
      /* Over-long token: it cannot be legal. Truncate for the error text. */
      tok[TOK_MAX - 1u] = '\0';
      err_set(err, err_sz, tok);
      return ENVSET_REJECTED;
    }
  }

  if (!closed) {
    err_set(err, err_sz, "missing '}'");
    return ENVSET_REJECTED;
  }
  if (!any_tok) {                          /* cannot happen, but be explicit  */
    err_set(err, err_sz, "(empty token)");
    return ENVSET_REJECTED;
  }

  /* Only whitespace or NUL padding may follow the closing brace — a gateway may
     pad a downlink, but real trailing text means the frame is not what we think
     it is, and guessing is how half-configured nodes happen. */
  for (; i < len; ++i) {
    if (!is_ws(text[i]) && (text[i] != '\0')) {
      char bad[2];
      bad[0] = text[i];
      bad[1] = '\0';
      err_set(err, err_sz, bad);
      return ENVSET_REJECTED;
    }
  }

  /* '?' is report-only and therefore exclusive: mixing it with edits would ask
     the node to both change and not change. */
  if (acc.query) {
    if (acc.have_plain || acc.add || acc.del || acc.have_int) {
      err_set(err, err_sz, "?");
      return ENVSET_REJECTED;
    }
    *out_mask     = cur_mask;
    *out_interval = clamp_interval(cur_interval);
    return ENVSET_QUERY;
  }

  /* Commit. A frame with any plain key REPLACES the set; +/- are then layered
     on top, so "{ALL,-R}" means "everything except rain". If a key appears with
     both signs, removal wins (it is applied last) — the safe reading of an
     ambiguous request. */
  {
    uint8_t base = acc.have_plain ? acc.plain : cur_mask;
    *out_mask     = (uint8_t)((base | acc.add) & (uint8_t)~acc.del);
    *out_interval = acc.have_int ? acc.interval : clamp_interval(cur_interval);
  }
  return ENVSET_ACCEPTED;
}

envset_result_t envnode_sensorset_parse(const char *text,
                                        uint8_t cur_mask, uint16_t cur_interval,
                                        uint8_t *out_mask, uint16_t *out_interval,
                                        char *err, size_t err_sz)
{
  size_t len = 0u;
  if (text == NULL) {
    err_set(err, err_sz, "(null)");
    return ENVSET_REJECTED;
  }
  while (text[len] != '\0') ++len;
  return envnode_sensorset_parse_n(text, len, cur_mask, cur_interval,
                                   out_mask, out_interval, err, err_sz);
}

/* --- formatter ------------------------------------------------------------ */

/** Append one NUL-terminated chunk; returns 0 if it would not fit. */
static int put_str(char *buf, size_t n, size_t *pos, const char *s)
{
  for (size_t i = 0u; s[i] != '\0'; ++i) {
    if ((*pos + 1u) >= n) return 0;        /* keep room for the NUL           */
    buf[(*pos)++] = s[i];
  }
  return 1;
}

/** Append 1..999 as decimal without pulling in printf. */
static int put_u16(char *buf, size_t n, size_t *pos, uint16_t v)
{
  char d[6];
  size_t k = 0u;
  do { d[k++] = (char)('0' + (v % 10u)); v = (uint16_t)(v / 10u); } while (v != 0u);
  while (k > 0u) {
    if ((*pos + 1u) >= n) return 0;
    buf[(*pos)++] = d[--k];
  }
  return 1;
}

size_t envnode_sensorset_format(uint8_t mask, uint16_t interval, char *buf, size_t n)
{
  size_t pos = 0u;
  int    ok  = 1;

  if ((buf == NULL) || (n == 0u)) return 0u;
  buf[0] = '\0';

  ok = ok && put_str(buf, n, &pos, "{");

  if (mask == SENSOR_NONE) {
    ok = ok && put_str(buf, n, &pos, "NONE");
  } else {
    int first = 1;
    for (size_t i = 0u; ok && (i < KEY_COUNT); ++i) {
      if ((mask & s_keys[i].bit) == 0u) continue;
      if (!first) ok = ok && put_str(buf, n, &pos, ",");
      ok = ok && put_str(buf, n, &pos, s_keys[i].key);
      first = 0;
    }
  }

  ok = ok && put_str(buf, n, &pos, ",");
  ok = ok && put_u16(buf, n, &pos, clamp_interval(interval));
  ok = ok && put_str(buf, n, &pos, "}");

  if (!ok) { buf[0] = '\0'; return 0u; }
  buf[pos] = '\0';
  return pos;
}

/* --- power / sleep predicate ---------------------------------------------- */

int envnode_sensorset_requires_awake(uint8_t mask)
{
  return ((mask & SENSOR_EXTI_COUNTED) != 0u) ? 1 : 0;
}

const char *envnode_sensorset_awake_reason(uint8_t mask)
{
  const uint8_t e = (uint8_t)(mask & SENSOR_EXTI_COUNTED);

  /* Wind speed is no longer here: it is sampled in a burst, not edge-counted. */
  if (e == SENSOR_R) return "rain is EXTI edge-counted: must stay awake";
  return "no edge-counted sensors selected: may sleep between cycles";
}

/* --- selection -> on-air status gate -------------------------------------- */

uint8_t envnode_sensorset_to_okmask(uint8_t mask)
{
  uint8_t ok = 0u;

  if (mask & SENSOR_T1) ok |= SENS_OK_AIR1;
  if (mask & SENSOR_T2) ok |= SENS_OK_AIR2;
  if (mask & SENSOR_SM) ok |= SENS_OK_SOIL;
  if (mask & SENSOR_LW) ok |= SENS_OK_LEAF;
  if (mask & SENSOR_ST) ok |= SENS_OK_PT1000;
  if (mask & SENSOR_R)  ok |= SENS_OK_RAIN;
  /* One status bit covers speed, direction and gust (docs/PAYLOAD.md), so
     either wind key lights it. */
  if (mask & (SENSOR_WS | SENSOR_WD)) ok |= SENS_OK_WIND;

  return ok;
}

/* --- console helper ------------------------------------------------------- */

const char *envnode_sensorset_find_brace(const char *raw)
{
  if (raw == NULL) return NULL;
  for (size_t i = 0u; raw[i] != '\0'; ++i) {
    if (raw[i] == '{') return &raw[i];
  }
  return NULL;
}
