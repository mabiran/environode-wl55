/* USER CODE BEGIN Header */
/* =====================================================================
 *  EnviroNode-WL55  —  CM4 application core
 * ---------------------------------------------------------------------
 *  The agrometeorological node's application core. CM4 owns every
 *  application peripheral; CM0+ owns only the SubGHz radio (LoRaWAN) and
 *  is driven through the shared SRAM2 mailbox (korero_mailbox.h).
 *
 *  What lives here:
 *    - SystemClock_Config + the dual UART command server (USART1 pin
 *      header, USART2 ST-Link VCP) with the normalize_cmd parser
 *    - RTC + epoch helpers (set/read the wall clock from the console)
 *    - CM4<->CM0+ mailbox: uplink queue, downlink drain, trace forward,
 *      OTAA key provisioning, persisted in backup registers AND flash
 *    - the EnviroNode sensor path: envnode_sensors_init() at boot, then
 *      periodic sample + 30-byte FPort-1 uplink (docs/PAYLOAD.md), plus
 *      `info` / `nucleo sensors` / `nucleo uplink now` on the console
 *    - the sensor-set configuration string "{LW,T1,...,15}" (docs/CONFIG.md):
 *      one parser, two transports — a downlink on any FPort whose first
 *      byte is '{', or a console line — both echoed on the console
 *    - battery / INA219 power statistics (battery is a sensor channel)
 *    - the retained event log (`nucleo report`) and the IWDG watchdog
 *
 *  This file started life as the KoreroNet 2 acoustic-node application.
 *  Everything that served the Raspberry Pi, the recording timetable and
 *  the acoustic detection relay has been removed — this project has no
 *  Pi, no AudioMoth and no audio. PB10 (D6) and PC1 (D7) are free as a
 *  result; see ../../docs/PINOUT.md.
 *
 *  NOT implemented yet: low-power STOP2 sleep (Phase 5). The main loop
 *  still spins, which is also what the EXTI-counted rain/wind gauges
 *  require while they are enabled.
 * ===================================================================== */
/* USER CODE END Header */
/* newlib hides strcasestr() behind __GNU_VISIBLE, which is decided by the first
   libc header pulled in below. Without this the console's raw-line searches get
   an implicit `int` declaration and only work by accident (pointers are 32-bit
   here). Must stay above every #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* CubeMX peripheral headers (provide extern handles + MX_*_Init decls) */
#include "adc.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"
#if defined(HAL_RTC_MODULE_ENABLED)
#include "rtc.h"
#endif

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdlib.h>   // strtol, strtoul
#include <strings.h>  // strcasestr, strncasecmp
#include <ctype.h>
#include <stddef.h>   // size_t
#include <stdint.h>   // uint*_t
#include <stdio.h>    // snprintf, sscanf
#include "pins_config.h"
#include "battery_adc.h"
#include "battery_flow.h"
#include <stdbool.h>   // for bool, true, false
#include <math.h>      // fabsf()
#include "korero_mailbox.h"   // shared CM4<->CM0+ LoRaWAN mailbox

/* EnviroNode sensor subsystem (docs/SENSORS.md, docs/PAYLOAD.md) */
#include "envnode_keystore.h"          /* AppKey/DevEUI kept in internal flash */
#include "envnode_config.h"            /* interval / calibration / sensor mask */
#include "envnode_sensorset.h"         /* "{LW,T1,...,15}" grammar (docs/CONFIG.md) */
#include "envnode_power.h"             /* STOP2 sleep between measurement cycles */
#include "envnode_identity.h"          /* compiled-in fallback OTAA identity     */
#include "envnode_log.h"               /* offline sensor log (flash ring)        */
#include "sd_spi.h"                    /* SD low-level driver                    */
#include "envnode_sdlog.h"             /* SD CSV logging + CONFIG.INI creds      */
#include "sensors/envnode_sensors.h"
#include "sensors/envnode_payload.h"
#include "sensors/analog_sensors.h"
#include "sensors/pulse_counter.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Firmware version reported by `nucleo version` and `info`. */
#define KORERO_FW_VERSION "2.5"

/* Wait this long after boot before the first sensor uplink, so the radio core
   has a chance to complete its OTAA join first, and retry this soon when the
   radio refuses a frame (not joined / duty-cycle hold). */
#define ENVNODE_FIRST_UPLINK_DELAY_MS  (60000u)
#define ENVNODE_UPLINK_RETRY_MS        (60000u)

/* How long the node stays awake after a transmission before it may sleep.
   A Class A device can only receive in the RX1/RX2 windows that follow its own
   uplink (RX2 lands ~2 s after TX on AU915), and CM4 must then be awake to drain
   what CM0+ received. 10 s covers both with margin and gives an operator at the
   console a usable window each cycle. */
#define ENVNODE_POST_TX_AWAKE_MS       (10000u)

/* Wake this far ahead of the deadline so the sample+pack+TX path is running
   before the frame is actually due, rather than starting late every cycle. */
#define ENVNODE_SLEEP_GUARD_MS         (1500u)

#if defined(HAL_RTC_MODULE_ENABLED)
/* -------- RTC backup-register layout ---------------------------------------
   DR0 is RESERVED for the CubeMX RTC "first-boot" magic (0x32F2); we never
   touch it, so the RTC is not reset behind our back. DR1..DR8 used to hold the
   inherited recording timetable and are now unused/free. */

/* Detect availability */
#if defined(RTC_BKP_NUMBER)
  #define BKP_COUNT RTC_BKP_NUMBER
#else
  #define BKP_COUNT 0
#endif

/* ---------------- LoRaWAN key persistence ----------------------------------
   CM4 stores the OTAA identity (AppKey + optional DevEUI/JoinEUI overrides) in
   backup registers DR9..DR18 and restores it to the radio core on boot, so the
   node re-joins on its own after a reset. Backup registers survive a reset
   always, and a full power-off only when VBAT is wired (the flash key store in
   envnode_keystore.c is the fallback that survives a real power cut).
   Needs >= 19 backup registers (STM32WL55 has 20). ------------------------- */
#if BKP_COUNT >= 19
  #define LK_HAVE_PERSIST              1
  #define LK_MAGIC                     0x4B4E4C4Bu   /* 'KNLK' */
  #define LK_REG_MAGIC                 RTC_BKP_DR9
  #define LK_REG_FLAGS                 RTC_BKP_DR10  /* bit0 appkey, bit1 deveui, bit2 joineui */
  #define LK_FLAG_APPKEY               (1u << 0)
  #define LK_FLAG_DEVEUI               (1u << 1)
  #define LK_FLAG_JOINEUI              (1u << 2)
  #define LK_REG_APPKEY0               RTC_BKP_DR11  /* DR11..DR14 (16 bytes) */
  #define LK_REG_DEVEUI0               RTC_BKP_DR15  /* DR15..DR16 (8 bytes)  */
  #define LK_REG_JOINEUI0              RTC_BKP_DR17  /* DR17..DR18 (8 bytes)  */
#else
  #define LK_HAVE_PERSIST              0
#endif

#endif /* HAL_RTC_MODULE_ENABLED */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
static float soc_from_voltage(float vbat) {
  /*
   * Revised LiFePO₄ 4S voltage map.
   */
  typedef struct { float v, p; } vp_t;
  static const vp_t map[] = {
    {14.27f, 100.0f}, {14.00f, 95.0f}, {13.80f, 90.0f}, {13.60f, 75.0f},
    {13.20f, 55.0f},  {12.80f, 30.0f}, {12.40f, 10.0f}, {12.00f,  0.0f}
  };
  if (vbat >= map[0].v) return 100.0f;
  for (size_t i = 1; i < sizeof(map)/sizeof(map[0]); ++i) {
    if (vbat >= map[i].v) {
      float dv = (map[i-1].v - map[i].v);
      float t  = (dv > 0.0f) ? (vbat - map[i].v) / dv : 0.0f;
      float p  = map[i].p + t * (map[i-1].p - map[i].p);
      if (p <   0.0f) p =   0.0f;
      if (p > 100.0f) p = 100.0f;
      return p;
    }
  }
  return 0.0f;
}
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
COM_InitTypeDef BspCOMInit;
//__IO uint32_t BspButtonState = BUTTON_RELEASED;

/* NOTE: Peripheral handles are defined in adc.c/i2c.c/usart.c/rtc.c by CubeMX.
   Do NOT define them here to avoid multiple-definition link errors. */

/* USER CODE BEGIN PV */
/* UART1 line buffer (single definition) */
static uint8_t  uart1_rx_byte;
static char     uart1_line[128];
static uint32_t uart1_len = 0;

/* Second console on the ST-Link VCP (USART2). Same command interface as USART1. */
static uint8_t  uart2_rx_byte;
static char     uart2_line[128];
static uint32_t uart2_len = 0;

/* Defer command handling to main loop (ISR just sets this) */
static volatile uint8_t cmd_ready = 0;
static char             cmd_buf[128];

/* Credentials found in CONFIG.INI on the SD card at boot (all-zero if none). */
static sdlog_creds_t g_sd_creds;

/* One CSV header + row formatter, shared by the console dump and the SD file so
   the two can never disagree about the format. */
static const char ENVNODE_CSV_HEADER[] =
    "timestamp,epoch2000,status,batt_V,air1_C,air1_RH,air1_hPa,"
    "air2_C,air2_RH,air2_hPa,soil_raw,leaf_raw,soil_C,"
    "wind_ms,wind_dir,gust_ms,rain_tips,rain_mm\r\n";

/* Measurement scheduler state. File scope rather than function statics because
   the sleep tick has to see the same deadline the scheduler is working to. */
static uint32_t g_next_due_ms    = 0u;  /* when the next frame is due          */
static uint32_t g_last_tx_ms     = 0u;  /* last transmission attempt           */
static uint16_t g_armed_interval = 0u;  /* interval the deadline was built from*/
static uint8_t  g_sched_armed    = 0u;  /* boot frame has been scheduled       */
static uint8_t  g_sched_paused   = 0u;  /* {NONE} notice already printed       */

/* Battery “full” tracking state */
static uint8_t  full_marked   = 0;
static uint8_t  cond_active   = 0;
static uint32_t cond_start_ms = 0;

/* ------------------------------------------------------------------------
 * Persistent event log  (nucleo report)
 * A small ring of timestamped events: currently the cause of every (re)boot
 * (brown-out / watchdog / software / pin). It is kept in a dedicated .noinit
 * RAM region the C startup never clears, so it survives a warm reset
 * (watchdog / software / NRST) and lets `nucleo report` show what happened
 * right up to the reset. Dumped on demand only; never auto-printed.
 * ---------------------------------------------------------------------- */
/* 'ENL1'. Deliberately different from the KoreroNet magic ('KNL2'): a board
   re-flashed from that firmware must DISCARD its retained ring rather than
   render ghost Pi-power events, whose type codes no longer exist here. */
#define EVLOG_MAGIC   0x454E4C31u
#define EVLOG_SLOTS   32u

/* event types */
#define EV_BOOT        0u   /* generic boot (cause unknown)                    */
#define EV_RST_POR     1u   /* power-on / brown-out reset (BOR)                */
#define EV_RST_IWDG    2u   /* independent-watchdog reset  <-- the key one     */
#define EV_RST_WWDG    3u   /* window-watchdog reset                          */
#define EV_RST_SOFT    4u   /* software reset (NVIC_SystemReset)              */
#define EV_RST_PIN     5u   /* NRST pin reset                                 */
#define EV_RST_LPWR    6u   /* illegal low-power / option-byte reset          */

typedef struct {
  uint32_t epoch;    /* rtc_now_epoch2000() at the moment of the event */
  uint32_t uptime;   /* HAL_GetTick() ms (fallback when the RTC is unset) */
  uint16_t seq;      /* monotonically increasing id                    */
  uint8_t  type;     /* EV_* event type                                */
  uint8_t  arg;      /* reserved for a per-event reason code, else 0   */
} EvEntry_t;

typedef struct {
  uint32_t  magic;
  uint16_t  head;    /* next write slot                                */
  uint16_t  count;   /* valid entries (<= EVLOG_SLOTS)                 */
  uint32_t  nextseq;
  uint32_t  boots;   /* lifetime (re)boot counter                     */
  EvEntry_t e[EVLOG_SLOTS];
} EvLog_t;

/* Retained across warm resets: placed in .noinit (see the linker script). */
__attribute__((section(".noinit"))) static EvLog_t g_evlog;

static void    EvLog_Init(void);
static void    EvLog_Add(uint8_t type, uint8_t arg);
static void    EvLog_Report(void);
static uint8_t EvLog_ResetCause(void);

/* Power stats helper */
typedef struct {
  int    bus_ok;
  int    shunt_ok;
  int    use_ina;          /* 1=INA bus voltage chosen, 0=ADC fallback */
  float  v_bus;            /* INA bus voltage (V) */
  float  v_shunt;          /* INA shunt voltage (V) */
  float  current;          /* A (sign depends on shunt orientation) */
  float  v_adc;            /* ADC backup voltage (V) */
  float  v_src;            /* chosen source voltage (V) */
  float  power_W;          /* v_src * current (W) valid only if shunt_ok */
  float  soc_v;            /* SoC estimate from voltage (%) */
} PowerStats_t;

/* Latest power snapshot (silently maintained) */
static volatile PowerStats_t g_ps;
static volatile float        g_used_mAh = 0.0f;
static volatile float        g_soc_i    = 0.0f;

/* ---------------- Power history (rolling, hourly, last 25 hours) --------- */
#define PH_BINS            25u          /* 25 hours: oldest..current */
static float   g_ph_wh[PH_BINS]  = {0}; /* energy per hour (Wh) */
static float   g_ph_mAh[PH_BINS] = {0}; /* charge per hour (mAh) */

static float   g_ph_soc_i[PH_BINS] = {0}; /* time-weighted avg SoC (coulomb counter, %) */
static float   g_ph_soc_v[PH_BINS] = {0}; /* time-weighted avg SoC (voltage map, %) */
static float   g_ph_dt_h [PH_BINS] = {0}; /* accumulated hours in current bin (for averaging) */
static uint8_t g_ph_head = 0;           /* index of CURRENT hour bin [0..24] */
static uint8_t g_ph_inited = 0;         /* lazy init on first tick */
#if defined(HAL_RTC_MODULE_ENABLED)
static uint8_t g_ph_hourRTC = 0xFF;     /* last RTC hour used */
#else
static uint32_t g_ph_uptimeHour = 0;    /* last uptime-derived hour */
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
static void UART1_Send(const char *s);
static void Console_Tx(const uint8_t *d, uint16_t n);       /* write to BOTH UARTs        */
static void Korero_MailboxInit(void);                       /* CM4<->CM0+ LoRaWAN mailbox */
static void Korero_LoraQueue(const char *text, size_t n);   /* queue an uplink for CM0+   */
static void Korero_DrainTrace(void);                        /* CM0+ trace ring -> VCP     */
static void Korero_JoinTick(void);                          /* announce the join result   */
static void Korero_LoraSendPower(void);                     /* uplink battery snapshot    */
static void Korero_LoraSendPowerHistory(void);              /* uplink 24h hourly SoC      */
static int  parse_hex_bytes(const char *s, uint8_t *out, int nbytes);
static void Console_HandleLine(const char *line);           /* one console command line   */
static void Status_Led_Tick(void);                          /* external status LED (PC2/D8)*/
static void normalize_cmd(const char *in, char *out, size_t out_sz);
static void Print_MessageSyntax(void);                      /* `nucleo list message syntax`*/

/* --- EnviroNode sensor console + uplink ---------------------------------- */
static void EnvNode_PrintInfo(void);       /* `info`: identity + inventory     */
static void EnvNode_SampleAndPrint(void);  /* `nucleo sensors`                 */
static int  EnvNode_UplinkNow(void);       /* `nucleo uplink now` -> FPort 1   */
static void EnvNode_ScheduleTick(void);    /* periodic sample + uplink          */
static void EnvNode_SleepTick(void);       /* STOP2 between cycles when safe    */
static void EnvNode_SelfTest(void);        /* `nucleo selftest` — pre-flight     */
static void EnvNode_LogDump(uint32_t max_rows); /* `nucleo log dump` CSV         */
static void EnvNode_FormatCsvRow(uint32_t ep, const uint8_t *f, char *line, size_t sz);
static void EnvNode_ApplySdCreds(const sdlog_creds_t *c);  /* CONFIG.INI keys   */
uint32_t    EnvNode_EpochNow(void);              /* RTC epoch for envnode_sdlog  */
static void EnvNode_DrainDownlinks(void);  /* config string + FPort-10 commands */
static void EnvNode_ApplyConfigString(const char *text, size_t len);
static void EnvNode_PrintPowerVerdict(const char *prefix);

static int  ReadPowerStats(PowerStats_t *ps);
static void PrintPowerSnapshot(void);
static void PowerStats_Tick(void);

/* Power history helpers */
static void PH_TickIntegrate(float power_W, float current_A, float dt_s, float soc_i_pct, float soc_v_pct);
static void PH_MaybeRollHour(void);
static void PH_PrintHistory(void);

/* RTC helpers (compiled only if RTC enabled) */
#if defined(HAL_RTC_MODULE_ENABLED)
/* BCD helpers */
static inline uint8_t bcd2bin(uint8_t v){ return (uint8_t)((v>>4)*10 + (v&0x0F)); }
static inline uint8_t bin2bcd(uint8_t v){ return (uint8_t)(((v/10)<<4) | (v%10)); }

/* Accepts DD/MM/YYYY HH:MM:SS (and still DD/MM/YY) */
static uint8_t  parse_datetime_ddmmyyyy_hhmmss(const char *p, RTC_DateTypeDef *d, RTC_TimeTypeDef *t);
static int      is_leap(int y);                  /* y in full years, e.g., 2000 */
static uint32_t rtc_now_epoch2000(void);
static uint32_t make_epoch2000(const RTC_DateTypeDef *d, const RTC_TimeTypeDef *t);

/* LoRaWAN OTAA key persistence (backup registers; survives power-off w/ VBAT) */
#if LK_HAVE_PERSIST
static void     Persist_SaveLoraKeys(void);      /* backup regs + flash mirror */
static void     Persist_SaveLoraKeysToBkp(void); /* backup regs only, no wear  */
static int      Persist_LoadDefaultLoraKeys(void); /* compiled-in fallback     */
static int      Persist_LoadLoraKeys(void);
static void     Persist_ForgetLoraKeys(void);
#endif
#endif /* HAL_RTC_MODULE_ENABLED */

/* Flash-backed identity (works with or without the backup-register store). */
static void        Persist_SaveLoraKeysToFlash(void);
static int         Persist_LoadLoraKeysFromFlash(void);
static const char *Persist_KeySourceName(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* Configure the system clock (LSI/LSE per config) */
  SystemClock_Config();

  /* Initialize all configured peripherals (implementations are in CubeMX .c files) */
  MX_GPIO_Init();
  MX_ADC_Init();
  MX_I2C1_Init();          /* BME280 #2 on the board pins (PA9/PA10)  */
  MX_I2C2_Init();          /* Grove shield I2C: BME280 #1 + INA219     */
  MX_SPI1_Init();          /* MAX31865 PT1000 front-end                */
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();   /* ST-Link VCP console mirror */
#if defined(HAL_RTC_MODULE_ENABLED)
  MX_RTC_Init();
#endif

  /* Start UART1 + USART2(VCP) RX interrupts (1 byte at a time) */
  HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
  HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);
#if defined(HAL_RTC_MODULE_ENABLED)
{
    RTC_TimeTypeDef t_bcd;
    RTC_DateTypeDef d_bcd;
    uint32_t magic = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0);

    HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
    HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);

    uint8_t dd = bcd2bin(d_bcd.Date);
    uint8_t mm = bcd2bin(d_bcd.Month);
    uint8_t yy = bcd2bin(d_bcd.Year);
    uint8_t hh = bcd2bin(t_bcd.Hours);
    uint8_t mi = bcd2bin(t_bcd.Minutes);
    uint8_t ss = bcd2bin(t_bcd.Seconds);

    char msg[96];
    snprintf(msg, sizeof(msg),
             "BOOT: BKP_DR0=0x%08lX  %02u/%02u/%04u %02u:%02u:%02u\r\n",
             (unsigned long)magic,
             (unsigned)dd, (unsigned)mm, (unsigned)(2000u + yy),
             (unsigned)hh, (unsigned)mi, (unsigned)ss);
    UART1_Send(msg);
}
#endif

  /* Initialize leds and button via BSP */
  BSP_LED_Init(LED_GREEN);
  //BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Optional COM settings (not used directly below) */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;

  /* USER CODE BEGIN BSP */
  /* Configure INA219 (short timeout) */
  uint8_t ina_cfg[3] = { INA_REG_CONFIG, 0x39, 0x9F }; /* 32V, ±320mV, 12b, cont */
  (void)HAL_I2C_Master_Transmit(&hi2c2, INA219_ADDR, ina_cfg, 3, 10);

  /* Seed the coulomb counter from the INA219 bus voltage on cold boot. The
     INA219 is the primary battery source (the A4/PB14 divider is only a
     fallback and may be unpopulated), so the seed comes from the bus voltage. */
  HAL_Delay(10);                       /* let the INA219 finish a conversion */
  PowerStats_t ps_boot;
  ReadPowerStats(&ps_boot);
  float v_bat_boot = ps_boot.bus_ok ? ps_boot.v_bus : ps_boot.v_src;
  float soc_v_boot = soc_from_voltage(v_bat_boot);
  float used_mAh_guess = (1.0f - (soc_v_boot / 100.0f)) * BATTERY_NOMINAL_mAh;
  if (used_mAh_guess < 0.0f) used_mAh_guess = 0.0f;
  BatteryFlow_Reset(used_mAh_guess);

  /* --- EnviroNode sensor subsystem -----------------------------------------
     Brings up both BME280s, the MAX31865 RTD front-end, the analog block and
     the rain/wind pulse counters. A missing sensor is reported but never fatal:
     the node still uplinks the channels that do work. */
  {
    envnode_config_init();                     /* interval / cal / sensor mask */
    envnode_power_init();                      /* RTC wake-up for STOP2 sleep  */
    {
      uint32_t nrec = envnode_log_init();      /* offline sensor log (flash)   */
      char lm[96];
      snprintf(lm, sizeof(lm), "LOG   : %lu of %u records (\"nucleo log dump\" for CSV)\r\n",
               (unsigned long)nrec, (unsigned)ENVLOG_CAPACITY);
      UART1_Send(lm);

      /* SD card: mount + CONFIG.INI. No card / no reader degrades cleanly to
         the flash ring. Credentials found here outrank every stored identity —
         they are applied in the key-restore block below, after CM0+ is up. */
      (void)envnode_sdlog_init(&g_sd_creds);
      envnode_sdlog_status(lm, sizeof(lm));
      UART1_Send(lm); UART1_Send("\r\n");
    }
    env_status_t src = envnode_sensors_init();
    analog_set_winddir_offset((float)envnode_config_get_winddir_offset_deg10() / 10.0f);
    uint8_t present  = envnode_sensors_present();
    char m[112];
    snprintf(m, sizeof(m),
             "SENSORS: %s (air1=%c air2=%c rtd=%c analog=%c pulse=%c ina219=%c)\r\n",
             (src == ENV_OK) ? "all up" : "partial",
             (present & 0x01u) ? 'y' : 'n', (present & 0x02u) ? 'y' : 'n',
             (present & 0x04u) ? 'y' : 'n', (present & 0x08u) ? 'y' : 'n',
             (present & 0x10u) ? 'y' : 'n', (present & 0x20u) ? 'y' : 'n');
    UART1_Send(m);

    /* Echo the sensor set that just came back from the flash config page: after
       a field reboot the first question is always "did it keep my settings?". */
    char cfg[ENVSET_STR_MAX];
    (void)envnode_sensorset_format(envnode_config_get_sensor_mask(),
                                   envnode_config_get_interval_min(),
                                   cfg, sizeof(cfg));
    snprintf(m, sizeof(m), "CONFIG: %s\r\n", cfg);
    UART1_Send(m);
  }

  UART1_Send("BOOT: Nucleo ready\r\n");

  /* Prepare the shared LoRaWAN mailbox, THEN boot the Cortex-M0+ radio core
     (CPU2). It runs the LoRaWAN stack from flash @ 0x08020000 and polls the
     mailbox for uplink commands. Safe even if CM0+ holds only the skeleton. */
  Korero_MailboxInit();
  HAL_PWREx_ReleaseCore(PWR_CORE_CPU2);
  UART1_Send("BOOT: CM0+ (radio core) released\r\n");

  /* Restore any LoRaWAN keys persisted in backup registers and push them to the
     radio core, so the node re-joins on its own after a reset/power-cycle. */
  /* Highest priority: credentials from CONFIG.INI on the SD card — inserting a
     prepared card IS the provisioning act, no console needed. Applied and then
     persisted (backup regs + flash) so the card can be removed afterwards. */
  if (g_sd_creds.has_appkey) {
    EnvNode_ApplySdCreds(&g_sd_creds);
  } else
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
  if (Persist_LoadLoraKeys()) {
    UART1_Send("BOOT: restored LoRaWAN keys from backup; radio (re)joining\r\n");
  } else if (Persist_LoadLoraKeysFromFlash()) {
    /* Backup registers are gone (real power cut, no VBAT) — fall back to the
       flash key store and refill the backup registers for next time.
       Deliberately NOT Persist_SaveLoraKeys(): that would also rewrite the flash
       key page with byte-identical content, costing an erase cycle on page 63
       for every single power cycle. The keys just came FROM flash; only the
       volatile copy needs restoring. */
    UART1_Send("BOOT: restored LoRaWAN keys from flash; radio (re)joining\r\n");
    Persist_SaveLoraKeysToBkp();
  }
  else if (Persist_LoadDefaultLoraKeys()) {
    /* Virgin board, or the operator ran `nucleo lorawan forget`. Fall back to
       the compiled-in identity (envnode_identity.c) so the node still joins and
       is visible on the gateway rather than sitting silent until someone
       provisions it. */
    UART1_Send("BOOT: no stored keys - using compiled-in default identity "
               "(edit envnode_identity.c, or 'nucleo lorawan appkey <hex>')\r\n");
  }
#else
  if (Persist_LoadLoraKeysFromFlash()) {
    UART1_Send("BOOT: restored LoRaWAN keys from flash; radio (re)joining\r\n");
  } else if (Persist_LoadDefaultLoraKeys()) {
    UART1_Send("BOOT: no stored keys - using compiled-in default identity\r\n");
  }
#endif
  /* USER CODE END BSP */

  /* Infinite loop */
  while (1)
  {
    /* One-time bring-up on the first loop pass: event log + watchdog. */
    static uint8_t g_boot_once = 0;
    if (!g_boot_once) {
      g_boot_once = 1;

      /* Persistent event log: validate the retained ring, count this boot, and
         record WHY we just (re)booted -- brown-out / watchdog / software / pin --
         with a timestamp. Read the RCC reset flags here, before they are cleared
         below. */
      EvLog_Init();
      g_evlog.boots++;
      EvLog_Add(EvLog_ResetCause(), 0u);
      __HAL_RCC_CLEAR_RESET_FLAGS();

      /* Independent Watchdog: hardware auto-reset if the firmware ever hangs.
         Refreshed each loop pass below; a hang or a fault-handler while(1) stops
         the refresh, so the chip hard-resets after ~15 s and reboots (config and
         OTAA identity are reloaded from flash). Frozen while a debugger is
         halted so it can't fight flashing. */
      __HAL_DBGMCU_FREEZE_IWDG();
      IWDG->KR  = 0x0000CCCCu;   /* start IWDG (also starts the LSI)      */
      IWDG->KR  = 0x00005555u;   /* enable write access to PR/RLR         */
      IWDG->PR  = 0x00000006u;   /* prescaler /256                        */
      IWDG->RLR = 0x00000753u;   /* 1875 -> ~15 s @ 32 kHz LSI            */
      while (IWDG->SR != 0u) { } /* wait for PR/RLR to latch              */
      IWDG->KR  = 0x0000AAAAu;   /* initial refresh                       */
    }
    IWDG->KR = 0x0000AAAAu;      /* kick the watchdog every pass          */

    /* Keep RX armed (in case of error) on both consoles */
    if (HAL_UART_GetState(&huart1) != HAL_UART_STATE_BUSY_RX) {
      HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
    }
    if (HAL_UART_GetState(&huart2) != HAL_UART_STATE_BUSY_RX) {
      HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);
    }

    /* Forward the CM0+ radio core's LoRaWAN trace to the VCP */
    Korero_DrainTrace();
    Korero_JoinTick();                  /* announce join/leave on both consoles */

    /* Single silent call that samples INA/ADC, handles FULL detection, and updates coulomb counter */
    PowerStats_Tick();

    /* EnviroNode: apply any gateway commands, then sample + uplink on schedule */
    EnvNode_DrainDownlinks();
    EnvNode_ScheduleTick();

    /* Process any received command (handled outside ISR) */
    if (cmd_ready) {
      cmd_ready = 0;
      Console_HandleLine(cmd_buf);
    }

    /* External status LED (brief, low-duty state indicator on PC2 / D8) */
    Status_Led_Tick();

    /* Small pacing slice to keep ISR latency low */
    uint32_t until = HAL_GetTick() + 50;
    while ((int32_t)(HAL_GetTick() - until) < 0) {
      if (cmd_ready) {
        cmd_ready = 0;
        Console_HandleLine(cmd_buf);
      }
      /* Keep the VCP fed with the radio core's trace */
      Korero_DrainTrace();
      /* Status LED — called here too so pulses stay crisp (~5 ms resolution) */
      Status_Led_Tick();
      HAL_Delay(5);  /* Does NOT disable UART interrupts — only adds latency */
    }

    /* Last thing in the pass: stop the core until the next measurement is due,
       if the selected sensors and the post-transmission window allow it. */
    EnvNode_SleepTick();
  }
}

/* -------------------- Clocks (prefer LSE for RTC; fallback to LSI) -------- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2);

  /* Try LSE first for RTC accuracy */
  HAL_PWR_EnableBkUpAccess();                               // allow backup-domain writes
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    /* Fallback: disable LSE, enable LSI for RTC */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
    RCC_OscInitStruct.LSEState = RCC_LSE_OFF;
    RCC_OscInitStruct.LSIState = RCC_LSI_ON;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
      Error_Handler();
    }
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) {
    Error_Handler();
  }

  /* Route RTC clock */
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
#if defined(RCC_OSCILLATORTYPE_LSE)
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY)) {
    PeriphClkInit.RTCClockSelection    = RCC_RTCCLKSOURCE_LSE;
  } else
#endif
  {
    PeriphClkInit.RTCClockSelection    = RCC_RTCCLKSOURCE_LSI;
  }
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
    Error_Handler();
  }
}


/* USER CODE BEGIN 4 */

/* ---- CM4 <-> CM0+ LoRaWAN mailbox ---------------------------------------- */
/* Initialise the shared mailbox. Must run BEFORE the CM0+ core is released so
   that core sees a clean, magic-stamped structure. */
static void Korero_MailboxInit(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  mb->req_seq = 0;
  mb->ack_seq = 0;
  mb->status  = KORERO_ST_IDLE;
  mb->port    = 0;
  mb->len     = 0;
  mb->trace_head = 0;
  mb->trace_tail = 0;
  mb->key_seq = 0;
  mb->key_ack = 0;
  mb->dl_head = 0;
  mb->dl_tail = 0;
  for (int i = 0; i < 8; i++)  { mb->dev_eui[i] = 0; mb->join_eui[i] = 0; }
  for (int i = 0; i < 16; i++) { mb->app_key[i] = 0; }   /* all-zero => keep chip DevEUI */
  mb->deveui_ready = 0;            /* CM0+ fills dev_eui_now once the stack is up */
  mb->pi_pwr_seq = 0;              /* legacy B1/B2 channel; CM4 no longer acts on it */
  mb->pi_pwr_on  = 0;
  mb->joined  = 0;                 /* radio not joined yet (CM0+ sets it on join)  */
  mb->magic   = KORERO_MB_MAGIC;   /* write magic last */
}

/* ---- Persistent event log implementation (declarations near the top) ----- */

/* Validate the retained ring; clear it only on a cold boot or corruption. */
static void EvLog_Init(void)
{
  if (g_evlog.magic != EVLOG_MAGIC) {          /* cold boot or garbage */
    memset((void *)&g_evlog, 0, sizeof(g_evlog));
    g_evlog.magic   = EVLOG_MAGIC;
    g_evlog.nextseq = 1u;
  }
  if (g_evlog.head  >= EVLOG_SLOTS) g_evlog.head  = 0u;   /* paranoia vs. bit-rot */
  if (g_evlog.count >  EVLOG_SLOTS) g_evlog.count = EVLOG_SLOTS;
}

/* Append one timestamped event to the ring (overwrites the oldest when full). */
static void EvLog_Add(uint8_t type, uint8_t arg)
{
  uint16_t h = g_evlog.head;
  if (h >= EVLOG_SLOTS) h = 0u;
#if defined(HAL_RTC_MODULE_ENABLED)
  g_evlog.e[h].epoch = rtc_now_epoch2000();
#else
  g_evlog.e[h].epoch = 0u;
#endif
  g_evlog.e[h].uptime = HAL_GetTick();
  g_evlog.e[h].seq    = (uint16_t)g_evlog.nextseq;
  g_evlog.e[h].type   = type;
  g_evlog.e[h].arg    = arg;
  g_evlog.nextseq++;
  g_evlog.head = (uint16_t)((h + 1u) % EVLOG_SLOTS);
  if (g_evlog.count < EVLOG_SLOTS) g_evlog.count++;
}

/* Classify the cause of THIS boot from the RCC reset flags. MUST be called
   before __HAL_RCC_CLEAR_RESET_FLAGS(). A watchdog reset also asserts PINRST,
   so the specific causes are tested before the pin. */
static uint8_t EvLog_ResetCause(void)
{
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) return EV_RST_IWDG;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) return EV_RST_WWDG;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) return EV_RST_LPWR;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  return EV_RST_SOFT;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))  return EV_RST_POR;
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  return EV_RST_PIN;
  return EV_BOOT;
}

/* Format seconds-since-2000 as "YYYY-MM-DD HH:MM:SS" (self-contained). */
static void EvLog_FmtTime(uint32_t e, char *buf, size_t n)
{
  static const uint8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint32_t s  = e % 60u; e /= 60u;
  uint32_t mi = e % 60u; e /= 60u;
  uint32_t h  = e % 24u; e /= 24u;      /* e = days since 2000-01-01 */
  int y = 2000;
  for (;;) {
    uint32_t dy = (((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0)) ? 366u : 365u;
    if (e >= dy) { e -= dy; y++; } else break;
  }
  int mo;
  for (mo = 0; mo < 12; mo++) {
    uint32_t dm = md[mo];
    if (mo == 1 && ((((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0)))) dm++;
    if (e >= dm) e -= dm; else break;
  }
  snprintf(buf, n, "%04d-%02d-%02d %02lu:%02lu:%02lu",
           y, mo + 1, (int)(e + 1u),
           (unsigned long)h, (unsigned long)mi, (unsigned long)s);
}

/* Dump the whole ring, oldest -> newest, to both consoles. */
static void EvLog_Report(void)
{
  static const char *RSTNAME[] = {
    "boot", "power-on / BROWN-OUT (BOR)", "WATCHDOG reset (IWDG)",
    "window-watchdog reset", "software reset", "NRST pin reset",
    "low-power / illegal reset"
  };
  char line[128];
  char whenbuf[24];

#if defined(HAL_RTC_MODULE_ENABLED)
  uint32_t now = rtc_now_epoch2000();
#else
  uint32_t now = 0u;
#endif
  EvLog_FmtTime(now, whenbuf, sizeof(whenbuf));
  snprintf(line, sizeof(line),
           "REPORT: EnviroNode-WL55 event log v" KORERO_FW_VERSION
           "  now=%s  boots=%lu  events=%u\r\n",
           whenbuf, (unsigned long)g_evlog.boots, (unsigned)g_evlog.count);
  UART1_Send(line);
  UART1_Send("REPORT:  #  seq  when                 up(s)   event\r\n");

  uint16_t n   = g_evlog.count;
  uint16_t idx = (uint16_t)((g_evlog.head + EVLOG_SLOTS - n) % EVLOG_SLOTS); /* oldest */
  for (uint16_t i = 0; i < n; i++) {
    EvEntry_t *e = &g_evlog.e[idx];
    EvLog_FmtTime(e->epoch, whenbuf, sizeof(whenbuf));
    uint8_t t = (e->type <= EV_RST_LPWR) ? e->type : 0u;
    snprintf(line, sizeof(line), "REPORT: %2u  %3u  %s  %6lu  %s\r\n",
             (unsigned)(i + 1u), (unsigned)e->seq, whenbuf,
             (unsigned long)(e->uptime / 1000u), RSTNAME[t]);
    UART1_Send(line);
    idx = (uint16_t)((idx + 1u) % EVLOG_SLOTS);
  }
  UART1_Send("REPORT: end\r\n");
}

/* Announce the CM0+ radio's LoRaWAN join result on the consoles. The radio core
   only logs "= JOINED =" to its trace ring, so watching the mailbox `joined`
   flag here gives a single, greppable line per state change. */
static void Korero_JoinTick(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  static uint32_t last = 0u;       /* matches mailbox init: no boot false-positive */
  if (mb->magic != KORERO_MB_MAGIC) { return; }
  uint32_t j = mb->joined;
  if (j != last) {
    last = j;
    UART1_Send(j ? "JOINED\r\n" : "JOIN FAILED\r\n");
  }
}

/* Stage a payload and signal the CM0+ radio core to transmit it via LoRaWAN. */
static void Korero_LoraQueue(const char *text, size_t n)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  if (n > KORERO_MB_MAX_PAYLOAD) { n = KORERO_MB_MAX_PAYLOAD; }
  for (size_t i = 0; i < n; i++)
  {
    mb->payload[i] = (uint8_t)text[i];
  }
  mb->len    = (uint8_t)n;
  mb->port   = 0;                  /* 0 => CM0+ uses its default app port */
  mb->status = KORERO_ST_IDLE;
  __DMB();                         /* payload must be visible before the commit */
  mb->req_seq = mb->req_seq + 1;   /* commit: CM0+ picks this up on its next poll */
}

/* Write a buffer to BOTH consoles: USART1 (pin header) and USART2 (ST-Link VCP). */
static void Console_Tx(const uint8_t *d, uint16_t n)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)d, n, 50);
  HAL_UART_Transmit(&huart2, (uint8_t *)d, n, 50);
}

/* Drain the CM0+ -> CM4 trace ring and print it on the VCP (USB) only, so the
   radio core's verbose LoRaWAN logs stay off the USART1 pin header. */
static void Korero_DrainTrace(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  if (mb->magic != KORERO_MB_MAGIC) { return; }

  uint32_t tail = mb->trace_tail & KORERO_TRACE_MASK;
  uint32_t head = mb->trace_head & KORERO_TRACE_MASK;
  uint8_t  chunk[64];
  uint16_t n = 0;

  while ((tail != head) && (n < sizeof(chunk)))
  {
    chunk[n++] = mb->trace[tail];
    tail = (tail + 1U) & KORERO_TRACE_MASK;
  }
  if (n > 0)
  {
    mb->trace_tail = tail;
    HAL_UART_Transmit(&huart2, chunk, n, 50);   /* VCP only */
  }
}

/* Uplink a compact battery snapshot to TTN (fire-and-forget via the mailbox).
   7 bytes: [0x01][SoC_i %][SoC_v %][Vsrc centi-volts LE16][current mA signed LE16]. */
static void Korero_LoraSendPower(void)
{
  PowerStats_t ps;
  if (!ReadPowerStats(&ps)) { ps = g_ps; }

  int soc_i = (int)(g_soc_i  + 0.5f); if (soc_i < 0) soc_i = 0; if (soc_i > 100) soc_i = 100;
  int soc_v = (int)(ps.soc_v + 0.5f); if (soc_v < 0) soc_v = 0; if (soc_v > 100) soc_v = 100;
  int cv    = (int)(ps.v_src * 100.0f + 0.5f); if (cv < 0) cv = 0; if (cv > 65535) cv = 65535;
  int ma    = (int)(ps.current * 1000.0f);     if (ma > 32767) ma = 32767; if (ma < -32768) ma = -32768;
  uint16_t uma = (uint16_t)((int16_t)ma);

  uint8_t b[7];
  b[0] = 0x01;
  b[1] = (uint8_t)soc_i;
  b[2] = (uint8_t)soc_v;
  b[3] = (uint8_t)(cv & 0xFF);
  b[4] = (uint8_t)((cv >> 8) & 0xFF);
  b[5] = (uint8_t)(uma & 0xFF);
  b[6] = (uint8_t)((uma >> 8) & 0xFF);
  Korero_LoraQueue((const char *)b, sizeof(b));
}

/* Uplink the last 24 h of hourly state-of-charge (coulomb-counter %) to TTN.
   26 bytes: [0x02][count=24][soc_oldest .. soc_newest]; 0xFF = hour had no data. */
static void Korero_LoraSendPowerHistory(void)
{
  uint8_t b[2 + 24];
  b[0] = 0x02;
  b[1] = 24;
  for (uint32_t j = 0; j < 24; j++) {
    uint32_t idx = (g_ph_head + 1u + j) % PH_BINS;   /* oldest .. newest */
    if (g_ph_dt_h[idx] <= 0.0001f) {
      b[2 + j] = 0xFF;                               /* no data this hour */
    } else {
      int s = (int)(g_ph_soc_i[idx] + 0.5f);
      if (s <   0) s =   0;
      if (s > 100) s = 100;
      b[2 + j] = (uint8_t)s;
    }
  }
  Korero_LoraQueue((const char *)b, sizeof(b));
}

/* ==========================================================================
   EnviroNode: sensor console + sensor uplink
   ========================================================================== */

/* Format 8/16 raw bytes as uppercase hex into `dst` (dst must hold 2*n+1). */
static void hexdump_into(char *dst, size_t dst_sz, const volatile uint8_t *src, size_t n)
{
  static const char HEX[] = "0123456789ABCDEF";
  size_t w = 0;
  for (size_t i = 0; i < n && (w + 2u) < dst_sz; ++i) {
    uint8_t v = src[i];
    dst[w++] = HEX[(v >> 4) & 0x0Fu];
    dst[w++] = HEX[v & 0x0Fu];
  }
  dst[w] = '\0';
}

/* `info` — everything you need to register the node on TTN plus what hardware
   answered at boot. The AppKey is printed because provisioning a replacement
   node from the console is the normal field workflow for this project. */
static void EnvNode_PrintInfo(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  char line[128];
  char hex[40];

  UART1_Send("---- EnviroNode-WL55 ----\r\n");
  snprintf(line, sizeof(line), "Firmware  : v%s\r\n", KORERO_FW_VERSION);
  UART1_Send(line);

  /* DevEUI: prefer the one the radio core is actually using. */
  if (mb->magic == KORERO_MB_MAGIC && mb->deveui_ready) {
    hexdump_into(hex, sizeof(hex), mb->dev_eui_now, 8);
    snprintf(line, sizeof(line), "DevEUI    : %s\r\n", hex);
  } else {
    snprintf(line, sizeof(line), "DevEUI    : (radio core not ready; retry in a second)\r\n");
  }
  UART1_Send(line);

  /* JoinEUI / AppEUI — all-zero is the TTN default. */
  hexdump_into(hex, sizeof(hex), mb->join_eui, 8);
  snprintf(line, sizeof(line), "AppEUI    : %s\r\n", hex);
  UART1_Send(line);

  /* AppKey — the stored provisioning key. Zero means the radio core is still
     running its compiled-in placeholder. */
  uint8_t anz = 0; for (int i = 0; i < 16; ++i) anz |= mb->app_key[i];
  if (anz) {
    hexdump_into(hex, sizeof(hex), mb->app_key, 16);
    snprintf(line, sizeof(line), "AppKey    : %s\r\n", hex);
  } else {
    snprintf(line, sizeof(line), "AppKey    : (none stored — radio core default in use)\r\n");
  }
  UART1_Send(line);

  snprintf(line, sizeof(line), "Key store : %s\r\n", Persist_KeySourceName());
  UART1_Send(line);

  snprintf(line, sizeof(line), "Joined    : %s\r\n",
           (mb->magic == KORERO_MB_MAGIC && mb->joined) ? "yes" : "no");
  UART1_Send(line);

  /* The sensor set in its canonical form (docs/CONFIG.md): what `{?}` returns
     and what can be pasted straight into another node. */
  const uint8_t sel = envnode_config_get_sensor_mask();
  const uint16_t interval = envnode_config_get_interval_min();
  char setstr[ENVSET_STR_MAX];
  (void)envnode_sensorset_format(sel, interval, setstr, sizeof(setstr));
  snprintf(line, sizeof(line), "Config    : %s   (mask 0x%02X)\r\n",
           setstr, (unsigned)sel);
  UART1_Send(line);

  snprintf(line, sizeof(line), "Uplink    : every %u min on FPort %u (30-byte frame)%s\r\n",
           (unsigned)interval, (unsigned)ENVNODE_UPLINK_FPORT,
           (sel == SENSOR_NONE) ? " - PAUSED, set is {NONE}" : "");
  UART1_Send(line);

  EnvNode_PrintPowerVerdict("Power     ");

  snprintf(line, sizeof(line), "Vane offs : %.1f deg\r\n",
           (double)envnode_config_get_winddir_offset_deg10() / 10.0);
  UART1_Send(line);

  uint8_t present = envnode_sensors_present();
  snprintf(line, sizeof(line),
           "Sensors   : air1(I2C2)=%c air2(I2C1)=%c pt1000=%c analog=%c pulse=%c ina219=%c\r\n",
           (present & 0x01u) ? 'y' : 'n', (present & 0x02u) ? 'y' : 'n',
           (present & 0x04u) ? 'y' : 'n', (present & 0x08u) ? 'y' : 'n',
           (present & 0x10u) ? 'y' : 'n', (present & 0x20u) ? 'y' : 'n');
  UART1_Send(line);
  UART1_Send("-------------------------\r\n");
}

/* `nucleo sensors` — take a full frame and print it in engineering units. */
static void EnvNode_SampleAndPrint(void)
{
  sensor_readings_t r;
  char line[128];

  /* Peek, not sample: the rain/wind accumulators belong to the next uplink. */
  if (envnode_sensors_peek(&r) != ENV_OK) {
    UART1_Send("ERR: sensor sample failed\r\n");
    return;
  }

  snprintf(line, sizeof(line), "air1 : %.2f C  %.1f %%RH  %.1f hPa  [%s]\r\n",
           r.air1_temp_c, r.air1_rh_pct, r.air1_press_hpa,
           (r.status & SENS_OK_AIR1) ? "ok" : "FAIL");
  UART1_Send(line);

  snprintf(line, sizeof(line), "air2 : %.2f C  %.1f %%RH  %.1f hPa  [%s]\r\n",
           r.air2_temp_c, r.air2_rh_pct, r.air2_press_hpa,
           (r.status & SENS_OK_AIR2) ? "ok" : "FAIL");
  UART1_Send(line);

  snprintf(line, sizeof(line), "soil : moisture %u counts  temp %.2f C [%s]\r\n",
           (unsigned)r.soil_moist_raw, r.soil_temp_c,
           (r.status & SENS_OK_PT1000) ? "ok" : "RTD FAIL");
  UART1_Send(line);

  /* The Decagon LWS is specified in millivolts, so print both: counts are what
     goes on air, mV is what you compare against the datasheet threshold. */
  snprintf(line, sizeof(line), "leaf : %u counts = %u mV  [%s]\r\n",
           (unsigned)r.leaf_wet_raw,
           (unsigned)(((uint32_t)r.leaf_wet_raw * 3300u) / 4095u),
           (r.status & SENS_OK_LEAF) ? "ok" : "FAIL");
  UART1_Send(line);

  snprintf(line, sizeof(line), "wind : %.2f m/s  gust %.2f m/s  dir %.1f deg  [%s]\r\n",
           r.wind_speed_ms, r.wind_gust_ms, r.wind_dir_deg,
           (r.status & SENS_OK_WIND) ? "ok" : "FAIL");
  UART1_Send(line);

  snprintf(line, sizeof(line), "rain : %u tips  %.2f mm this interval\r\n",
           (unsigned)r.rain_tips, r.rain_mm);
  UART1_Send(line);

  snprintf(line, sizeof(line), "batt : %.2f V\r\nstatus: 0x%02X\r\n",
           r.batt_v, (unsigned)r.status);
  UART1_Send(line);
}

/* `nucleo uplink now` — sample, pack the 30-byte FPort-1 frame (docs/PAYLOAD.md)
   and hand it to the radio core. Blocks until CM0+ acknowledges, feeding the
   watchdog so the bounded (<=12 s) wait is never mistaken for a hang. */
static int EnvNode_UplinkNow(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  sensor_readings_t r;
  uint8_t frame[ENVNODE_UPLINK_LEN];

  /* Peek rather than sample: peeking leaves the rain/wind accumulators running.
     They are only cleared once the radio core confirms the frame went out (see
     below) — otherwise a refused uplink ("not joined yet", duty-cycle hold)
     would silently destroy that interval's rainfall, which is unrecoverable. */
  if (envnode_sensors_peek(&r) != ENV_OK) {
    UART1_Send("ERR: sensor sample failed\r\n");
    return 0;
  }
  size_t n = envnode_payload_pack(&r, frame, sizeof(frame));
  if (n != ENVNODE_UPLINK_LEN) {
    UART1_Send("ERR: payload pack failed\r\n");
    return 0;
  }

  /* Offline log FIRST, before any radio involvement: a measurement must reach
     flash even when the node has never joined — that is the whole point of an
     offline log. This is also the safe moment for the occasional page erase
     (every 51st record): the radio is idle here, so the ~20-40 ms stall cannot
     land inside an RX window. */
#if defined(HAL_RTC_MODULE_ENABLED)
  {
    const uint32_t ep = rtc_now_epoch2000();
    if (!envnode_log_append(ep, frame)) {
      UART1_Send("WARN: offline log write failed\r\n");
    }
    /* Same reading, same format, onto the SD card when one is mounted. */
    if (envnode_sdlog_active()) {
      char row[192];
      EnvNode_FormatCsvRow(ep, frame, row, sizeof(row));
      if (!envnode_sdlog_append(ep, ENVNODE_CSV_HEADER, row)) {
        UART1_Send("WARN: SD write failed - SD logging disabled (flash ring continues)\r\n");
      }
    }
  }
#endif

  if (mb->magic != KORERO_MB_MAGIC) {
    UART1_Send("ERR: mailbox not ready (radio core down?)\r\n");
    return 0;
  }

  for (size_t i = 0; i < n; ++i) { mb->payload[i] = frame[i]; }
  mb->len    = (uint8_t)n;
  mb->port   = ENVNODE_UPLINK_FPORT;      /* FPort 1 = sensor frame */
  mb->status = KORERO_ST_IDLE;
  uint32_t want = mb->req_seq + 1u;
  __DMB();
  mb->req_seq = want;                     /* commit -> CM0+ transmits */
  g_last_tx_ms = HAL_GetTick();           /* starts the post-TX awake window */

  uint32_t t0 = HAL_GetTick();
  while (mb->ack_seq != want) {
    if ((HAL_GetTick() - t0) > 12000u) {
      UART1_Send("ERR: uplink timeout (radio core did not ack)\r\n");
      return 0;
    }
    IWDG->KR = 0x0000AAAAu;               /* bounded wait, not a hang */
    Korero_DrainTrace();
  }

  if (mb->status == KORERO_ST_SENT) {
    /* The frame is away, so this interval's rain/wind totals have been reported
       and can be cleared. Pulses that arrived during the transmission itself are
       lost — bounded by the airtime, and far cheaper than losing a whole
       interval every time the radio refuses. */
    uint16_t tips; float mm;
    (void)pulse_read_and_reset(&tips, &mm);

    char line[64];
    snprintf(line, sizeof(line), "ACK: uplink sent, %u bytes on FPort %u\r\n",
             (unsigned)n, (unsigned)ENVNODE_UPLINK_FPORT);
    UART1_Send(line);
    return 1;
  }
  UART1_Send((mb->status == KORERO_ST_NOJOIN) ? "ERR: not joined yet\r\n"
                                              : "ERR: radio busy / duty cycle\r\n");
  return 0;
}

/**
  * @brief  Print the sleep verdict for the running sensor set (report only).
  * @param  prefix  Line prefix — "ACK", or the column-aligned "Power     " that
  *                 `info` uses.
  *
  * Phase 5 owns the actual STOP2 implementation; all this does is tell the
  * operator whether the selection they are looking at would ever permit it, and
  * which sensor is keeping the node awake if not (docs/CONFIG.md).
  */
static void EnvNode_PrintPowerVerdict(const char *prefix)
{
  const uint8_t sel = envnode_config_get_sensor_mask();
  char line[160];
  const char *state;

  if (envnode_sensorset_requires_awake(sel)) {
    state = "";                                   /* the reason says it all   */
  } else if (!envnode_power_is_enabled()) {
    state = "  (sleep disabled by 'nucleo sleep off')";
  } else {
    state = "  (STOP2 between cycles, RTC wake)";
  }

  snprintf(line, sizeof(line), "%s: %s%s\r\n", prefix,
           envnode_sensorset_awake_reason(sel), state);
  UART1_Send(line);
}

/**
  * @brief  Apply one sensor-set configuration string and report it.
  * @param  text  Start of the string (points at '{'); need not be NUL-terminated.
  * @param  len   Readable bytes at @p text.
  *
  * The single place both transports converge on, so a string sent by a gateway
  * and one typed on the console behave — and read back — identically. Parsing is
  * all-or-nothing: a rejected frame changes nothing and writes no flash, and the
  * console names the offending token so the operator can fix it (docs/CONFIG.md).
  * A downlink is echoed here too, so someone standing at the node can see what
  * the gateway just changed.
  */
static void EnvNode_ApplyConfigString(const char *text, size_t len)
{
  char reply[ENVNODE_CFGSTR_REPLY_MAX];
  char line[128];

  if (envnode_apply_config_string(text, len, reply, sizeof(reply)) == ENV_OK) {
    /* `reply` is the canonical rendering plus "saved"/"unchanged"/"NOT SAVED" —
       rendered from the stored config, so it is proof of what the node holds. */
    snprintf(line, sizeof(line), "ACK: config %s\r\n", reply);
    UART1_Send(line);
    EnvNode_PrintPowerVerdict("ACK");
  } else {
    snprintf(line, sizeof(line),
             "ERR: config rejected -- bad token '%s' (nothing applied)\r\n", reply);
    UART1_Send(line);
  }
}

/* Periodic sample + uplink. Runs from the main loop; the first frame goes out a
   short while after boot so the radio core has a chance to join first, then one
   frame every configured interval (the {…} config string and downlink 0x01 both
   change it). `uplink_now` requests (downlink 0x02 / console) are serviced here
   too, so an uplink is never started from inside the downlink drain. */
static void EnvNode_ScheduleTick(void)
{
  const uint16_t interval  = envnode_config_get_interval_min();
  const uint32_t period_ms = (uint32_t)interval * 60000u;
  const uint8_t  sel       = envnode_config_get_sensor_mask();

  if (!g_sched_armed) {               /* first pass: schedule the boot frame */
    g_next_due_ms   = HAL_GetTick() + ENVNODE_FIRST_UPLINK_DELAY_MS;
    g_armed_interval = interval;
    g_sched_armed   = 1u;
  }

  /* The interval changed (config string, downlink 0x01, or the console) while a
     deadline was already pending: restart the period from now. Without this the
     old deadline runs to completion first, so shortening the interval to watch
     the node looks like the command was ignored — the previous, longer period
     still has to elapse. Restarting from now is also the behaviour an operator
     predicts. */
  if (interval != g_armed_interval) {
    g_next_due_ms    = HAL_GetTick() + period_ms;
    g_armed_interval = interval;
  }

  /* An explicit request beats everything, including {NONE}: someone asked for a
     frame, and a battery-only frame is still a useful "I am alive". */
  if (envnode_config_take_uplink_request()) {
    (void)EnvNode_UplinkNow();
    g_next_due_ms = HAL_GetTick() + period_ms;
    return;
  }

  /* {NONE} parks the measurement cycle — with nothing selected every field but
     the battery would be a sentinel, which is not worth the airtime. Said once
     per transition, not once per pass, or the console fills with it. */
  if (sel == SENSOR_NONE) {
    if (!g_sched_paused) {
      UART1_Send("INFO: sensor set is {NONE} - periodic uplinks paused "
                 "('nucleo uplink now' still sends one)\r\n");
      g_sched_paused = 1u;
    }
    /* Keep the deadline rolling while parked. Letting it fall arbitrarily far
       into the past would eventually wrap the signed tick comparison below. */
    g_next_due_ms = HAL_GetTick() + period_ms;
    return;
  }
  if (g_sched_paused) {
    /* Sensors just came back: send one frame now rather than after another full
       interval, so the operator sees the new set land on the gateway. */
    g_sched_paused = 0u;
    g_next_due_ms = HAL_GetTick();
  }

  if ((int32_t)(HAL_GetTick() - g_next_due_ms) >= 0) {
    int sent = EnvNode_UplinkNow();   /* failures are reported on the console */
    /* A refusal is usually "not joined yet" or a duty-cycle hold, both of which
       clear in well under a minute — retry soon rather than skipping a whole
       interval, which would drop the first frames of a deployment while the node
       is still joining. */
    g_next_due_ms = HAL_GetTick() + (sent ? period_ms : ENVNODE_UPLINK_RETRY_MS);
  }
}

/**
  * @brief  Dump the offline log as CSV on the console (`nucleo log dump [n]`).
  *
  * One row per record, newest first, decoded with the same offsets/scalings as
  * docs/PAYLOAD.md — the log stores the transmitted frame verbatim, so this is
  * the single place log bytes become engineering units. A field whose OK-bit was
  * clear (sentinel on air) prints as an empty CSV cell, not a fake zero.
  */
/**
  * @brief  Apply CONFIG.INI credentials: mailbox → radio joins; persist only if
  *         they differ from the flash keystore (a card left inserted must not
  *         cost a page erase per boot).
  */
static void EnvNode_ApplySdCreds(const sdlog_creds_t *c)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint8_t f_app[16], f_dev[8], f_join[8];
  int same = 0;

  if (env_keystore_load(f_app, f_dev, f_join, NULL)) {
    same = (memcmp(f_app, c->app_key, 16) == 0) &&
           (!c->has_deveui  || memcmp(f_dev,  c->dev_eui,  8) == 0) &&
           (!c->has_joineui || memcmp(f_join, c->join_eui, 8) == 0);
  }

  for (int i = 0; i < 8;  i++) mb->dev_eui[i]  = c->has_deveui  ? c->dev_eui[i]  : 0u;
  for (int i = 0; i < 8;  i++) mb->join_eui[i] = c->has_joineui ? c->join_eui[i] : 0u;
  for (int i = 0; i < 16; i++) mb->app_key[i]  = c->app_key[i];
  __DMB();
  mb->key_seq = mb->key_seq + 1u;          /* CM0+ applies + joins */

  if (same) {
    UART1_Send("BOOT: CONFIG.INI keys match the stored identity; applied\r\n");
  } else {
    UART1_Send("BOOT: LoRaWAN keys taken from SD CONFIG.INI; persisting\r\n");
    Persist_SaveLoraKeys();                /* backup regs + flash mirror */
  }
}

/** RTC epoch for modules that cannot see the static helper (SD timestamps). */
uint32_t EnvNode_EpochNow(void)
{
#if defined(HAL_RTC_MODULE_ENABLED)
  return rtc_now_epoch2000();
#else
  return 0u;
#endif
}

static void EnvNode_FormatCsvRow(uint32_t ep, const uint8_t *f, char *line, size_t sz)
{

    /* epoch2000 -> civil date (Gregorian; same convention as make_epoch2000) */
    uint32_t days = ep / 86400u, rem = ep % 86400u;
    uint32_t hh = rem / 3600u, mi = (rem % 3600u) / 60u, ss = rem % 60u;
    uint32_t y = 2000u;
    for (;;) {
      uint32_t dy = is_leap((int)y) ? 366u : 365u;
      if (days < dy) break;
      days -= dy; y++;
    }
    static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t mo = 0u;
    for (; mo < 12u; ++mo) {
      uint32_t dm = dim[mo] + ((mo == 1u && is_leap((int)y)) ? 1u : 0u);
      if (days < dm) break;
      days -= dm;
    }

    /* Little-endian field getters over the stored frame. */
    #define G16(o)  ((uint16_t)(f[(o)] | ((uint16_t)f[(o) + 1] << 8)))
    #define GS16(o) ((int16_t)G16(o))
    const uint8_t st = f[1];

    int n = snprintf(line, sz,
                     "%04lu-%02lu-%02lu %02lu:%02lu:%02lu,%lu,0x%02X,%.3f,",
                     (unsigned long)y, (unsigned long)(mo + 1u), (unsigned long)(days + 1u),
                     (unsigned long)hh, (unsigned long)mi, (unsigned long)ss,
                     (unsigned long)ep, (unsigned)st, (double)G16(2) / 1000.0);

    #define CAT(...) n += snprintf(line + n, sz - (size_t)n, __VA_ARGS__)
    if (st & SENS_OK_AIR1) CAT("%.2f,%.1f,%.1f,", GS16(4) / 100.0, f[6] / 2.0, G16(7) / 10.0);
    else                   CAT(",,,");
    if (st & SENS_OK_AIR2) CAT("%.2f,%.1f,%.1f,", GS16(9) / 100.0, f[11] / 2.0, G16(12) / 10.0);
    else                   CAT(",,,");
    if (st & SENS_OK_SOIL)   CAT("%u,",   (unsigned)G16(14)); else CAT(",");
    if (st & SENS_OK_LEAF)   CAT("%u,",   (unsigned)G16(16)); else CAT(",");
    if (st & SENS_OK_PT1000) CAT("%.2f,", GS16(18) / 100.0);  else CAT(",");
    if (st & SENS_OK_WIND)   CAT("%.2f,%.1f,%.2f,", G16(20) / 100.0, G16(22) / 10.0, G16(24) / 100.0);
    else                     CAT(",,,");
    if (st & SENS_OK_RAIN)   CAT("%u,%.2f", (unsigned)G16(26), G16(28) / 100.0);
    else                     CAT(",");
    CAT("\r\n");
    #undef CAT
    #undef GS16
    #undef G16
}

static void EnvNode_LogDump(uint32_t max_rows)
{
  const uint32_t total = envnode_log_count();
  uint32_t rows = (max_rows == 0u || max_rows > total) ? total : max_rows;
  char line[192];

  snprintf(line, sizeof(line), "LOG: %lu records stored, printing %lu (newest first)\r\n",
           (unsigned long)total, (unsigned long)rows);
  UART1_Send(line);
  UART1_Send(ENVNODE_CSV_HEADER);

  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t ep = 0;
    uint8_t f[ENVLOG_FRAME_LEN];
    if (!envnode_log_get(i, &ep, f)) break;
    IWDG->KR = 0x0000AAAAu;      /* a full dump takes seconds on two UARTs */
    EnvNode_FormatCsvRow(ep, f, line, sizeof(line));
    UART1_Send(line);
  }
  UART1_Send("LOG: end\r\n");
}

/**
  * @brief  `nucleo selftest` — exercise the whole pipeline without a gateway.
  *
  * The point is to separate "my wiring is wrong" from "my firmware is wrong"
  * before either is suspected. It runs four independent checks and prints a
  * PASS/FAIL line for each:
  *
  *   1. **Config-string parser** — a table of accept / reject / edit / query
  *      vectors, so the grammar is proven on the target, not just on paper.
  *   2. **Payload packer** — a synthetic reading with known values is packed and
  *      compared byte-for-byte against the expected frame. The frame is also
  *      printed as hex so it can be pasted into the TTN payload-formatter tester
  *      to validate the decoder before a single radio packet exists.
  *   3. **I²C bus scan** — both buses, so a mis-wired or mis-strapped BME280
  *      shows up as an address on the wrong bus rather than a silent failure.
  *   4. **Sensor read** — one live sample through the real drivers.
  */
static void EnvNode_SelfTest(void)
{
  char line[160];
  int  pass = 0, fail = 0;

  UART1_Send("---- selftest ----\r\n");

  /* --- 1. config-string parser ------------------------------------------- */
  {
    static const struct {
      const char *in;
      envset_result_t want;
      uint8_t  want_mask;
      uint16_t want_interval;
    } vec[] = {
      /* full replace, with interval                                          */
      { "{T1,T2,15}",   ENVSET_ACCEPTED, (uint8_t)(SENSOR_T1 | SENSOR_T2), 15u },
      /* case and whitespace are irrelevant                                    */
      { "{ t1 , R , 5 }", ENVSET_ACCEPTED, (uint8_t)(SENSOR_T1 | SENSOR_R), 5u },
      /* aliases                                                               */
      { "{ALL,15}",     ENVSET_ACCEPTED, SENSOR_ALL,  15u },
      { "{NONE}",       ENVSET_ACCEPTED, SENSOR_NONE, 1u  },
      /* incremental edit of the current set (T1|T2 @1)                        */
      { "{+R}",         ENVSET_ACCEPTED, (uint8_t)(SENSOR_T1 | SENSOR_T2 | SENSOR_R), 1u },
      { "{-T2}",        ENVSET_ACCEPTED, SENSOR_T1, 1u },
      /* interval only                                                         */
      { "{5}",          ENVSET_ACCEPTED, (uint8_t)(SENSOR_T1 | SENSOR_T2), 5u },
      /* query changes nothing                                                 */
      { "{?}",          ENVSET_QUERY,    (uint8_t)(SENSOR_T1 | SENSOR_T2), 1u },
      /* rejects: unknown key, out-of-range interval, malformed                */
      { "{T1,XX}",      ENVSET_REJECTED, 0u, 0u },
      { "{T1,0}",       ENVSET_REJECTED, 0u, 0u },
      { "{T1,1000}",    ENVSET_REJECTED, 0u, 0u },
      { "{}",           ENVSET_REJECTED, 0u, 0u },
      { "{T1",          ENVSET_REJECTED, 0u, 0u },
    };
    const uint8_t  cur_mask     = (uint8_t)(SENSOR_T1 | SENSOR_T2);
    const uint16_t cur_interval = 1u;
    int sub_ok = 1;

    for (size_t i = 0; i < (sizeof(vec) / sizeof(vec[0])); ++i) {
      uint8_t  m = 0u;
      uint16_t iv = 0u;
      char err[ENVSET_ERR_MAX];
      envset_result_t rc = envnode_sensorset_parse_n(vec[i].in, strlen(vec[i].in),
                                                     cur_mask, cur_interval,
                                                     &m, &iv, err, sizeof(err));
      int ok = (rc == vec[i].want);
      if (ok && rc != ENVSET_REJECTED) {
        ok = (m == vec[i].want_mask) && (iv == vec[i].want_interval);
      }
      if (!ok) {
        sub_ok = 0;
        snprintf(line, sizeof(line),
                 "  parse FAIL  \"%s\" -> rc=%d mask=0x%02X int=%u\r\n",
                 vec[i].in, (int)rc, (unsigned)m, (unsigned)iv);
        UART1_Send(line);
      }
    }
    if (sub_ok) { pass++; UART1_Send("  [PASS] config-string parser (13 vectors)\r\n"); }
    else        { fail++; UART1_Send("  [FAIL] config-string parser\r\n"); }
  }

  /* --- 2. payload packer -------------------------------------------------- */
  {
    sensor_readings_t r;
    uint8_t frame[ENVNODE_UPLINK_LEN];
    memset(&r, 0, sizeof(r));

    /* Known values chosen so every scaling rule is visible in the bytes:
       21.50 C -> 2150 (0x0866), 55.0 %RH -> 110 (0x6E), 1013.2 hPa -> 10132
       (0x2794), 3.700 V -> 3700 mV (0x0E74). */
    r.air1_temp_c = 21.50f; r.air1_rh_pct = 55.0f; r.air1_press_hpa = 1013.2f;
    r.air2_temp_c = -5.25f; r.air2_rh_pct = 80.0f; r.air2_press_hpa =  998.7f;
    r.batt_v      = 3.700f;
    r.status      = (uint8_t)(SENS_OK_AIR1 | SENS_OK_AIR2);

    size_t n = envnode_payload_pack(&r, frame, sizeof(frame));

    static const uint8_t expect[ENVNODE_UPLINK_LEN] = {
      0x01,                   /* fmt                                         */
      0x03,                   /* status: AIR1|AIR2 ok                        */
      0x74, 0x0E,             /* batt 3700 mV                                */
      0x66, 0x08,             /* air1 temp  2150 = 21.50 C                   */
      0x6E,                   /* air1 rh    110  = 55.0 %                    */
      0x94, 0x27,             /* air1 press 10132 = 1013.2 hPa               */
      0xF3, 0xFD,             /* air2 temp  -525 = -5.25 C                   */
      0xA0,                   /* air2 rh    160  = 80.0 %                    */
      0x03, 0x27,             /* air2 press 9987 = 998.7 hPa                 */
      0xFF, 0xFF,             /* soil  (not ok -> sentinel)                  */
      0xFF, 0xFF,             /* leaf  (not ok -> sentinel)                  */
      0xFF, 0x7F,             /* soil temp (not ok -> i16 sentinel)          */
      0xFF, 0xFF,             /* wind speed                                  */
      0xFF, 0xFF,             /* wind dir                                    */
      0xFF, 0xFF,             /* wind gust                                   */
      0xFF, 0xFF,             /* rain tips                                   */
      0xFF, 0xFF,             /* rain mm                                     */
    };

    int ok = (n == ENVNODE_UPLINK_LEN) && (memcmp(frame, expect, ENVNODE_UPLINK_LEN) == 0);

    /* Print the frame either way — on failure so the difference is visible, on
       success so it can be pasted into the TTN decoder tester. */
    char hex[2 * ENVNODE_UPLINK_LEN + 1];
    hexdump_into(hex, sizeof(hex), frame, ENVNODE_UPLINK_LEN);
    snprintf(line, sizeof(line), "  frame: %s\r\n", hex);
    UART1_Send(line);

    if (ok) { pass++; UART1_Send("  [PASS] payload packer (matches expected bytes)\r\n"); }
    else    { fail++; UART1_Send("  [FAIL] payload packer\r\n"); }
  }

  /* --- 3. I2C scan, both buses -------------------------------------------- */
  {
    struct { I2C_HandleTypeDef *bus; const char *name; } buses[2] = {
      { &hi2c2, "I2C2 (shield, T1)" },
      { &hi2c1, "I2C1 (board pins, T2)" },
    };
    int bme_seen = 0;

    for (int b = 0; b < 2; ++b) {
      char found[64]; found[0] = '\0';
      int  n_found = 0;
      for (uint8_t a = 0x08; a < 0x78; ++a) {
        if (HAL_I2C_IsDeviceReady(buses[b].bus, (uint16_t)(a << 1), 1, 5) == HAL_OK) {
          char one[8];
          snprintf(one, sizeof(one), "0x%02X ", a);
          if (strlen(found) + strlen(one) < sizeof(found)) strcat(found, one);
          n_found++;
          if (a == 0x76u || a == 0x77u) bme_seen++;
        }
      }
      snprintf(line, sizeof(line), "  %-22s : %s%s\r\n", buses[b].name,
               n_found ? found : "(nothing responded)",
               n_found ? "" : "  <-- check wiring / pull-ups");
      UART1_Send(line);
    }

    if (bme_seen >= 2) { pass++; UART1_Send("  [PASS] a BME280 address answered on each bus\r\n"); }
    else if (bme_seen == 1) { fail++; UART1_Send("  [FAIL] only one BME280 found - check the second bus\r\n"); }
    else { fail++; UART1_Send("  [FAIL] no BME280 (0x76/0x77) on either bus\r\n"); }
  }

  /* --- 4. live sensor read ------------------------------------------------ */
  {
    sensor_readings_t r;
    if (envnode_sensors_peek(&r) == ENV_OK &&
        (r.status & (SENS_OK_AIR1 | SENS_OK_AIR2)) == (SENS_OK_AIR1 | SENS_OK_AIR2)) {
      snprintf(line, sizeof(line),
               "  T1 %.2f C / %.1f %%RH / %.1f hPa   T2 %.2f C / %.1f %%RH / %.1f hPa\r\n",
               r.air1_temp_c, r.air1_rh_pct, r.air1_press_hpa,
               r.air2_temp_c, r.air2_rh_pct, r.air2_press_hpa);
      UART1_Send(line);
      pass++; UART1_Send("  [PASS] both BME280s returned a plausible reading\r\n");
    } else {
      fail++; UART1_Send("  [FAIL] live sensor read (see 'nucleo sensors' for detail)\r\n");
    }
  }

  /* --- 5. CONFIG.INI parser (pure function — no card needed) -------------- */
  {
    static const char good[] =
      "; EnviroNode credentials\r\n"
      "AppKey = 000102030405060708090A0B0C0D0E0F   ; comment\r\n"
      "deveui=0080E115061BF803\r\n"
      "# hash comment\r\n"
      "JOINEUI = 0000000000000001\r\n";
    static const char bad[] =
      "appkey = 000102030405060708090A0B0C0D0E0G\r\n"   /* bad hex digit  */
      "appkey = 00010203\r\n"                            /* too short      */
      "deveui = 0080E115061BF803FF extra\r\n";           /* trailing junk  */
    sdlog_creds_t c;
    int ok = 1;

    ok &= (envnode_ini_parse(good, sizeof(good) - 1u, &c) == 3);
    ok &= c.has_appkey && c.has_deveui && c.has_joineui;
    ok &= (c.app_key[0] == 0x00u && c.app_key[15] == 0x0Fu);
    ok &= (c.dev_eui[0] == 0x00u && c.dev_eui[7] == 0x03u);
    ok &= (c.join_eui[7] == 0x01u);
    ok &= (envnode_ini_parse(bad, sizeof(bad) - 1u, &c) == 0);
    ok &= !c.has_appkey;

    if (ok) { pass++; UART1_Send("  [PASS] CONFIG.INI parser (accept + 3 reject vectors)\r\n"); }
    else    { fail++; UART1_Send("  [FAIL] CONFIG.INI parser\r\n"); }
  }

  snprintf(line, sizeof(line), "---- selftest: %d passed, %d failed ----\r\n", pass, fail);
  UART1_Send(line);
}

/**
  * @brief  Stop the core until the next measurement is due, when that is safe.
  *
  * Called at the end of each main-loop pass. It deliberately refuses to sleep in
  * three situations, each of which would otherwise cost correctness:
  *
  *   - **Too soon after a transmission.** A Class A node's only chance to
  *     receive is the RX1/RX2 windows that follow its own uplink, and CM4 has to
  *     be awake afterwards to drain what CM0+ received. The awake window covers
  *     both, and doubles as the operator's chance to type a command.
  *   - **Edge-counted sensors selected.** Rain and wind-speed are counted in
  *     GPIO interrupts with millisecond timestamps (envnode_power.h).
  *   - **A command is already waiting**, which would otherwise sit unserviced
  *     for a whole interval.
  *
  * While the core is stopped the console is dead. That is stated on the console
  * before each nap, and `nucleo sleep off` disables sleeping for bench work.
  */
static void EnvNode_SleepTick(void)
{
  char line[96];

  if (!envnode_power_may_sleep()) return;
  if (cmd_ready) return;                       /* serve the operator first */

  const uint32_t now = HAL_GetTick();

  /* Stay awake long enough after a transmission for RX1/RX2 and the downlink
     drain that follows them. */
  if ((uint32_t)(now - g_last_tx_ms) < ENVNODE_POST_TX_AWAKE_MS) return;

  const int32_t remain_ms = (int32_t)(g_next_due_ms - now);
  if (remain_ms <= (int32_t)ENVNODE_SLEEP_GUARD_MS) return;   /* nearly due */

  const uint32_t sleep_s = (uint32_t)(remain_ms - (int32_t)ENVNODE_SLEEP_GUARD_MS) / 1000u;
  if (sleep_s < ENVNODE_SLEEP_MIN_S) return;

  snprintf(line, sizeof(line),
           "SLEEP: STOP2 for %lus (console idle until wake)\r\n",
           (unsigned long)sleep_s);
  UART1_Send(line);

  const uint32_t slept = envnode_power_sleep_seconds(sleep_s);

  snprintf(line, sizeof(line), "WAKE : after %lus (next uplink in %lums)\r\n",
           (unsigned long)slept,
           (unsigned long)((int32_t)(g_next_due_ms - HAL_GetTick()) > 0
                             ? (uint32_t)(g_next_due_ms - HAL_GetTick()) : 0u));
  UART1_Send(line);
}

/* Drain the radio core's downlink ring. A payload whose first byte is '{' is a
   sensor-set config string and is honoured on ANY FPort (docs/CONFIG.md);
   FPort-10 frames are the binary EnviroNode command table (docs/PAYLOAD.md);
   anything else is echoed as hex so the console still shows what the gateway
   sent rather than silently dropping it. */
static void EnvNode_DrainDownlinks(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  if (mb->magic != KORERO_MB_MAGIC) return;

  /* If CM0+ lapped us, skip to the newest slots rather than serving garbage. */
  if ((uint32_t)(mb->dl_head - mb->dl_tail) > KORERO_DL_SLOTS) {
    mb->dl_tail = mb->dl_head - KORERO_DL_SLOTS;
  }

  while (mb->dl_tail != mb->dl_head) {
    uint32_t i    = mb->dl_tail % KORERO_DL_SLOTS;
    uint8_t  len  = mb->dl_len[i];
    uint8_t  port = mb->dl_port[i];
    uint8_t  data[KORERO_DL_MAX];

    if (len > KORERO_DL_MAX) len = KORERO_DL_MAX;
    for (uint8_t k = 0; k < len; k++) data[k] = mb->dl_data[i][k];
    mb->dl_tail = mb->dl_tail + 1U;

    char line[96];
    if (len && data[0] == (uint8_t)ENVNODE_CFGSTR_MARKER) {
      /* Applied through the console helper rather than envnode_downlink_apply()
         so the operator gets the same echo (and the same rejection reason) as if
         they had typed it — a node reconfigured remotely must still be legible
         to whoever is standing next to it. */
      snprintf(line, sizeof(line), "DL: config string on FPort %u (%u bytes)\r\n",
               (unsigned)port, (unsigned)len);
      UART1_Send(line);
      EnvNode_ApplyConfigString((const char *)data, len);
    } else if (port == ENVNODE_DOWNLINK_FPORT) {
      int rc = envnode_downlink_apply(port, data, len);
      snprintf(line, sizeof(line), "DL: cmd 0x%02X (%u args) -> %s\r\n",
               (unsigned)(len ? data[0] : 0u), (unsigned)(len ? len - 1u : 0u),
               (rc == ENV_OK) ? "applied" :
               (rc == ENV_NOTIMPL) ? "not implemented" : "rejected");
      UART1_Send(line);
      /* 0x01 and 0x06 write the very fields the config string owns, so echo the
         canonical form after them too — one way of reading the configuration,
         however it was changed. */
      if (rc == ENV_OK && (data[0] == ENVNODE_CMD_SET_INTERVAL ||
                           data[0] == ENVNODE_CMD_SET_ENABLE)) {
        char setstr[ENVSET_STR_MAX];
        (void)envnode_sensorset_format(envnode_config_get_sensor_mask(),
                                       envnode_config_get_interval_min(),
                                       setstr, sizeof(setstr));
        snprintf(line, sizeof(line), "DL: config %s\r\n", setstr);
        UART1_Send(line);
      }
    } else {
      char hex[2 * 16 + 1];
      uint8_t n = (len > 16u) ? 16u : len;
      hexdump_into(hex, sizeof(hex), data, n);
      snprintf(line, sizeof(line), "DL: port %u, %u bytes: %s%s\r\n",
               (unsigned)port, (unsigned)len, hex, (len > n) ? "..." : "");
      UART1_Send(line);
    }
  }
}

/* Parse up to nbytes of hex from a string into out[], skipping any non-hex
   separators (spaces, colons, dashes). Returns the number of bytes parsed. */
static int parse_hex_bytes(const char *s, uint8_t *out, int nbytes)
{
  int n = 0, hi = -1;
  for (; *s && n < nbytes; s++)
  {
    int v;
    char c = *s;
    if      (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;                      /* skip separators */
    if (hi < 0) { hi = v; }
    else        { out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return n;
}

/* --- I2C utility with short timeouts --- */
static int i2c_read_reg(uint8_t reg, uint8_t *buf, uint32_t len) {
  const uint32_t TO = 10; // ms
  if (HAL_I2C_Master_Transmit(&hi2c2, INA219_ADDR, &reg, 1, TO) != HAL_OK) return 0;
  if (HAL_I2C_Master_Receive (&hi2c2, INA219_ADDR, buf, len, TO)   != HAL_OK) return 0;
  return 1;
}

/* --- Power stats --- */
static int ReadPowerStats(PowerStats_t *ps) {
  if (!ps) return 0;
  memset(ps, 0, sizeof(*ps));

  uint8_t data[2];
  uint8_t reg = INA_REG_BUS_V;
  if (i2c_read_reg(reg, data, 2)) {
    uint16_t bus_raw = ((uint16_t)data[0] << 8) | data[1];
    bus_raw >>= 3;
    ps->v_bus = bus_raw * 0.004f;
    ps->bus_ok = 1;

    reg = INA_REG_SHUNT_V;
    if (i2c_read_reg(reg, data, 2)) {
      int16_t shunt_raw = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
      ps->v_shunt = shunt_raw * 0.00001f;
      ps->current = ps->v_shunt / SHUNT_OHMS;
      ps->shunt_ok = 1;
    }
  }

  /* backup ADC path always available */
  ps->v_adc   = Battery_ReadVoltage(8);
  ps->v_src   = ps->bus_ok ? ps->v_bus : ps->v_adc;
  ps->use_ina = ps->bus_ok;

  if (ps->shunt_ok) ps->power_W = ps->v_src * ps->current;
  ps->soc_v = soc_from_voltage(ps->v_src);

  return (ps->bus_ok || ps->shunt_ok);
}

static void PH_MaybeRollHour(void){
#if defined(HAL_RTC_MODULE_ENABLED)
  RTC_TimeTypeDef t_bcd; RTC_DateTypeDef d_bcd;
  HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
  HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);
  uint8_t h = bcd2bin(t_bcd.Hours);
  if (g_ph_hourRTC == 0xFF) g_ph_hourRTC = h;
  if (h != g_ph_hourRTC) {
    g_ph_hourRTC = h;
    g_ph_head = (uint8_t)((g_ph_head + 1u) % PH_BINS);
    g_ph_wh[g_ph_head]   = 0.0f;
    g_ph_mAh[g_ph_head]  = 0.0f;
    g_ph_soc_i[g_ph_head]= 0.0f;
    g_ph_soc_v[g_ph_head]= 0.0f;
    g_ph_dt_h [g_ph_head]= 0.0f;
  }
#else
  uint32_t h = (HAL_GetTick() / 3600000u) % 24u;
  if (h != g_ph_uptimeHour) {
    g_ph_uptimeHour = h;
    g_ph_head = (uint8_t)((g_ph_head + 1u) % PH_BINS);
    g_ph_wh[g_ph_head]   = 0.0f;
    g_ph_mAh[g_ph_head]  = 0.0f;
    g_ph_soc_i[g_ph_head]= 0.0f;
    g_ph_soc_v[g_ph_head]= 0.0f;
    g_ph_dt_h [g_ph_head]= 0.0f;
  }
#endif
}


static void PH_TickIntegrate(float power_W, float current_A, float dt_s,
                             float soc_i_pct, float soc_v_pct)
{
  if (!g_ph_inited) {
#if defined(HAL_RTC_MODULE_ENABLED)
    RTC_TimeTypeDef t_bcd; RTC_DateTypeDef d_bcd;
    HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
    HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);
    g_ph_hourRTC = bcd2bin(t_bcd.Hours);
#else
    g_ph_uptimeHour = (HAL_GetTick() / 3600000u) % 24u;
#endif
    g_ph_inited = 1;
  }

  if (dt_s <= 0.0f) return;

  const float dt_h = dt_s / 3600.0f;

  /* Energy & charge */
  g_ph_wh[g_ph_head]  += (power_W   * dt_h);            /* Wh (signed) */
  g_ph_mAh[g_ph_head] += (current_A * dt_h * 1000.0f);  /* mAh (signed) */

  /* Time-weighted SoC averages */
  float prev_dt = g_ph_dt_h[g_ph_head];
  float new_dt  = prev_dt + dt_h;
  g_ph_dt_h[g_ph_head] = new_dt;
  if (new_dt > 0.0f) {
    g_ph_soc_i[g_ph_head] = (g_ph_soc_i[g_ph_head] * prev_dt + soc_i_pct * dt_h) / new_dt;
    g_ph_soc_v[g_ph_head] = (g_ph_soc_v[g_ph_head] * prev_dt + soc_v_pct * dt_h) / new_dt;
  }
}


static void PH_PrintHistory(void){
  char line[128];

  /* Wh row */
  UART1_Send("PH_WH,");
  for (uint32_t i = 0; i < PH_BINS; ++i) {
    uint32_t idx = (g_ph_head + 1u + i) % PH_BINS;
    int n = snprintf(line, sizeof(line), (i + 1 < PH_BINS) ? "%.6f," : "%.6f\r\n", (double)g_ph_wh[idx]);
    Console_Tx((uint8_t*)line, (uint16_t)n);
  }

  /* mAh row */
  UART1_Send("PH_mAh,");
  for (uint32_t i = 0; i < PH_BINS; ++i) {
    uint32_t idx = (g_ph_head + 1u + i) % PH_BINS;
    int n = snprintf(line, sizeof(line), (i + 1 < PH_BINS) ? "%.6f," : "%.6f\r\n", (double)g_ph_mAh[idx]);
    Console_Tx((uint8_t*)line, (uint16_t)n);
  }

  /* SoC_i (%) row */
  UART1_Send("PH_SoCi,");
  for (uint32_t i = 0; i < PH_BINS; ++i) {
    uint32_t idx = (g_ph_head + 1u + i) % PH_BINS;
    int n = snprintf(line, sizeof(line), (i + 1 < PH_BINS) ? "%.3f," : "%.3f\r\n", (double)g_ph_soc_i[idx]);
    Console_Tx((uint8_t*)line, (uint16_t)n);
  }

  /* SoC_v (%) row */
  UART1_Send("PH_SoCv,");
  for (uint32_t i = 0; i < PH_BINS; ++i) {
    uint32_t idx = (g_ph_head + 1u + i) % PH_BINS;
    int n = snprintf(line, sizeof(line), (i + 1 < PH_BINS) ? "%.3f," : "%.3f\r\n", (double)g_ph_soc_v[idx]);
    Console_Tx((uint8_t*)line, (uint16_t)n);
  }
}


static void PowerStats_Tick(void)
{
  static uint32_t last_ms = 0;
  if (last_ms == 0) last_ms = HAL_GetTick();

  PowerStats_t tmp;
  ReadPowerStats(&tmp);
  g_ps = tmp;

  /* FULL detection */
  uint32_t tnow = HAL_GetTick();

  /* End-of-charge (tail current + high voltage). Only this condition can trip
     the dwell timer and mark the pack FULL, because a merely-high terminal
     voltage under charge says nothing about how full the cells actually are. */
  const float CHARGE_FULL_V  = 14.2f;       // set to your charger absorb/cv voltage
  const float EOC_TAIL_A     = 0.15f;       // ~C/80 for 12 Ah pack ≈ 0.15 A

  bool end_of_charge =
      (tmp.shunt_ok) &&
      (tmp.v_src >= CHARGE_FULL_V) &&
      (fabsf(tmp.current) <= EOC_TAIL_A);

  if (end_of_charge) {
      if (!cond_active) { cond_active = 1; cond_start_ms = tnow; }
      if (!full_marked && (tnow - cond_start_ms) >= CHARGE_CONFIRM_MS) {
          BatteryFlow_Reset(0.0f);          // mark 100% right at the tail
          full_marked = 1;
      }
  } else {
      cond_active = 0;
  }


  /* Coulomb counting */
  uint32_t now = HAL_GetTick();
  float dt_s = (now - last_ms) / 1000.0f;
  if (dt_s <= 0.0f) dt_s = 0.001f;
  if (dt_s >  5.0f) dt_s = 5.0f;
  last_ms = now;

  if (tmp.shunt_ok) {
    BatteryFlow_Update(tmp.current, dt_s);
  }

  float used_mAh = BatteryFlow_Get_mAh();
  if (used_mAh < 0.0f) used_mAh = 0.0f;
  float soc_i = 100.0f * (1.0f - (used_mAh / BATTERY_NOMINAL_mAh));
  if (soc_i < 0.0f)   soc_i = 0.0f;
  if (soc_i > 100.0f) soc_i = 100.0f;

  g_used_mAh = used_mAh;
  g_soc_i    = soc_i;

  /* ---- Power history integration & hour roll ---- */
  PH_MaybeRollHour();
  if (tmp.shunt_ok) {
    PH_TickIntegrate(tmp.power_W, tmp.current, dt_s, soc_i, tmp.soc_v);
  }
}

/* One-line printer for on-demand snapshot */
static void PrintPowerSnapshot(void)
{
  PowerStats_t latest;
  if (ReadPowerStats(&latest)) {
    g_ps = latest;
  }

  PowerStats_t ps  = g_ps;
  float used_mAh   = g_used_mAh;
  float soc_i      = g_soc_i;

  const char *vsrc_tag = ps.use_ina ? "INA" : "ADC";
  char status_msg[256];

  if (ps.shunt_ok) {
    float pW = ps.v_src * ps.current;
    snprintf(status_msg, sizeof(status_msg),
      "Vsrc[%s]=%.3f V | I=%+.3f A | used=%5.0f/%5.0f mAh | "
      "SoC_i=%3.0f%% | SoC_v=%3.0f%% | %s\r\n",
      vsrc_tag, ps.v_src, ps.current, used_mAh,
      (float)BATTERY_NOMINAL_mAh, soc_i, ps.soc_v,
      full_marked ? "FULL_MARKED" : "—");
    UART1_Send(status_msg);

    char power_msg[96];
    snprintf(power_msg, sizeof(power_msg),
      "Power=%.3f W (%s)\r\n",
      pW, (pW >= 0.0f ? "discharge" : "charge"));
    UART1_Send(power_msg);
  } else {
    snprintf(status_msg, sizeof(status_msg),
      "Vsrc[%s]=%.3f V | I=NA (INA fail) | used=%5.0f/%5.0f mAh | "
      "SoC_i=%3.0f%% | SoC_v=%3.0f%% | %s\r\n",
      vsrc_tag, ps.v_src, used_mAh,
      (float)BATTERY_NOMINAL_mAh, soc_i, ps.soc_v,
      full_marked ? "FULL_MARKED" : "—");
    UART1_Send(status_msg);
  }
}

/* --- UART helpers --- */
static void UART1_Send(const char *s) {
  Console_Tx((const uint8_t*)s, (uint16_t)strlen(s));   /* USART1 + VCP */
}

static void normalize_cmd(const char *in, char *out, size_t out_sz) {
  size_t n = 0;
  for (const char *p = in; *p && n + 1 < out_sz; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c==' ' || c=='\t' || c==',' || c=='.' || c=='!' || c=='?' ||
        c==':' || c==';' || c=='\'' || c=='\"')
      continue;                 // skip whitespace & punctuation
    out[n++] = (char)tolower(c); // lowercase everything
  }
  out[n] = '\0';
}

/* ----------------------------------------------------------------------------
 * Status LED (external, PC2 / Arduino D8). Brief low-duty pulses encode state:
 *   Charging (solar) ............ 3 brief pulses every 7 s
 *   Idle / running .............. 1 brief pulse  every 10 s
 * Non-blocking; the LED is on only ~ON_MS per pulse => negligible power.
 * --------------------------------------------------------------------------*/
static void Status_Led_Tick(void)
{
  uint8_t  pulses;
  uint32_t period_ms;
  if (g_ps.shunt_ok && (g_ps.current <= STATUS_LED_CHARGE_A)) {   /* solar charging */
    pulses = 3; period_ms = 7000u;
  } else {                                                        /* idle / running */
    pulses = 1; period_ms = 10000u;
  }

  /* ---- emit `pulses` brief blinks at the start of each period, then off ---- */
  static uint32_t cycle_start = 0;
  static uint8_t  last_pulses = 0;
  static uint32_t last_period = 0;
  const uint32_t  ON_MS   = 30u;    /* very brief pulse                          */
  const uint32_t  SLOT_MS = 200u;   /* ON_MS + gap between pulses within a burst */

  uint32_t now = HAL_GetTick();
  if (cycle_start == 0) { cycle_start = now; }
  if (pulses != last_pulses || period_ms != last_period) {   /* state changed -> restart */
    last_pulses = pulses; last_period = period_ms; cycle_start = now;
  }
  uint32_t t = now - cycle_start;
  if (t >= period_ms) { cycle_start = now; t = 0; }

  uint8_t on = (uint8_t)((t < (uint32_t)pulses * SLOT_MS) && ((t % SLOT_MS) < ON_MS));
  HAL_GPIO_WritePin(STATUS_LED_Port, STATUS_LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* --- UART RX ISR: line assembly --- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart == &huart1) {
    uint8_t c = uart1_rx_byte;
    HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);  /* re-arm */

    if (c == '\r' || c == '\n') {
      if (uart1_len && !cmd_ready) {
        uart1_line[uart1_len] = '\0';
        strncpy(cmd_buf, uart1_line, sizeof(cmd_buf));
        cmd_buf[sizeof(cmd_buf)-1] = '\0';
        cmd_ready = 1;
      }
      uart1_len = 0;
      return;
    }
    if (uart1_len < sizeof(uart1_line) - 1) {
      uart1_line[uart1_len++] = (char)c;
    } else {
      uart1_len = 0;  // reset on overflow
    }
  }
  else if (huart == &huart2) {            /* ST-Link VCP console */
    uint8_t c = uart2_rx_byte;
    HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);  /* re-arm */

    if (c == '\r' || c == '\n') {
      if (uart2_len && !cmd_ready) {
        uart2_line[uart2_len] = '\0';
        strncpy(cmd_buf, uart2_line, sizeof(cmd_buf));
        cmd_buf[sizeof(cmd_buf)-1] = '\0';
        cmd_ready = 1;
      }
      uart2_len = 0;
      return;
    }
    if (uart2_len < sizeof(uart2_line) - 1) {
      uart2_line[uart2_len++] = (char)c;
    } else {
      uart2_len = 0;  // reset on overflow
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart == &huart2) {
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);
    return;
  }
  if (huart == &huart1) {
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
  }
}

/* --- Command handler --- */
/* Print every supported command and its syntax over UART, one line per send so
   each Console_Tx stays well under its timeout. Drives `nucleo list message
   syntax`. Keep in sync when adding/removing commands. */
static void Print_MessageSyntax(void)
{
  static const char *const lines[] = {
    "SYNTAX: EnviroNode-WL55 command reference (v" KORERO_FW_VERSION ")\r\n",
    "-- Node / sensors --\r\n",
    "info                   (identity, sensor set, sleep verdict, inventory)\r\n",
    "nucleo sensors         (sample every sensor and print it)\r\n",
    "nucleo uplink now      (sample + send the 30-byte frame on FPort 1)\r\n",
    "nucleo interval <min>  (uplink period, 1..999, persisted in flash)\r\n",
    "nucleo reset rain      (zero the rain-tip accumulator)\r\n",
    "nucleo selftest        (parser + packer + I2C scan + live read; no gateway)\r\n",
    "nucleo log             (offline log status: records used / capacity)\r\n",
    "nucleo log dump [n]    (CSV of every logged reading, newest first)\r\n",
    "nucleo log erase       (wipe the offline log)\r\n",
    "nucleo sd              (probe for an SD card; driver only, logging future)\r\n",
    "nucleo sleep on|off    (STOP2 between cycles; off keeps the console live)\r\n",
    "-- Sensor set: what to measure, how often (docs/CONFIG.md) --\r\n",
    "nucleo set {LW,T1,T2,SM,ST,WS,WD,R,15}   (replace the set + interval)\r\n",
    "{ALL,15} | {NONE}      (a bare brace line works too; ALL/NONE aliases)\r\n",
    "{+R} | {-LW,-WD}       (add/remove sensors, keep the rest)\r\n",
    "{5}                    (change only the interval, 1..999 min)\r\n",
    "{?}                    (print the current set; changes nothing)\r\n",
    "                       keys: LW leaf, T1/T2 air, SM soil moisture,\r\n",
    "                             ST soil temp, WS wind speed, WD wind dir,\r\n",
    "                             R rain.  One bad token rejects the whole line.\r\n",
    "-- Power / battery --\r\n",
    "nucleo power stats     (INA219 snapshot: V, I, SoC)\r\n",
    "nucleo power history   (last 25 h of Wh / mAh / SoC, CSV rows)\r\n",
    "nucleo send power      (uplink the battery snapshot)\r\n",
    "nucleo send power history  (uplink 24 h hourly SoC)\r\n",
    "nucleo i2c scan        (probe I2C2 for devices)\r\n",
    "-- Time / RTC --\r\n",
    "nucleo tell me time\r\n",
    "nucleo time is DD/MM/YYYY HH:MM:SS\r\n",
    "-- LoRaWAN keys / join --\r\n",
    "nucleo lorawan deveui <16 hex>\r\n",
    "nucleo lorawan appeui <16 hex>\r\n",
    "nucleo lorawan appkey <32 hex>\r\n",
    "nucleo lorawan join     (apply keys + (re)join, then persist them)\r\n",
    "nucleo lorawan forget   (clear persisted keys)\r\n",
    "nucleo lorawan status   (JOINED / NOJOIN)\r\n",
    "-- LoRaWAN data --\r\n",
    "nucleo, lorawan: <text> (raw uplink on the default port)\r\n",
    "-- Info / diagnostics --\r\n",
    "nucleo version\r\n",
    "nucleo deveui\r\n",
    "nucleo report          (boot/reset event log, timestamped)\r\n",
    "nucleo list message syntax\r\n",
    "END SYNTAX\r\n",
  };
  for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    UART1_Send(lines[i]);
  }
}

/**
  * @brief  Handle one complete console line from USART1 or the ST-Link VCP.
  * @param  line_in  the RAW line as received (case and punctuation preserved).
  *
  * Matching is done against a normalized copy (lowercase, punctuation removed)
  * so "Nucleo, sensors." and "nucleosensors" both work. Anything that needs the
  * original spelling — hex keys, the date/time string, a raw uplink payload,
  * the sensor-set config string — is re-parsed from @p line_in, never from the
  * normalized buffer.
  */
static void Console_HandleLine(const char *line_in) {
  char cmd[128];

  /* Sensor-set configuration string, FIRST so nothing can shadow it:
       nucleo set {T1,T2,15}     {ALL,15}     {+R}     {-LW,-WD}     {?}
     Any line carrying a '{' is one — braces have no other meaning here now that
     the inherited timetable is gone. It MUST be taken from the raw line:
     normalize_cmd() below strips ',' and '?', which turns "{T1,T2,15}" into
     "{t1t215}" and "{?}" into "{}" (docs/CONFIG.md, "Gotchas"). */
  {
    const char *brace = envnode_sensorset_find_brace(line_in);
    /* ...with one exception: "nucleo, lorawan: {...}" is a raw uplink payload
       the operator wants sent verbatim, braces and all. */
    const char *passthrough = strcasestr(line_in, "lorawan:");
    if (brace != NULL && (passthrough == NULL || brace < passthrough)) {
      EnvNode_ApplyConfigString(brace, strlen(brace));
      return;
    }
  }

  normalize_cmd(line_in, cmd, sizeof(cmd));

  /* Firmware version + full command list (first, so nothing else shadows them). */
  if (strstr(cmd, "nucleoversion")) {
    UART1_Send("VERSION: EnviroNode-WL55 firmware v" KORERO_FW_VERSION "\r\n");
    return;
  }
  /* Persistent event log dump: every reset cause with a timestamp. The primary
     tool for diagnosing "the node rebooted and I don't know why". */
  if (strstr(cmd, "nucleoreport")) {
    EvLog_Report();
    return;
  }
  /* Report the radio core's active DevEUI (+ AppEUI) for TTN registration.
     Matches "nucleo deveui" only — the write form "nucleo lorawan deveui <hex>"
     normalizes to "nucleolorawandeveui…" and is handled separately below. */
  if (strstr(cmd, "nucleodeveui")) {
    volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
    if (mb->magic == KORERO_MB_MAGIC && mb->deveui_ready) {
      char m[48];
      snprintf(m, sizeof(m), "DevEUI: %02X%02X%02X%02X%02X%02X%02X%02X\r\n",
               mb->dev_eui_now[0], mb->dev_eui_now[1], mb->dev_eui_now[2], mb->dev_eui_now[3],
               mb->dev_eui_now[4], mb->dev_eui_now[5], mb->dev_eui_now[6], mb->dev_eui_now[7]);
      UART1_Send(m);
      UART1_Send("AppEUI: 0000000000000000\r\n");
    } else {
      UART1_Send("DevEUI: (radio core not ready; retry in a second)\r\n");
    }
    return;
  }
  if (strstr(cmd, "nucleolistmessagesyntax")) {
    Print_MessageSyntax();
    return;
  }

  /* --- EnviroNode sensors ---------------------------------------------------
       info / nucleo info      -> identity (DevEUI + AppKey) + sensor inventory
       nucleo sensors          -> sample every sensor and print it
       nucleo uplink now       -> sample, pack the 30-byte frame, send on FPort 1
       nucleo reset rain       -> zero the rain-tip accumulator                */
  if (strcmp(cmd, "info") == 0 || strstr(cmd, "nucleoinfo")) {
    EnvNode_PrintInfo();
    return;
  }
  if (strstr(cmd, "nucleosensors") || strstr(cmd, "nucleoreadsensors")) {
    EnvNode_SampleAndPrint();
    return;
  }
  if (strstr(cmd, "nucleouplinknow") || strstr(cmd, "nucleosendsensors")) {
    EnvNode_UplinkNow();
    return;
  }
  if (strstr(cmd, "nucleoresetrain")) {
    pulse_reset_rain();
    UART1_Send("ACK: rain accumulator cleared\r\n");
    return;
  }
  /* SD card probe — the driver is programmed but nothing logs to SD yet
     (docs/LOGBOOK.md "SD-card mass logging"). Safe with no breakout wired:
     CMD0 times out and reports "no card" in ~100 ms. */
  if (strstr(cmd, "nucleosd")) {
    sd_info_t si;
    char lm[112];
    UART1_Send("ACK: probing SPI1 for an SD card (CS = D2/PB12)...\r\n");
    if (sd_spi_probe(&si)) {
      static const char *tname[] = { "none", "SD v1", "SD v2", "SDHC" };
      snprintf(lm, sizeof(lm), "SD: %s, %lu MB\r\n", tname[si.type], (unsigned long)si.capacity_mb);
      UART1_Send(lm);
      /* Hot insert: (re)mount, re-read CONFIG.INI, apply any credentials. */
      if (envnode_sdlog_init(&g_sd_creds)) {
        if (g_sd_creds.has_appkey) EnvNode_ApplySdCreds(&g_sd_creds);
      }
    } else {
      UART1_Send("SD: no card responded (no breakout, no card, or wiring - docs/LOGBOOK.md)\r\n");
    }
    envnode_sdlog_status(lm, sizeof(lm));
    UART1_Send(lm); UART1_Send("\r\n");
    return;
  }
  /* Offline sensor log. Ordering matters: "logdump"/"logerase" contain "log",
     so the bare status command is checked last. */
  if (strstr(cmd, "nucleologdump")) {
    const char *p = strstr(cmd, "nucleologdump") + strlen("nucleologdump");
    long want = strtol(p, NULL, 10);          /* 0 / absent = everything */
    EnvNode_LogDump((want > 0) ? (uint32_t)want : 0u);
    return;
  }
  if (strstr(cmd, "nucleologerase")) {
    UART1_Send(envnode_log_erase_all() ? "ACK: offline log erased\r\n"
                                       : "ERR: log erase failed\r\n");
    return;
  }
  if (strstr(cmd, "nucleolog")) {
    char lm[96];
    snprintf(lm, sizeof(lm),
             "LOG: %lu of %u records used (ring; oldest page recycles when full)\r\n",
             (unsigned long)envnode_log_count(), (unsigned)ENVLOG_CAPACITY);
    UART1_Send(lm);
    return;
  }
  /* Pre-flight check — proves the parser, the packer, both I2C buses and a live
     sensor read without needing a gateway. Run this first on a new node. */
  if (strstr(cmd, "nucleoselftest")) {
    EnvNode_SelfTest();
    return;
  }
  /* `nucleo sleep on|off` — off keeps the console continuously responsive for
     bench work, at the cost of the power saving. */
  if (strstr(cmd, "nucleosleep")) {
    char line[96];
    if (strstr(cmd, "nucleosleepoff")) {
      envnode_power_set_enabled(0);
      UART1_Send("ACK: sleep disabled - core stays awake (console always live)\r\n");
    } else if (strstr(cmd, "nucleosleepon")) {
      envnode_power_set_enabled(1);
      UART1_Send("ACK: sleep enabled\r\n");
    }
    /* Report the two causes separately: "disabled" and "blocked by the sensor
       set" are different problems, and conflating them sent the operator hunting
       for an edge-counted sensor that was never selected. */
    const char *why;
    if (!envnode_power_is_enabled())                      why = "off by command";
    else if (envnode_sensorset_requires_awake(envnode_config_get_sensor_mask()))
                                                          why = "blocked: R or WS is selected (edge-counted)";
    else                                                  why = "permitted by the sensor set";

    snprintf(line, sizeof(line), "SLEEP: %s, %s, %lus asleep since boot\r\n",
             envnode_power_is_enabled() ? "enabled" : "disabled", why,
             (unsigned long)envnode_power_total_asleep_s());
    UART1_Send(line);
    return;
  }
  /* Reached only when "nucleo set" carried no braces at all — the real handler
     ran before normalize_cmd(). Say what was expected instead of echoing. */
  if (strstr(cmd, "nucleoset")) {
    UART1_Send("ERR: use 'nucleo set {LW,T1,T2,SM,ST,WS,WD,R,15}' "
               "(or {?}, {ALL,15}, {+R}, {-LW}, {5}, {NONE})\r\n");
    return;
  }
  /* `nucleo interval <minutes>` — same setting as downlink command 0x01. */
  if (strstr(cmd, "nucleointerval")) {
    const char *p = strstr(cmd, "nucleointerval") + strlen("nucleointerval");
    long mins = strtol(p, NULL, 10);
    char line[80];
    if (mins >= (long)ENVCFG_INTERVAL_MIN_MIN && mins <= (long)ENVCFG_INTERVAL_MIN_MAX) {
      envnode_config_set_interval_min((uint16_t)mins);
      if (envnode_config_save()) {
        snprintf(line, sizeof(line), "ACK: uplink interval = %u min (saved)\r\n",
                 (unsigned)envnode_config_get_interval_min());
      } else {
        snprintf(line, sizeof(line), "ACK: uplink interval = %u min (NOT saved)\r\n",
                 (unsigned)envnode_config_get_interval_min());
      }
    } else {
      snprintf(line, sizeof(line), "ERR: interval must be %u..%u minutes (now %u)\r\n",
               (unsigned)ENVCFG_INTERVAL_MIN_MIN, (unsigned)ENVCFG_INTERVAL_MIN_MAX,
               (unsigned)envnode_config_get_interval_min());
    }
    UART1_Send(line);
    return;
  }

  /* Power snapshot */
  if (strstr(cmd, "nucleopowerstats")) {
    UART1_Send("ACK: power snapshot\r\n");
    PrintPowerSnapshot();
    return;
  }

  /* Power history dump */
  if (strstr(cmd, "nucleopowerhistory")) {
    UART1_Send("ACK: power history\r\n");
    PH_PrintHistory();
    return;
  }

  /* Time query: "nucleo tell me time" */
  if (strstr(cmd, "nucleotellmetime")) {
#if defined(HAL_RTC_MODULE_ENABLED)
    RTC_TimeTypeDef t_bcd;
    RTC_DateTypeDef d_bcd;
    HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
    HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD); /* must read Date after Time */

    uint8_t dd = bcd2bin(d_bcd.Date);
    uint8_t mm = bcd2bin(d_bcd.Month);
    uint8_t yy = bcd2bin(d_bcd.Year);
    uint8_t hh = bcd2bin(t_bcd.Hours);
    uint8_t mi = bcd2bin(t_bcd.Minutes);
    uint8_t ss = bcd2bin(t_bcd.Seconds);

    char msg[64];
    snprintf(msg, sizeof(msg), "%02u/%02u/%04u %02u:%02u:%02u\r\n",
             (unsigned)dd, (unsigned)mm, (unsigned)(2000u + yy),
             (unsigned)hh, (unsigned)mi, (unsigned)ss);
    UART1_Send(msg);
#else
    UART1_Send("ERR: RTC not enabled\r\n");
#endif
    return;
  }

  /* Time set: "nucleo time is DD/MM/YYYY HH:MM:SS" (4-digit; 2-digit accepted).
     Parsed from the RAW line — normalize_cmd strips the '/' and ':' separators. */
  {
    const char *k = "nucleo time is";
    const char *p = strcasestr(line_in, k);
    if (p) {
  #if defined(HAL_RTC_MODULE_ENABLED)
      p += strlen(k);
      while (*p==' ' || *p=='\t') p++;

      RTC_DateTypeDef d_bin; RTC_TimeTypeDef t_bin;
      if (parse_datetime_ddmmyyyy_hhmmss(p, &d_bin, &t_bin)) {
        /* Convert BIN -> BCD */
        RTC_DateTypeDef d_bcd = {0};
        RTC_TimeTypeDef t_bcd = {0};
        d_bcd.Date   = bin2bcd(d_bin.Date);
        d_bcd.Month  = bin2bcd(d_bin.Month);
        d_bcd.Year   = bin2bcd(d_bin.Year);
        /* The RTC rejects a zero weekday, and nothing here uses the day name. */
        d_bcd.WeekDay = RTC_WEEKDAY_MONDAY;

        t_bcd.Hours   = bin2bcd(t_bin.Hours);
        t_bcd.Minutes = bin2bcd(t_bin.Minutes);
        t_bcd.Seconds = bin2bcd(t_bin.Seconds);

        /* Recommended order: TIME then DATE */
        if (HAL_RTC_SetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD) == HAL_OK &&
            HAL_RTC_SetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD) == HAL_OK) {
          UART1_Send("ACK: time set\r\n");
        } else {
          UART1_Send("ERR: time set failed (weekday or format)\r\n");
        }
      } else {
        UART1_Send("ERR: bad time format, use DD/MM/YYYY HH:MM:SS\r\n");
      }
  #else
      UART1_Send("ERR: RTC not enabled\r\n");
  #endif
      return;
    }
  }

  /* I2C2 bus scan — confirm the INA219 (and any Grove I2C device) address. */
  if (strstr(cmd, "nucleoi2cscan")) {
    UART1_Send("ACK: I2C2 scan (7-bit addrs)...\r\n");
    uint8_t found = 0;
    for (uint8_t a = 1; a < 128; a++) {
      if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(a << 1), 2, 5) == HAL_OK) {
        char m[40];
        snprintf(m, sizeof(m), "  0x%02X\r\n", a);
        UART1_Send(m);
        found++;
      }
    }
    if (!found) UART1_Send("  (no devices responded)\r\n");
    UART1_Send("ACK: scan done\r\n");
    return;
  }

  /* --- Runtime OTAA key provisioning (relayed to CM0+ via the mailbox) ---
       nucleo lorawan deveui <16 hex>   (8 bytes, MSB first)
       nucleo lorawan appeui <16 hex>   (8 bytes; a.k.a. JoinEUI)
       nucleo lorawan appkey <32 hex>   (16 bytes)
       nucleo lorawan join              apply keys + (re)join TTN
     Note: "joineui" is checked before bare "join" (prefix). */
  if (strstr(cmd, "nucleolorawandeveui")) {
    const char *h = strcasestr(line_in, "deveui"); h = h ? h + 6 : line_in;
    uint8_t b[8];
    if (parse_hex_bytes(h, b, 8) == 8) {
      for (int i = 0; i < 8; i++) { KORERO_MAILBOX->dev_eui[i] = b[i]; }
      UART1_Send("ACK: DevEUI set\r\n");
    } else { UART1_Send("ERR: DevEUI needs 8 bytes (16 hex)\r\n"); }
    return;
  }
  if (strstr(cmd, "nucleolorawanappeui") || strstr(cmd, "nucleolorawanjoineui")) {
    const char *h = strcasestr(line_in, "appeui");
    if (h) { h += 6; } else { h = strcasestr(line_in, "joineui"); if (h) h += 7; }
    uint8_t b[8];
    if (h && parse_hex_bytes(h, b, 8) == 8) {
      for (int i = 0; i < 8; i++) { KORERO_MAILBOX->join_eui[i] = b[i]; }
      UART1_Send("ACK: JoinEUI/AppEUI set\r\n");
    } else { UART1_Send("ERR: JoinEUI needs 8 bytes (16 hex)\r\n"); }
    return;
  }
  if (strstr(cmd, "nucleolorawanappkey")) {
    const char *h = strcasestr(line_in, "appkey"); h = h ? h + 6 : line_in;
    uint8_t b[16];
    if (parse_hex_bytes(h, b, 16) == 16) {
      for (int i = 0; i < 16; i++) { KORERO_MAILBOX->app_key[i] = b[i]; }
      UART1_Send("ACK: AppKey set\r\n");
    } else { UART1_Send("ERR: AppKey needs 16 bytes (32 hex)\r\n"); }
    return;
  }
  /* Forget persisted keys -> revert to the compiled-in placeholder after reset. */
  if (strstr(cmd, "nucleolorawanforget")) {
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
    Persist_ForgetLoraKeys();
    /* Drop straight back to the compiled-in identity rather than making the
       operator power-cycle to see the effect. */
    if (Persist_LoadDefaultLoraKeys()) {
      UART1_Send("ACK: stored keys cleared; reverted to the compiled-in default "
                 "identity (envnode_identity.c) and rejoining\r\n");
    } else {
      UART1_Send("ACK: stored LoRaWAN keys cleared (no compiled-in default set)\r\n");
    }
#else
    UART1_Send("ERR: key persistence not available on this build\r\n");
#endif
    return;
  }
  if (strstr(cmd, "nucleolorawanjoin")) {
    volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
    __DMB();
    mb->key_seq = mb->key_seq + 1u;   /* tell CM0+ to apply keys + rejoin */
    UART1_Send("ACK: keys sent to radio core; (re)joining TTN...\r\n");
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
    Persist_SaveLoraKeys();           /* persist the just-provisioned identity */
#endif
    return;
  }

  /* Report whether the radio core has joined TTN (mailbox flag set by CM0+).
       nucleo lorawan status  ->  "STATUS: JOINED" | "STATUS: NOJOIN"
     Placed BEFORE the generic "nucleo, lorawan:" passthrough so it isn't eaten. */
  if (strstr(cmd, "nucleolorawanstatus")) {
    volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
    UART1_Send((mb->magic == KORERO_MB_MAGIC && mb->joined)
               ? "STATUS: JOINED\r\n" : "STATUS: NOJOIN\r\n");
    return;
  }

  /* Push battery data to TTN (uplink, fire-and-forget):
       nucleo send power history   -> last 24 h hourly SoC   (checked first)
       nucleo send power           -> compact battery snapshot
     ("nucleo lorawan power[ history]" is accepted too; checked BEFORE the
     generic "nucleo, lorawan:" passthrough below). */
  if (strstr(cmd, "nucleosendpowerhistory") || strstr(cmd, "nucleolorawanpowerhistory")) {
    Korero_LoraSendPowerHistory();
    UART1_Send("ACK: power history uplink queued (24 h hourly SoC)\r\n");
    return;
  }
  if (strstr(cmd, "nucleosendpower") || strstr(cmd, "nucleolorawanpower")) {
    Korero_LoraSendPower();
    UART1_Send("ACK: power uplink queued (battery snapshot)\r\n");
    return;
  }

  /* LoRaWAN passthrough: "nucleo, lorawan: <text>" -> CM0+ transmits <text>.
     Payload is taken from the RAW line (case/spaces preserved), not the
     normalized command. */
  if (strstr(cmd, "nucleolorawan")) {
    const char *p = strcasestr(line_in, "lorawan:");
    if (p) {
      p += strlen("lorawan:");
      while (*p == ' ' || *p == '\t') p++;          /* skip leading space */
      size_t n = strlen(p);
      while (n > 0 && (p[n-1]=='\r' || p[n-1]=='\n' || p[n-1]==' ' || p[n-1]=='\t')) {
        n--;                                          /* trim trailing ws/EOL */
      }
      Korero_LoraQueue(p, n);
      char ack[96];
      snprintf(ack, sizeof(ack), "ACK: LoRaWAN uplink queued (%u bytes) -> CM0+\r\n",
               (unsigned)((n > KORERO_MB_MAX_PAYLOAD) ? KORERO_MB_MAX_PAYLOAD : n));
      UART1_Send(ack);
    } else {
      UART1_Send("ERR: use 'nucleo, lorawan: <message>'\r\n");
    }
    return;
  }

  /* fallback: echo */
  UART1_Send("ACK: ");
  UART1_Send(line_in);
  UART1_Send("\r\n");
}

/* -------------------- RTC helpers (only if enabled) -------------------- */
#if defined(HAL_RTC_MODULE_ENABLED)
static int is_leap(int y) {
  return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}
static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

/* seconds since 2000-01-01 00:00:00 (not Unix epoch); expects BIN fields (yy=0..99) */
static uint32_t make_epoch2000(const RTC_DateTypeDef *d, const RTC_TimeTypeDef *t) {
  int yy = 2000 + d->Year;
  int mm = d->Month;
  int dd = d->Date;

  uint32_t days = 0;
  for (int y = 2000; y < yy; ++y) days += is_leap(y) ? 366 : 365;
  for (int m = 1; m < mm; ++m) {
    days += mdays[m-1];
    if (m == 2 && is_leap(yy)) days += 1;
  }
  days += (dd - 1);
  uint32_t secs = days*86400u + (uint32_t)t->Hours*3600u + (uint32_t)t->Minutes*60u + (uint32_t)t->Seconds;
  return secs;
}

/* Read now() in BCD, convert to BIN locally, then compute epoch */
static uint32_t rtc_now_epoch2000(void) {
  RTC_TimeTypeDef t_bcd; RTC_DateTypeDef d_bcd;
  HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
  HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);
  RTC_TimeTypeDef t_bin = {0};
  RTC_DateTypeDef d_bin = {0};
  d_bin.Year  = bcd2bin(d_bcd.Year);
  d_bin.Month = bcd2bin(d_bcd.Month);
  d_bin.Date  = bcd2bin(d_bcd.Date);
  t_bin.Hours   = bcd2bin(t_bcd.Hours);
  t_bin.Minutes = bcd2bin(t_bcd.Minutes);
  t_bin.Seconds = bcd2bin(t_bcd.Seconds);
  return make_epoch2000(&d_bin, &t_bin);
}

/* Parser that accepts DD/MM/YYYY HH:MM:SS; falls back to DD/MM/YY */
static uint8_t parse_datetime_ddmmyyyy_hhmmss(const char *p,
                                              RTC_DateTypeDef *d,
                                              RTC_TimeTypeDef *t)
{
  /* Accepts:
     - "DD/MM/YYYY HH:MM:SS"  (e.g., 18/10/2025 16:42:05)
     - "DD/MM/YY   HH:MM:SS"  (e.g., 18/10/25   16:42:05)  <-- still supported
  */
  unsigned int DD=0, MM=0, YYYY=0, YY=0, hh=0, mm=0, ss=0;

  /* Try 4-digit year first */
  int n = sscanf(p, " %2u/%2u/%4u %2u:%2u:%2u", &DD, &MM, &YYYY, &hh, &mm, &ss);
  if (n == 6) {
    if (!(DD>=1 && DD<=31 && MM>=1 && MM<=12 && hh<=23 && mm<=59 && ss<=59))
      return 0;

    /* Map YYYY to RTC 0..99 (offset from 2000) */
    if (YYYY < 2000 || YYYY > 2099) return 0; /* RTC holds 0..99 => 2000..2099 */
    unsigned int YYoff = YYYY - 2000;         /* 0..99 */

    d->Date  = (uint8_t)DD;
    d->Month = (uint8_t)MM;
    d->Year  = (uint8_t)YYoff;

    t->Hours          = (uint8_t)hh;
    t->Minutes        = (uint8_t)mm;
    t->Seconds        = (uint8_t)ss;
    t->SubSeconds     = 0;
    t->DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t->StoreOperation = RTC_STOREOPERATION_RESET;
    return 1;
  }

  /* Fallback: 2-digit year */
  n = sscanf(p, " %2u/%2u/%2u %2u:%2u:%2u", &DD, &MM, &YY, &hh, &mm, &ss);
  if (n != 6) return 0;
  if (!(DD>=1 && DD<=31 && MM>=1 && MM<=12 && hh<=23 && mm<=59 && ss<=59))
    return 0;

  YY %= 100;

  d->Date  = (uint8_t)DD;
  d->Month = (uint8_t)MM;
  d->Year  = (uint8_t)YY;

  t->Hours          = (uint8_t)hh;
  t->Minutes        = (uint8_t)mm;
  t->Seconds        = (uint8_t)ss;
  t->SubSeconds     = 0;
  t->DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  t->StoreOperation = RTC_STOREOPERATION_RESET;
  return 1;
}

/* -------- LoRaWAN OTAA key persistence in RTC backup registers ------------- */
#if LK_HAVE_PERSIST
/* Pack/unpack big-endian key bytes <-> consecutive 32-bit backup registers. */
static void lk_write_bytes(uint32_t reg0, const volatile uint8_t *b, int n) {
  for (int i = 0; i < n; i += 4) {
    uint32_t w = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                 ((uint32_t)b[i + 2] << 8) | (uint32_t)b[i + 3];
    HAL_RTCEx_BKUPWrite(&hrtc, reg0 + (uint32_t)(i / 4), w);
  }
}
static void lk_read_bytes(uint32_t reg0, volatile uint8_t *b, int n) {
  for (int i = 0; i < n; i += 4) {
    uint32_t w = HAL_RTCEx_BKUPRead(&hrtc, reg0 + (uint32_t)(i / 4));
    b[i] = (uint8_t)(w >> 24); b[i + 1] = (uint8_t)(w >> 16);
    b[i + 2] = (uint8_t)(w >> 8); b[i + 3] = (uint8_t)w;
  }
}

/* Snapshot the OTAA keys currently in the mailbox into the backup registers.
   Only persists when a non-zero AppKey is present. Writing backup registers is
   free — no erase, no wear — so this is safe to call on any boot path. */
static void Persist_SaveLoraKeysToBkp(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint8_t anz = 0; for (int i = 0; i < 16; i++) anz |= mb->app_key[i];
  if (!anz) return;                                   /* nothing real to store */
  uint8_t dnz = 0; for (int i = 0; i < 8; i++) dnz |= mb->dev_eui[i];
  uint8_t jnz = 0; for (int i = 0; i < 8; i++) jnz |= mb->join_eui[i];
  uint32_t flags = LK_FLAG_APPKEY;
  if (dnz) flags |= LK_FLAG_DEVEUI;
  if (jnz) flags |= LK_FLAG_JOINEUI;
  HAL_PWR_EnableBkUpAccess();
  lk_write_bytes(LK_REG_APPKEY0,  mb->app_key,  16);
  lk_write_bytes(LK_REG_DEVEUI0,  mb->dev_eui,   8);
  lk_write_bytes(LK_REG_JOINEUI0, mb->join_eui,  8);
  HAL_RTCEx_BKUPWrite(&hrtc, LK_REG_FLAGS, flags);
  HAL_RTCEx_BKUPWrite(&hrtc, LK_REG_MAGIC, LK_MAGIC);
}

/* Provisioning path: backup registers AND the flash mirror. The flash write
   costs a page erase, so it belongs only here — where the operator has just
   supplied a new identity — and never on a boot path. */
static void Persist_SaveLoraKeys(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint8_t anz = 0; for (int i = 0; i < 16; i++) anz |= mb->app_key[i];
  if (!anz) return;                                   /* nothing real to store */

  Persist_SaveLoraKeysToBkp();
  UART1_Send("INFO: LoRaWAN keys saved to backup registers (persist across reset)\r\n");

  /* Mirror into flash so the identity also survives a full power loss — backup
     registers only hold up while VBAT does. */
  Persist_SaveLoraKeysToFlash();
}

/* On boot: if a valid AppKey is stored, load the identity into the mailbox and
   trigger the radio core to apply + join. Returns 1 if restored. */
static int Persist_LoadLoraKeys(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  HAL_PWR_EnableBkUpAccess();
  if (HAL_RTCEx_BKUPRead(&hrtc, LK_REG_MAGIC) != LK_MAGIC) return 0;
  uint32_t flags = HAL_RTCEx_BKUPRead(&hrtc, LK_REG_FLAGS);
  if (!(flags & LK_FLAG_APPKEY)) return 0;
  if (flags & LK_FLAG_DEVEUI) { lk_read_bytes(LK_REG_DEVEUI0, mb->dev_eui, 8); }
  else { for (int i = 0; i < 8; i++) mb->dev_eui[i] = 0; }   /* 0 => keep chip DevEUI */
  if (flags & LK_FLAG_JOINEUI) { lk_read_bytes(LK_REG_JOINEUI0, mb->join_eui, 8); }
  else { for (int i = 0; i < 8; i++) mb->join_eui[i] = 0; }
  lk_read_bytes(LK_REG_APPKEY0, mb->app_key, 16);
  __DMB();
  mb->key_seq = mb->key_seq + 1u;        /* CM0+ applies keys + (re)joins */
  return 1;
}

/* Clear the stored keys -> the node reverts to its compiled-in placeholder on
   the next reset (factory reset for the OTAA identity). */
static void Persist_ForgetLoraKeys(void) {
  HAL_PWR_EnableBkUpAccess();
  HAL_RTCEx_BKUPWrite(&hrtc, LK_REG_MAGIC, 0);
  HAL_RTCEx_BKUPWrite(&hrtc, LK_REG_FLAGS, 0);
  (void)env_keystore_erase();            /* and drop the flash copy too */
}
#endif /* LK_HAVE_PERSIST */
#endif /* HAL_RTC_MODULE_ENABLED */

/* -------- LoRaWAN key persistence in internal flash ------------------------
   Backup registers vanish on a real power cut (VBAT rides VDD on this board),
   which would leave a deployed node unable to re-join until someone plugged in
   a console. The flash key store (Src/envnode_keystore.c, page 63) keeps the
   identity across power loss; the backup registers stay as the fast path.
   ------------------------------------------------------------------------- */
static void Persist_SaveLoraKeysToFlash(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint8_t app[16], dev[8], join[8];
  for (int i = 0; i < 16; i++) app[i]  = mb->app_key[i];
  for (int i = 0; i < 8;  i++) dev[i]  = mb->dev_eui[i];
  for (int i = 0; i < 8;  i++) join[i] = mb->join_eui[i];

  if (env_keystore_save(app, dev, join)) {
    UART1_Send("INFO: LoRaWAN keys saved to flash (survive power loss)\r\n");
  } else {
    UART1_Send("WARN: flash key store write failed\r\n");
  }
}

/* Last resort: push the compiled-in identity (envnode_identity.c) into the
   mailbox so the radio core joins with it. Nothing is persisted — a default is
   not a provisioned key, and writing it to flash would make "forget" useless.
   Returns 1 if a usable default existed. */
static int Persist_LoadDefaultLoraKeys(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;

  if (!envnode_identity_default_is_usable()) return 0;

  for (int i = 0; i < 8;  i++) mb->dev_eui[i]  = envnode_default_dev_eui[i];
  for (int i = 0; i < 8;  i++) mb->join_eui[i] = envnode_default_join_eui[i];
  for (int i = 0; i < 16; i++) mb->app_key[i]  = envnode_default_app_key[i];
  __DMB();
  mb->key_seq = mb->key_seq + 1u;        /* CM0+ applies keys + joins */
  return 1;
}

static int Persist_LoadLoraKeysFromFlash(void) {
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  uint8_t app[16], dev[8], join[8];
  uint32_t flags = 0;

  if (!env_keystore_load(app, dev, join, &flags)) return 0;

  for (int i = 0; i < 8;  i++) mb->dev_eui[i]  = dev[i];   /* 0 => chip DevEUI */
  for (int i = 0; i < 8;  i++) mb->join_eui[i] = join[i];
  for (int i = 0; i < 16; i++) mb->app_key[i]  = app[i];
  __DMB();
  mb->key_seq = mb->key_seq + 1u;        /* CM0+ applies keys + (re)joins */
  return 1;
}

/* Where the identity currently in the mailbox came from — shown by `info`. */
static const char *Persist_KeySourceName(void) {
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
  HAL_PWR_EnableBkUpAccess();
  int in_bkp = (HAL_RTCEx_BKUPRead(&hrtc, LK_REG_MAGIC) == LK_MAGIC) &&
               (HAL_RTCEx_BKUPRead(&hrtc, LK_REG_FLAGS) & LK_FLAG_APPKEY);
#else
  int in_bkp = 0;
#endif
  int in_flash = env_keystore_valid();

  if (in_bkp && in_flash) return "flash + backup registers (survives power loss)";
  if (in_flash)           return "flash (survives power loss)";
  if (in_bkp)             return "backup registers only (lost on power cut unless VBAT is battery-backed)";
  return envnode_identity_default_is_usable()
           ? "compiled-in default (envnode_identity.c) - not provisioned"
           : "none - radio core placeholder";
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  */
void Error_Handler(void)
{
  /* You can toggle an error LED here */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* Optional: report file/line */
}
#endif /* USE_FULL_ASSERT */
