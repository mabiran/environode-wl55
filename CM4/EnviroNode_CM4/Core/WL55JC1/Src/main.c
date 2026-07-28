/* USER CODE BEGIN Header */
/* =====================================================================
 *  EnviroNode-WL55  —  CM4 application core
 * ---------------------------------------------------------------------
 *  NOTE: This file is the INHERITED KoreroNet 2 application, kept as the
 *  starting base so the project builds and to reuse its infrastructure:
 *    - SystemClock / UART command server + normalize_cmd parser
 *    - RTC + epoch helpers, backup-register persistence
 *    - CM4<->CM0+ SRAM2 mailbox, OTAA key provisioning
 *    - event log ("report"), IWDG watchdog
 *  The acoustic / AudioMoth / Raspberry-Pi-power logic below is to be
 *  PROGRESSIVELY REPLACED with the EnviroNode sensor-sampling + LoRaWAN
 *  payload logic. See ../../docs/ROADMAP.md (Phase 3) and PAYLOAD.md.
 * ===================================================================== */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* CubeMX peripheral headers (provide extern handles + MX_*_Init decls) */
#include "adc.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"
#if defined(HAL_RTC_MODULE_ENABLED)
#include "rtc.h"
#endif

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdlib.h>   // strtof, strtod
#include <strings.h>  // strcasestr, strncasecmp
#include <ctype.h>
#include <stddef.h>   // size_t
#include <stdint.h>   // uint*_t
#include <stdio.h>    // snprintf, sscanf
#include "pins_config.h"
#include "battery_adc.h"
#include "battery_flow.h"
#include <stdbool.h>   // for bool, true, false
#include <math.h>      // if you use fabsf()
#include "korero_mailbox.h"   // shared CM4<->CM0+ LoRaWAN mailbox

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SOC_WAKE_THRESHOLD_PCT        20.0f
/* Minimum battery SoC (%) required to START a Process window (power the Pi). The
   Pi is the big load, so below this the Nucleo defers processing until solar
   brings the pack back up. Adjustable. */
#define PROC_MIN_SOC_PCT              5.0f
/* Minimum battery SoC (%) to run the power-on self-init (wake the Pi on a fresh
   power-on so it self-configures from the online config). Set ABOVE the LiFePO4
   knee so the init only fires with genuine SURPLUS charge — a chronically
   power-starved node that keeps dying/reviving on weak solar won't wake the Pi on
   a marginal battery, so it can't drain the pack in a loop. (The 5% PROC gate
   remains the floor for scheduled work.) Adjustable. */
#define INIT_MIN_SOC_PCT             20.0f

/* Wake pulse length (keep your existing define if present) */
#ifndef RPI_WAKE_PULSE_MS
#define RPI_WAKE_PULSE_MS             200u
#endif
#ifndef RPI_WAKE_DELAY_MS
#define RPI_WAKE_DELAY_MS             60000u     // 60 seconds
#endif

/* Restart notifier to help Pi re-init after MCU resets */
#define RESTART_BROADCAST_COUNT       20
#define RESTART_BROADCAST_PERIOD_MS   2000

#if defined(HAL_RTC_MODULE_ENABLED)
/* -------- Adaptive RTC BKP layout (STM32U083 has 9 regs: DR0..DR8) ---------
   Important: DR0 is RESERVED for CubeMX RTC "first-boot" magic (0x32F2).
   We never touch DR0 to avoid unintended RTC resets.
*/

/* Detect availability */
#if defined(RTC_BKP_NUMBER)
  #define BKP_COUNT RTC_BKP_NUMBER
#else
  #define BKP_COUNT 0
#endif

/* ---------------- Timetable persistence ------------------------------------
   We support two layouts:

   A) Rich layout (BKP_COUNT >= 9): uses DR1..DR7 (DR0 kept for RTC, DR8 spare)
      DR1: TT_MAGIC (0xAC1DDA7A)
      DR2: TT_FLAGS (bit0: valid)
      DR3: PACK0  (modes bits [31:0], 2 bits/hour)
      DR4: PACK1  (modes bits [47:32] in low 16b)
      DR5: REC_MS
      DR6: SLEEP_MS
      DR7: CRC32  over [PACK0|PACK1|REC_MS|SLEEP_MS]

   B) Compact layout (5 <= BKP_COUNT < 9): uses DR1..DR4 (no DR0 usage)
      DR1: TT_MAGIC (0xAC1DDA7A)
      DR2: PACK0
      DR3: PACK1 with CRC16 in high 16b
      DR4: DUR     (uint16_t REC_S | uint16_t SLEEP_S)  <-- seconds granularity

   If BKP_COUNT < 5, persistence is disabled.
---------------------------------------------------------------------------- */
#define TT_MAGIC                       0xAC1DDA7Au

#if BKP_COUNT >= 9
  #define TT_HAVE_RICH                 1
  #define TT_REG_MAGIC                 RTC_BKP_DR1
  #define TT_REG_FLAGS                 RTC_BKP_DR2
  #define TT_FLAG_VALID                (1u << 0)
  #define TT_REG_PACK0                 RTC_BKP_DR3
  #define TT_REG_PACK1                 RTC_BKP_DR4
  #define TT_REG_REC_MS                RTC_BKP_DR5
  #define TT_REG_SLEEP_MS              RTC_BKP_DR6
  #define TT_REG_CRC32                 RTC_BKP_DR7
#elif BKP_COUNT >= 5
  #define TT_HAVE_RICH                 0
  #define TT_REG_MAGIC                 RTC_BKP_DR1
  #define TT_REG_PACK0                 RTC_BKP_DR2
  #define TT_REG_PACK1                 RTC_BKP_DR3   /* high16 = CRC16, low16 = modes[47:32] */
  #define TT_REG_DUR_COMBO             RTC_BKP_DR4   /* low16 = rec_s, high16 = sleep_s */
#else
  #define TT_HAVE_RICH                 0
  /* No BKP persistence available */
#endif

/* ---------------- LoRaWAN key persistence ----------------------------------
   CM4 stores the OTAA identity (AppKey + optional DevEUI/JoinEUI overrides) in
   free backup registers DR9..DR18 and restores it to the radio core on boot, so
   the node re-joins on its own WITHOUT the Pi re-pushing keys. Backup registers
   survive a reset always, and a full power-off when VBAT is wired (SB21).
   Needs >= 19 backup registers (STM32WL55 has 20). DR1..DR7 = timetable. ------ */
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
      if (p < 0.0f) p = 0.0f; if (p > 100.0f) p = 100.0f;
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

/* Battery “full” tracking state */
static uint8_t  full_marked   = 0;
static uint8_t  cond_active   = 0;
static uint32_t cond_start_ms = 0;

/* RPi wake scheduler (runtime, ms-based) */
typedef enum { RPI_WAKE_IDLE=0, RPI_WAKE_SCHEDULED, RPI_WAKE_PULSING } rpi_wake_state_t;
static rpi_wake_state_t rpi_wake_state = RPI_WAKE_IDLE;
static uint32_t rpi_wake_due_ms = 0;
static uint32_t rpi_wake_pulse_end_ms = 0;

/* Pi 5V supply timed power-off ("nucleo turn me off <X>" -> cut power after X s).
   The Pi is powered only during Process (P) hours; on any other hour the scheduler
   arms a graceful off UNLESS a manual override (pi_manual_on) is active.
   pi_manual_on is a STICKY override raised by button B1 (or "nucleo pi power on"):
   while set, every automatic power-off is ignored (scheduler non-P hours,
   "processing completed", and the Pi's own "turn me off") so the Pi stays on
   until button B2 / "nucleo pi power off" or a board restart clears it. */
#define PI_SCHED_POWEROFF_GRACE_MS  90000u   /* let the Pi finish sudo poweroff  */
/* Hard MAX-ON backstop: cut the Pi 5V this long after a Process (P) window
   starts, even if the Pi never sends "processing completed"/"turn me off"
   (serial/upload hang, crash, etc.). The Pi's normal completion re-arms a much
   shorter grace, so this only bites when the Pi goes silent. Adjust freely. */
#define PI_PWR_MAX_ON_MIN           150u     /* minutes; change to taste */
#define PI_PWR_MAX_ON_MS            (PI_PWR_MAX_ON_MIN * 60u * 1000u)
static uint8_t  pi_pwr_off_armed = 0;
static uint32_t pi_pwr_off_due_ms = 0;
static uint8_t  pi_manual_on = 0;            /* 1 = Pi forced on (B1); until B2/restart */

/* --- Detection -> LoRaWAN relay (Pi sends detections line-by-line) ---
   Each detection packs to 6 bytes:  class(1) | time(4, LE uint32) | conf(1).
   DET_BATCH_MAX = 1 => pure line-by-line (1 detection per uplink, ACK after the
   send). Raise it to pack several detections per uplink (saves airtime) — but it
   must fit BOTH the 64-byte mailbox AND the LoRaWAN max payload for the current
   data rate (AU915: ~11 B at DR0/SF10 ... 242 B at DR3/SF7). */
/* Firmware version reported by `nucleo version`. */
#define KORERO_FW_VERSION "2.5"

#define DET_REC_BYTES   6
#define DET_BATCH_MAX   1
static uint8_t  det_batch[DET_BATCH_MAX * DET_REC_BYTES];
static uint8_t  det_count = 0;

/* Persistent wake (absolute, seconds since 2000-01-01) */
#if defined(HAL_RTC_MODULE_ENABLED)
static uint32_t wake_epoch = 0;
static uint8_t  wake_active = 0;
#endif

/* Restart notifier */
static uint8_t  restart_left = 0;
static uint32_t restart_next_ms = 0;

/* ------------------------------------------------------------------------
 * Persistent event log  (nucleo report)
 * A small ring of timestamped events: Pi 5V ON/OFF (with the reason) and the
 * cause of every (re)boot (brown-out / watchdog / software / pin). It is kept
 * in a dedicated .noinit RAM region the C startup never clears, so it survives
 * a warm reset (watchdog / software / NRST) and lets `nucleo report` show what
 * happened right up to the reset. Dumped on demand only; never auto-printed.
 * ---------------------------------------------------------------------- */
#define EVLOG_MAGIC   0x4B4E4C32u        /* 'KNL2' — bump if the layout changes */
#define EVLOG_SLOTS   32u

/* event types */
#define EV_BOOT        0u   /* generic boot (cause unknown)                    */
#define EV_RST_POR     1u   /* power-on / brown-out reset (BOR)                */
#define EV_RST_IWDG    2u   /* independent-watchdog reset  <-- the key one     */
#define EV_RST_WWDG    3u   /* window-watchdog reset                          */
#define EV_RST_SOFT    4u   /* software reset (NVIC_SystemReset)              */
#define EV_RST_PIN     5u   /* NRST pin reset                                 */
#define EV_RST_LPWR    6u   /* illegal low-power / option-byte reset          */
#define EV_PI_ON       7u   /* Pi 5V optocoupler switched ON                  */
#define EV_PI_OFF      8u   /* Pi 5V optocoupler switched OFF                 */

/* reason codes carried in .arg for EV_PI_ON / EV_PI_OFF */
#define PON_INIT       1u   /* power-on self-init                             */
#define PON_SCHED      2u   /* scheduler Process (P) window                   */
#define PON_BTN        3u   /* button B1 (manual override)                    */
#define PON_CMD        4u   /* "nucleo pi power on" command                   */
#define POFF_BTN       1u   /* button B2                                      */
#define POFF_CMD       2u   /* "nucleo pi power off" command                  */
#define POFF_TIMED     3u   /* timed / grace / max-on backstop expiry         */

typedef struct {
  uint32_t epoch;    /* rtc_now_epoch2000() at the moment of the event */
  uint32_t uptime;   /* HAL_GetTick() ms (fallback when the RTC is unset) */
  uint16_t seq;      /* monotonically increasing id                    */
  uint8_t  type;     /* EV_* event type                                */
  uint8_t  arg;      /* PON_ or POFF_ reason code, else 0              */
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
static void    Pi_SetPwr(uint8_t on, uint8_t reason);

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

/* -----------------------------------------------------------------------
 * Daily recording scheduler
 * --------------------------------------------------------------------- */
static char     g_day_schedule[24] = {0};
static uint32_t g_rec_ms = 0;             /* record duration in ms */
static uint32_t g_sleep_ms = 0;           /* sleep duration in ms */
static uint8_t  g_sched_enabled = 0;      /* 1 when a timetable has been set */
static uint8_t  g_sched_paused  = 0;      /* 1 if waiting on Pi processing */
static uint32_t g_sched_pause_deadline_ms = 0; /* resume by this time */
static uint8_t  g_sched_hour = 0;         /* current hour we are executing */
static uint8_t  g_sched_recording = 0;    /* 1 if currently recording */
static uint32_t g_sched_next_toggle_ms = 0; /* when to toggle record state */
static uint8_t  g_pi_serviced = 0;        /* 1 once the Pi got its Process (P)
                                             window this recording cycle. Cleared
                                             on the next S/U hour, a B1 press, or a
                                             board restart. Stops a completed P
                                             hour from re-powering the Pi right
                                             after it asks to power off. */

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
static void Korero_JoinTick(void);                          /* report join result -> Pi   */
static int  Korero_LoraSendBatch(void);                     /* blocking detection uplink  */
static void Korero_ServeDownlinks(void);                    /* gateway DL -> Pi + apply tt*/
static void Korero_LoraSendPower(void);                     /* uplink battery snapshot    */
static void Korero_LoraSendPowerHistory(void);              /* uplink 24h hourly SoC      */
static int  parse_hex_bytes(const char *s, uint8_t *out, int nbytes);
static void RPi_HandleLine(const char *line);
static void RPi_ScheduleWake(uint32_t delay_ms);
static void RPi_WakeTick(void);
static void Pi_PowerTick(void);                             /* timed Pi 5V power-off      */
static void Status_Led_Tick(void);                          /* external status LED (PC2/D8)*/
static void Pi_ButtonTick(void);                            /* B1/B2 -> Pi 5V power on/off */
static void Pi_ArmGraceOff(void);                           /* schedule Pi 5V off + grace  */
static void normalize_cmd(const char *in, char *out, size_t out_sz);
static void Print_MessageSyntax(void);                      /* `nucleo list message syntax`*/

/* Parse a daily timetable from a command string.  Returns 1 on success. */
static int  parse_timetable(const char *line_in);

/* Execute the recording schedule.  Called periodically from the main loop. */
static void scheduler_tick(void);

static int  ReadPowerStats(PowerStats_t *ps);
static void PrintPowerSnapshot(void);
static void PowerStats_Tick(void);
static int  parse_wake_hours(const char *line_in, float *out_hours, uint32_t *out_ms);

/* Power history helpers */
static void PH_AdvanceHour(void);
static void PH_TickIntegrate(float power_W, float current_A, float dt_s, float soc_i_pct, float soc_v_pct);
static void PH_MaybeRollHour(void);
static void PH_PrintHistory(void);

/* RTC helpers (compiled only if RTC enabled) */
#if defined(HAL_RTC_MODULE_ENABLED)
/* BCD helpers */
static inline uint8_t bcd2bin(uint8_t v){ return (uint8_t)((v>>4)*10 + (v&0x0F)); }
static inline uint8_t bin2bcd(uint8_t v){ return (uint8_t)(((v/10)<<4) | (v%10)); }

/* *** CHANGED: prototype now supports DD/MM/YYYY HH:MM:SS (and still DD/MM/YY) *** */
static uint8_t  parse_datetime_ddmmyyyy_hhmmss(const char *p, RTC_DateTypeDef *d, RTC_TimeTypeDef *t);
static int      is_leap(int y);                  /* y in full years, e.g., 2000 */
static uint32_t rtc_now_epoch2000(void);
static uint32_t make_epoch2000(const RTC_DateTypeDef *d, const RTC_TimeTypeDef *t);

/* Persistence via RTC backup: wake + timetable */
#if HAVE_PERSIST_WAKE
static void     Persist_SaveWakeEpoch(uint32_t epoch);
static int      Persist_LoadWakeEpoch(uint32_t *epoch);
static void     Persist_ClearWake(void);
#endif

/* LoRaWAN OTAA key persistence (backup registers; survives power-off w/ VBAT) */
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
static void     Persist_SaveLoraKeys(void);
static int      Persist_LoadLoraKeys(void);
static void     Persist_ForgetLoraKeys(void);
#endif

/* Timetable persistence */
static uint32_t TT_CalcCRC32(const uint8_t *data, size_t len);
static uint16_t TT_CalcCRC16(const uint8_t *data, size_t len);
static uint8_t  TT_ModeToBits(char c);
static char     TT_BitsToMode(uint8_t b);
static void     TT_SaveToBKP(const char modes[24], uint32_t rec_ms, uint32_t sleep_ms);
static int      TT_LoadFromBKP(char modes_out[24], uint32_t *rec_ms, uint32_t *sleep_ms);
static void     TT_PrintCurrent(void);
#endif

/* Restart notifier */
static void RestartNotifier_Tick(void);
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
  MX_I2C2_Init();
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

  /* Seed coulomb counter from the INA219 bus voltage on cold boot.
     (The analog ADC battery input PB1/A0 is no longer wired through the Base
     Shield, so we no longer read Battery_ReadVoltage() here — that pin would
     float and give a garbage seed. Battery telemetry comes from the INA219.) */
  HAL_Delay(10);                       /* let the INA219 finish a conversion */
  PowerStats_t ps_boot;
  ReadPowerStats(&ps_boot);
  float v_bat_boot = ps_boot.bus_ok ? ps_boot.v_bus : ps_boot.v_src;
  float soc_v_boot = soc_from_voltage(v_bat_boot);
  float used_mAh_guess = (1.0f - (soc_v_boot / 100.0f)) * BATTERY_NOMINAL_mAh;
  if (used_mAh_guess < 0.0f) used_mAh_guess = 0.0f;
  BatteryFlow_Reset(used_mAh_guess);

  /* Set default levels for control lines after MX_GPIO_Init() */
#ifdef RPI_WAKE_GPIO_Port
  HAL_GPIO_WritePin(RPI_WAKE_GPIO_Port, RPI_WAKE_Pin, GPIO_PIN_SET);
#endif
#ifdef AM_REC_Port
  HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
#ifdef AM_CONFIG_Port
  HAL_GPIO_WritePin(AM_CONFIG_Port, AM_CONFIG_Pin, GPIO_PIN_SET);
#endif

  /* Load persisted wake schedule (RTC only, if supported) */
#if defined(HAL_RTC_MODULE_ENABLED) && HAVE_PERSIST_WAKE
  if (Persist_LoadWakeEpoch(&wake_epoch)) {
    wake_active = 1;
  } else {
    wake_active = 0;
  }

  /* Load persisted timetable if present */
  {
    char modes[24];
    uint32_t r_ms = 0, s_ms = 0;
    if (TT_LoadFromBKP(modes, &r_ms, &s_ms)) {
      for (int i = 0; i < 24; ++i) g_day_schedule[i] = modes[i];
      g_rec_ms = r_ms;
      g_sleep_ms = s_ms;
      g_sched_enabled = 1;
      g_sched_paused  = 0;
      g_sched_recording = 0;
      g_sched_next_toggle_ms = 0;
      UART1_Send("INFO: timetable restored from backup\r\n");
    }
  }
#elif defined(HAL_RTC_MODULE_ENABLED)
  /* Even if persisted wake isn't available, we can still restore timetable if regs exist */
  {
    char modes[24];
    uint32_t r_ms = 0, s_ms = 0;
    if (TT_LoadFromBKP(modes, &r_ms, &s_ms)) {
      for (int i = 0; i < 24; ++i) g_day_schedule[i] = modes[i];
      g_rec_ms = r_ms;
      g_sleep_ms = s_ms;
      g_sched_enabled = 1;
      g_sched_paused  = 0;
      g_sched_recording = 0;
      g_sched_next_toggle_ms = 0;
      UART1_Send("INFO: timetable restored from backup\r\n");
    }
  }
#endif

  /* Restart notifier */
  restart_left = RESTART_BROADCAST_COUNT;
  restart_next_ms = HAL_GetTick() + 500;  // first ping shortly after boot

  UART1_Send("BOOT: Nucleo ready\r\n");

  /* Prepare the shared LoRaWAN mailbox, THEN boot the Cortex-M0+ radio core
     (CPU2). It runs the LoRaWAN stack from flash @ 0x08020000 and polls the
     mailbox for uplink commands. Safe even if CM0+ holds only the skeleton. */
  Korero_MailboxInit();
  HAL_PWREx_ReleaseCore(PWR_CORE_CPU2);
  UART1_Send("BOOT: CM0+ (radio core) released\r\n");

  /* Restore any LoRaWAN keys persisted in backup registers and push them to the
     radio core, so the node re-joins on its own after a reset/power-cycle. */
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
  if (Persist_LoadLoraKeys()) {
    UART1_Send("BOOT: restored LoRaWAN keys from backup; radio (re)joining\r\n");
  }
#endif
  /* USER CODE END BSP */

  /* Infinite loop */
  while (1)
  {
    /* One-time bring-up on the first loop pass: power-on self-init + watchdog. */
    static uint8_t g_boot_once = 0;
    if (!g_boot_once) {
      g_boot_once = 1;

      /* Persistent event log: validate the retained ring, count this boot, and
         record WHY we just (re)booted -- brown-out / watchdog / software / pin --
         with a timestamp. Read the RCC reset flags here, before they are cleared
         below (and before the self-init consumes BORRST). */
      EvLog_Init();
      g_evlog.boots++;
      EvLog_Add(EvLog_ResetCause(), 0u);

      /* Power-on self-init: on a true power-on (BOR reset), wake the Pi ONCE so it
         self-configures from the online config -- no B1 press needed after a node
         is handed over. Skipped on watchdog/software/NRST resets (BORRST clear)
         and on a near-empty pack. The Pi provisions the board + reports internet,
         then asks to power off; normal scheduling resumes. */
      if ((__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != 0u) && !pi_manual_on) {
        if (soc_v_boot >= INIT_MIN_SOC_PCT) {
          Pi_SetPwr(1u, PON_INIT);
          g_pi_serviced             = 1;   /* don't double-power at the first P hour */
          g_sched_paused            = 1;   /* hold the schedule while the Pi runs    */
          g_sched_pause_deadline_ms = HAL_GetTick() + 5u * 3600000u;
          pi_pwr_off_due_ms         = HAL_GetTick() + PI_PWR_MAX_ON_MS;
          pi_pwr_off_armed          = 1;
#ifdef RPI_WAKE_GPIO_Port
          rpi_wake_state  = RPI_WAKE_SCHEDULED;
          rpi_wake_due_ms = HAL_GetTick();
#endif
          UART1_Send("EVENT: power-on init -> Pi 5V ON (self-config from online)\r\n");
        } else {
          UART1_Send("EVENT: power-on init skipped -- battery below 20%\r\n");
        }
      }
      __HAL_RCC_CLEAR_RESET_FLAGS();

      /* Independent Watchdog: hardware auto-reset if the firmware ever hangs.
         Refreshed each loop pass below; a hang or a fault-handler while(1) stops
         the refresh, so the chip hard-resets after ~15 s and reboots (re-reads the
         timetable from BKP; PI_PWR defaults LOW so a stuck Pi is power-cycled too).
         Frozen while a debugger is halted so it can't fight flashing. */
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
    Korero_JoinTick();                  /* surface CM0+ join result to the Pi (USART1) */

    /* RPi wake scheduler tick (handles both runtime and persisted wake) */
    RPi_WakeTick();

    /* Single silent call that samples INA/ADC, handles FULL detection, and updates coulomb counter */
    PowerStats_Tick();

    /* Drive the hourly recording scheduler */
    scheduler_tick();

    /* Process any received command (handled outside ISR) */
    if (cmd_ready) {
      cmd_ready = 0;
      RPi_HandleLine(cmd_buf);
    }

    /* Restart notifier (periodic) */
    RestartNotifier_Tick();

    /* Timed Pi 5V power-off */
    Pi_PowerTick();

    /* Board buttons: B1 -> Pi 5V power ON, B2 -> OFF (via CM0+ mailbox) */
    Pi_ButtonTick();

    /* External status LED (brief, low-duty state indicator on PC2 / D8) */
    Status_Led_Tick();

    /* Small pacing slice to keep ISR latency low */
    uint32_t until = HAL_GetTick() + 50;
    while ((int32_t)(HAL_GetTick() - until) < 0) {
      RPi_WakeTick();
      if (cmd_ready) {
        cmd_ready = 0;
        RPi_HandleLine(cmd_buf);
      }
      /* Invoke scheduler during pacing loop */
      scheduler_tick();
      /* Keep the VCP fed with the radio core's trace */
      Korero_DrainTrace();
      /* Status LED — called here too so pulses stay crisp (~5 ms resolution) */
      Status_Led_Tick();
      HAL_Delay(5);  /* Does NOT disable UART interrupts — only adds latency */
    }

    /* Heartbeat blink (BSP LED) */
    //BSP_LED_Toggle(LED_GREEN);
    //HAL_Delay(250);
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

/* ---- KoreroNet: CM4 <-> CM0+ LoRaWAN mailbox ----------------------------- */
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
  mb->pi_pwr_seq = 0;              /* button -> Pi power request channel          */
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

/* Drive the Pi 5V optocoupler (PC1) and log only real ON<->OFF edges, so the
   report shows genuine transitions (not every idempotent re-assert). This is
   the single choke-point every Pi power change now goes through. */
static void Pi_SetPwr(uint8_t on, uint8_t reason)
{
  GPIO_PinState want = on ? GPIO_PIN_SET : GPIO_PIN_RESET;
  if (HAL_GPIO_ReadPin(PI_PWR_Port, PI_PWR_Pin) != want) {
    HAL_GPIO_WritePin(PI_PWR_Port, PI_PWR_Pin, want);
    EvLog_Add(on ? EV_PI_ON : EV_PI_OFF, reason);
  }
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

/* Dump the whole ring, oldest -> newest, to the Pi on USART1. */
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
           "REPORT: KoreroNet 2 event log v" KORERO_FW_VERSION
           "  now=%s  boots=%lu  events=%u\r\n",
           whenbuf, (unsigned long)g_evlog.boots, (unsigned)g_evlog.count);
  UART1_Send(line);
  UART1_Send("REPORT:  #  seq  when                 up(s)   event\r\n");

  uint16_t n   = g_evlog.count;
  uint16_t idx = (uint16_t)((g_evlog.head + EVLOG_SLOTS - n) % EVLOG_SLOTS); /* oldest */
  for (uint16_t i = 0; i < n; i++) {
    EvEntry_t *e = &g_evlog.e[idx];
    EvLog_FmtTime(e->epoch, whenbuf, sizeof(whenbuf));
    char evtxt[40];
    if (e->type == EV_PI_ON) {
      const char *r = (e->arg == PON_INIT)  ? "power-on self-init" :
                      (e->arg == PON_SCHED) ? "scheduler P window" :
                      (e->arg == PON_BTN)   ? "button B1"          :
                      (e->arg == PON_CMD)   ? "pi-power-on cmd"    : "?";
      snprintf(evtxt, sizeof(evtxt), "Pi 5V ON  (%s)", r);
    } else if (e->type == EV_PI_OFF) {
      const char *r = (e->arg == POFF_BTN)   ? "button B2"            :
                      (e->arg == POFF_CMD)   ? "pi-power-off cmd"     :
                      (e->arg == POFF_TIMED) ? "timed/grace/backstop" : "?";
      snprintf(evtxt, sizeof(evtxt), "Pi 5V OFF (%s)", r);
    } else {
      uint8_t t = (e->type <= EV_RST_LPWR) ? e->type : 0u;
      snprintf(evtxt, sizeof(evtxt), "%s", RSTNAME[t]);
    }
    snprintf(line, sizeof(line), "REPORT: %2u  %3u  %s  %6lu  %s\r\n",
             (unsigned)(i + 1u), (unsigned)e->seq, whenbuf,
             (unsigned long)(e->uptime / 1000u), evtxt);
    UART1_Send(line);
    idx = (uint16_t)((idx + 1u) % EVLOG_SLOTS);
  }
  UART1_Send("REPORT: end\r\n");
}

/* Report the CM0+ radio's LoRaWAN join result to the Pi on USART1. The radio
   core only logs "= JOINED =" to its trace ring (drained to the VCP), so the
   Pi's wait_joined() never saw it and timed out, skipping every uplink. Here CM4
   watches the mailbox `joined` flag and emits a one-line "JOINED"/"JOIN FAILED"
   to the Pi (and VCP) on each change, so the Pi can proceed to send detections. */
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

/* Write a buffer to BOTH consoles: USART1 (pin header / Pi) and USART2 (VCP). */
static void Console_Tx(const uint8_t *d, uint16_t n)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)d, n, 50);
  HAL_UART_Transmit(&huart2, (uint8_t *)d, n, 50);
}

/* Drain the CM0+ -> CM4 trace ring and print it on the VCP (USB) only, so the
   radio core's LoRaWAN logs are visible on the ST-Link serial without polluting
   the Pi's command stream on USART1. */
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

/* Send the current detection batch to CM0+ for a LoRaWAN uplink, blocking until
   CM0+ reports the result (or a ~12 s timeout). Returns 1 if the radio core
   accepted/sent it (batch cleared), 0 otherwise (batch kept for retry). */
static int Korero_LoraSendBatch(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  if (det_count == 0) return 1;

  uint8_t len = (uint8_t)(det_count * DET_REC_BYTES);
  for (uint8_t i = 0; i < len; i++) { mb->payload[i] = det_batch[i]; }
  mb->len    = len;
  mb->port   = 0;                       /* CM0+ default app port */
  mb->status = KORERO_ST_IDLE;
  uint32_t want = mb->req_seq + 1u;
  __DMB();
  mb->req_seq = want;                   /* commit -> CM0+ transmits on next poll */

  uint32_t t0 = HAL_GetTick();
  while (mb->ack_seq != want) {
    if ((HAL_GetTick() - t0) > 12000u) { return 0; }   /* timeout */
    IWDG->KR = 0x0000AAAAu;             /* feed the watchdog: this bounded (<=12s)
                                           wait must never be mistaken for a hang */
    Korero_DrainTrace();                /* keep the VCP fed while we wait */
  }
  if (mb->status == KORERO_ST_SENT) { det_count = 0; return 1; }
  return 0;                             /* not joined / busy -> keep batch */
}

/* Serve any gateway downlinks the radio core stored, to the Pi, and apply a
   timetable to our OWN scheduler. Emits "DL: <text>" per stored message, then
   "ACK: downlink end". Called from `nucleo get downlink`. The gateway is
   expected to send config/timetable as ASCII (e.g. "{...24...},{rec,sleep}"). */
static void Korero_ServeDownlinks(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  if (mb->magic != KORERO_MB_MAGIC) { UART1_Send("ACK: downlink end\r\n"); return; }

  /* If CM0+ lapped us (more than the ring's worth arrived), skip to the newest. */
  if ((uint32_t)(mb->dl_head - mb->dl_tail) > KORERO_DL_SLOTS) {
    mb->dl_tail = mb->dl_head - KORERO_DL_SLOTS;
  }

  while (mb->dl_tail != mb->dl_head) {
    uint32_t i   = mb->dl_tail % KORERO_DL_SLOTS;
    uint8_t  len = mb->dl_len[i];
    if (len > KORERO_DL_MAX) { len = KORERO_DL_MAX; }

    char text[KORERO_DL_MAX + 1];
    for (uint8_t k = 0; k < len; k++) {
      uint8_t c = mb->dl_data[i][k];
      text[k] = (c >= 32 && c < 127) ? (char)c : '.';   /* printable only */
    }
    text[len] = '\0';

    UART1_Send("DL: ");
    UART1_Send(text);
    UART1_Send("\r\n");

    /* If the downlink carries a timetable, apply it to our scheduler too. */
    if (strchr(text, '{') && parse_timetable(text)) {
      UART1_Send("INFO: timetable from gateway applied\r\n");
    }

    mb->dl_tail = mb->dl_tail + 1U;
  }
  UART1_Send("ACK: downlink end\r\n");
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
      if (s < 0) s = 0; if (s > 100) s = 100;
      b[2 + j] = (uint8_t)s;
    }
  }
  Korero_LoraQueue((const char *)b, sizeof(b));
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
  /* --- Replace the FULL detection block --- */
  uint32_t tnow = HAL_GetTick();

  /* Stage 1: are we actually charging? */
  const float CHARGE_PRESENT_V = 13.5f;     // LiFePO4 4S under charge
  const float CHARGE_PRESENT_A = 0.20f;     // ~0.2 A charging present threshold

  bool charging_present =
      (tmp.shunt_ok) &&
      (tmp.v_src >= CHARGE_PRESENT_V) &&
      (tmp.current < -CHARGE_PRESENT_A);    // negative = charging

  /* Stage 2: end-of-charge (tail current + high voltage) */
  const float CHARGE_FULL_V  = 14.2f;       // set to your charger absorb/cv voltage
  const float EOC_TAIL_A     = 0.15f;       // ~C/80 for 12 Ah pack ≈ 0.15 A

  bool end_of_charge =
      (tmp.shunt_ok) &&
      (tmp.v_src >= CHARGE_FULL_V) &&
      (fabsf(tmp.current) <= EOC_TAIL_A);

  /* Only this condition can trip the dwell timer and mark FULL */
  bool meets = end_of_charge;
  if (meets) {
      if (!cond_active) { cond_active = 1; cond_start_ms = tnow; }
      if (!full_marked && (tnow - cond_start_ms) >= CHARGE_CONFIRM_MS) {
          BatteryFlow_Reset(0.0f);          // mark 100% right at the tail
          full_marked = 1;
          /* ... keep your EVENT: FULL_MARKED print ... */
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

/* --- Parse a daily timetable from an incoming command --- */
static int parse_timetable(const char *line_in)
{
  if (!line_in) return 0;
  /* Find first and second brace pairs */
  const char *p1 = strchr(line_in, '{');
  if (!p1) return 0;
  const char *p2 = strchr(p1 + 1, '}');
  if (!p2) return 0;
  /* Parse the first list: up to 24 mode characters */
  char modes[24];
  memset(modes, '0', sizeof(modes));
  unsigned idx = 0;
  const char *s = p1 + 1;
  while (s < p2 && idx < 24) {
    /* Skip whitespace */
    while (s < p2 && (*s == ' ' || *s == '\t')) s++;
    if (s >= p2) break;
    char c = *s;
    char upc;
    /* Accept valid characters */
    if (c == 'S' || c == 's') upc = 'S';
    else if (c == 'U' || c == 'u') upc = 'U';
    else if (c == 'P' || c == 'p') upc = 'P';
    else if (c == '0' || c == 'o' || c == 'O') upc = '0';
    else {
      /* Unrecognised token: treat as zero */
      upc = '0';
    }
    modes[idx++] = upc;
    /* Move to next comma or end */
    while (s < p2 && *s != ',') s++;
    if (s < p2 && *s == ',') s++;
  }
  /* Fill remaining hours with '0' */
  for (; idx < 24; ++idx) modes[idx] = '0';

  /* Parse the second list: record and sleep durations */
  const char *p3 = strchr(p2 + 1, '{');
  if (!p3) return 0;
  const char *p4 = strchr(p3 + 1, '}');
  if (!p4) return 0;
  char buf[64];
  int len = (int)(p4 - (p3 + 1));
  if (len <= 0 || len >= (int)sizeof(buf)) return 0;
  strncpy(buf, p3 + 1, (size_t)len);
  buf[len] = '\0';
  /* Extract two floating point numbers */
  char *endptr;
  double d1 = strtod(buf, &endptr);
  if (endptr == buf) return 0;
  while (*endptr == ' ' || *endptr == '\t' || *endptr == ',') endptr++;
  double d2 = strtod(endptr, NULL);
  if (d1 <= 0.0 || d2 <= 0.0) return 0;
  /* Convert seconds to milliseconds */
  g_rec_ms   = (uint32_t)(d1 * 1000.0 + 0.5);
  g_sleep_ms = (uint32_t)(d2 * 1000.0 + 0.5);
  if (g_rec_ms < 100) g_rec_ms = 100;
  if (g_sleep_ms < 100) g_sleep_ms = 100;

  /* Commit the new schedule */
  for (int i = 0; i < 24; ++i) g_day_schedule[i] = modes[i];
  g_sched_enabled       = 1;
  g_sched_paused        = 0;
  g_sched_recording     = 0;
  g_sched_next_toggle_ms = 0;

#if defined(HAL_RTC_MODULE_ENABLED)
  /* Persist it if backup regs exist */
  TT_SaveToBKP(g_day_schedule, g_rec_ms, g_sleep_ms);
#endif
  return 1;
}

/* --- Scheduler tick: drives hour-by-hour recording behaviour --- */
static void scheduler_tick(void)
{

  if (!g_sched_enabled) {
#if defined(HAL_RTC_MODULE_ENABLED)
    /* Plan B: wake Raspberry Pi at 11:00 local time once per day */
    static uint8_t _last_planb_day = 0xFF;
    RTC_TimeTypeDef t_bcd; RTC_DateTypeDef d_bcd;
    HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
    HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);
    uint8_t hour = bcd2bin(t_bcd.Hours);
    uint8_t day  = bcd2bin(d_bcd.Date);
    if (hour == 11 && _last_planb_day != day) {
#ifdef RPI_WAKE_GPIO_Port
      UART1_Send("PLANB: no timetable; waking Pi at 11:00\r\n");
      RPi_ScheduleWake(0);
#endif
      _last_planb_day = day;
    }
#endif
    return;
  }

  uint32_t now_ms = HAL_GetTick();
  /* If paused waiting for Pi */
  if (g_sched_paused) {
    /* Resume if timeout reached */
    if ((int32_t)(now_ms - g_sched_pause_deadline_ms) >= 0) {
      g_sched_paused = 0;
      UART1_Send("WARN: schedule timeout elapsed, resuming\r\n");
    } else {
      return;
    }
  }

  /* Determine the current hour.  Prefer RTC if available, otherwise use uptime. */
#if defined(HAL_RTC_MODULE_ENABLED)
  RTC_TimeTypeDef t_bcd;
  RTC_DateTypeDef d_bcd;
  HAL_RTC_GetTime(&hrtc, &t_bcd, RTC_FORMAT_BCD);
  HAL_RTC_GetDate(&hrtc, &d_bcd, RTC_FORMAT_BCD);
  uint8_t hour = bcd2bin(t_bcd.Hours);
#else
  uint8_t hour = (uint8_t)((HAL_GetTick() / 3600000u) % 24u);
#endif

  /* On hour change initialise recording state */
  if (g_sched_next_toggle_ms == 0 || hour != g_sched_hour) {
    g_sched_hour = hour;
    char mode = g_day_schedule[hour];
    /* Normalise to uppercase */
    if (mode >= 'a' && mode <= 'z') {
      mode = (char)(mode - ('a' - 'A'));
    }

    /* Pi is powered only during Process (P) hours: on any other hour, arm a
       graceful power-off of the Pi 5V if it's still on (runs only when NOT paused,
       i.e. not mid-processing, since scheduler_tick returns early while paused). */
    if (mode != 'P') { Pi_ArmGraceOff(); }

    /* Configure ultrasound based on mode */
    if (mode == 'S') {
#ifdef AM_CONFIG_Port
      HAL_GPIO_WritePin(AM_CONFIG_Port, AM_CONFIG_Pin, GPIO_PIN_SET);
#endif
    } else if (mode == 'U') {
#ifdef AM_CONFIG_Port
      HAL_GPIO_WritePin(AM_CONFIG_Port, AM_CONFIG_Pin, GPIO_PIN_RESET);
#endif
    }

    /* Standby: ensure AM_REC is high and no toggling */
    if (mode == '0') {
#ifdef AM_REC_Port
      HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
      g_sched_recording = 0;
      g_sched_next_toggle_ms = 0;
      return;
    }
    /* Process: stop recording, then power + wake the Pi ONCE per recording cycle. */
    if (mode == 'P') {
#ifdef AM_REC_Port
      HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
      /* Battery gate: the Pi is the big load, so do NOT start a Process window
         unless the pack is at/above PROC_MIN_SOC_PCT. g_soc_i is refreshed by
         PowerStats_Tick() right before this each loop, so the check costs no
         extra I2C. While deferred we keep re-entering this hour (toggle=0) so the
         Pi powers as soon as solar lifts the battery back over the threshold. A
         B1 manual override (pi_manual_on) bypasses the gate. */
      if (!pi_manual_on && !g_pi_serviced) {
        static uint8_t proc_deferred = 0;
        if (g_soc_i < PROC_MIN_SOC_PCT) {
          if (!proc_deferred) {
            char m[96];
            snprintf(m, sizeof(m),
                     "EVENT: Process deferred -- battery %d%% < %d%% (waiting for charge)\r\n",
                     (int)(g_soc_i + 0.5f), (int)PROC_MIN_SOC_PCT);
            UART1_Send(m);
            proc_deferred = 1;
          }
          g_sched_next_toggle_ms = 0;   /* re-enter this P hour to re-check */
          return;
        }
        proc_deferred = 0;              /* battery ok -> fall through and power Pi */
      }
      /* Set a non-zero toggle time so, after the Pi finishes and the schedule
         resumes, this same P hour is NOT re-entered and re-triggered. */
      g_sched_next_toggle_ms = now_ms;
      if (g_pi_serviced) {
        /* The Pi already had its Process window this cycle. Do NOT power it again
           just because we're still in a P hour (that's what caused the on/off
           loop): once the Pi asks to power off, keep it off until the next S/U
           recording hour, a B1 press, or a board restart. */
        UART1_Send("EVENT: Process hour already served; Pi stays off\r\n");
        return;
      }
      g_pi_serviced = 1;
      /* Turn the Pi 5V supply ON for the Process window (opto, PC1). */
      Pi_SetPwr(1u, PON_SCHED);
      /* Arm the hard max-on backstop (PI_PWR_MAX_ON_MIN) so a Pi that never asks
         to be powered off can't drain the battery for the full 5 h pause. The
         Pi's "processing completed"/"turn me off" re-arms a much shorter grace.
         Skipped while a B1 manual override (pi_manual_on) keeps the Pi on. */
      if (pi_manual_on) {
        pi_pwr_off_armed = 0;
      } else {
        pi_pwr_off_due_ms = now_ms + PI_PWR_MAX_ON_MS;
        pi_pwr_off_armed  = 1;
      }
      UART1_Send("EVENT: Process hour -> Pi 5V power ON\r\n");
#ifdef RPI_WAKE_GPIO_Port
      /* Schedule an immediate wake pulse */
      rpi_wake_state = RPI_WAKE_SCHEDULED;
      rpi_wake_due_ms = now_ms;
#endif
      /* Pause scheduler and set timeout for 5 hours */
      g_sched_paused = 1;
      g_sched_pause_deadline_ms = now_ms + 5U * 3600000U;
      UART1_Send("EVENT: schedule paused for processing\r\n");
      return;
    }
    /* For S or U: start with recording low */
    if (mode == 'S' || mode == 'U') {
      g_pi_serviced = 0;   /* new recording cycle -> allow the next P to serve */
      g_sched_recording = 1;
#ifdef AM_REC_Port
      HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_RESET);
#endif
      g_sched_next_toggle_ms = now_ms + g_rec_ms;
      return;
    }
    /* Unknown mode: treat as standby */
    g_sched_recording = 0;
    g_sched_next_toggle_ms = 0;
    return;
  }

  /* Within the hour: handle toggling for S/U modes */
  char mode = g_day_schedule[g_sched_hour];
  if (mode >= 'a' && mode <= 'z') {
    mode = (char)(mode - ('a' - 'A'));
  }
  if (mode == 'S' || mode == 'U') {
    if ((int32_t)(now_ms - g_sched_next_toggle_ms) >= 0) {
      g_sched_recording = (uint8_t)!g_sched_recording;
      if (g_sched_recording) {
        /* Begin recording: AM_REC low */
#ifdef AM_REC_Port
        HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_RESET);
#endif
        g_sched_next_toggle_ms = now_ms + g_rec_ms;
      } else {
        /* Sleep: AM_REC high */
#ifdef AM_REC_Port
        HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
        g_sched_next_toggle_ms = now_ms + g_sleep_ms;
      }
    }
  } else {
    /* Any other within-hour mode (a P hour already served, or an unknown slot):
       keep AM_REC high and recording off, but do NOT reset the hour-change
       trigger to 0. Zeroing it makes the hour-change block re-enter every tick,
       which for a served P hour spammed "already served" and re-triggered. Leave
       it non-zero so we only re-enter when the hour actually changes. */
#ifdef AM_REC_Port
    HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
    g_sched_recording = 0;
    if (g_sched_next_toggle_ms == 0) { g_sched_next_toggle_ms = now_ms; }
  }
}

/* Internet-status LED override on D8, set by "nucleo internet good/bad".
   0 = off (show the normal state indicator), 1 = GOOD pattern, 2 = BAD pattern.
   Active for 60 s from the command, then reverts to normal. */
static volatile uint8_t  g_net_led_mode     = 0;
static volatile uint32_t g_net_led_start_ms = 0;
static volatile uint32_t g_net_led_until_ms = 0;

/* ----------------------------------------------------------------------------
 * Status LED (external, PC2 / Arduino D8). Brief low-duty pulses encode state:
 *   Processing (P window) ....... 3 brief pulses every 1 s
 *   Recording  (S or U hour) .... 1 brief pulse  every 3 s
 *   Charging   (solar)  ......... 3 brief pulses every 7 s
 *   Idle / standby .............. 1 brief pulse  every 10 s
 * Priority: processing > recording > charging > idle (reorder the if/else to
 * change it). Non-blocking; the LED is on only ~ON_MS per pulse => minimal power.
 * --------------------------------------------------------------------------*/
static void Status_Led_Tick(void)
{
  uint32_t now = HAL_GetTick();

  /* Internet-status override: "nucleo internet good/bad" shows a 60 s pattern on
     the D8 LED, then falls through to the normal state indicator below.
       GOOD -> 5 s on, 0.5 s off, repeating (period 5.5 s)
       BAD  -> ~10 Hz strobe (deliberately unlike the 3-pulse processing blink) */
  if (g_net_led_mode) {
    if ((int32_t)(now - g_net_led_until_ms) >= 0) {
      g_net_led_mode = 0;                        /* 60 s elapsed -> back to normal */
    } else {
      uint32_t el = now - g_net_led_start_ms;
      uint8_t  on = (g_net_led_mode == 1u)
                  ? (uint8_t)((el % 5500u) < 5000u)   /* GOOD: 5 s on / 0.5 s off */
                  : (uint8_t)((el / 50u) & 1u);        /* BAD:  ~10 Hz strobe      */
      HAL_GPIO_WritePin(STATUS_LED_Port, STATUS_LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
      return;
    }
  }

  /* ---- pick the pattern for the current system state ---- */
  char mode = '0';
  if (g_sched_enabled) {
    mode = g_day_schedule[g_sched_hour];
    if (mode >= 'a' && mode <= 'z') mode = (char)(mode - ('a' - 'A'));
  }
  uint8_t  pulses;
  uint32_t period_ms;
  if (g_sched_paused) {                                          /* Pi processing (P) */
    pulses = 3; period_ms = 1000u;
  } else if (mode == 'S' || mode == 'U') {                       /* recording hour    */
    pulses = 1; period_ms = 3000u;
  } else if (g_ps.shunt_ok && (g_ps.current <= STATUS_LED_CHARGE_A)) { /* solar charging */
    pulses = 3; period_ms = 7000u;
  } else {                                                       /* idle / standby    */
    pulses = 1; period_ms = 10000u;
  }

  /* ---- emit `pulses` brief blinks at the start of each period, then off ---- */
  static uint32_t cycle_start = 0;
  static uint8_t  last_pulses = 0;
  static uint32_t last_period = 0;
  const uint32_t  ON_MS   = 30u;    /* very brief pulse                          */
  const uint32_t  SLOT_MS = 200u;   /* ON_MS + gap between pulses within a burst */

  now = HAL_GetTick();
  if (cycle_start == 0) { cycle_start = now; }
  if (pulses != last_pulses || period_ms != last_period) {   /* state changed -> restart */
    last_pulses = pulses; last_period = period_ms; cycle_start = now;
  }
  uint32_t t = now - cycle_start;
  if (t >= period_ms) { cycle_start = now; t = 0; }

  uint8_t on = (uint8_t)((t < (uint32_t)pulses * SLOT_MS) && ((t % SLOT_MS) < ON_MS));
  HAL_GPIO_WritePin(STATUS_LED_Port, STATUS_LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Apply a Pi 5V power request raised by a board button (B1=on, B2=off) on the
   CM0+ core and relayed through the mailbox. Polled from the main loop. */
static void Pi_ButtonTick(void)
{
  volatile KoreroMailbox_t *mb = KORERO_MAILBOX;
  static uint32_t last_seq = 0;
  if (mb->magic != KORERO_MB_MAGIC) { return; }
  if (mb->pi_pwr_seq == last_seq)   { return; }
  last_seq = mb->pi_pwr_seq;
  if (mb->pi_pwr_on) {
    Pi_SetPwr(1u, PON_BTN);
    pi_pwr_off_armed = 0;                 /* cancel any pending timed-off */
    pi_manual_on = 1;                     /* sticky override: on until B2 or restart */
    g_pi_serviced = 0;                    /* B1 press -> let the scheduler serve again */
    UART1_Send("EVENT: button B1 -> Pi 5V ON (manual override until B2/restart)\r\n");
  } else {
    Pi_SetPwr(0u, POFF_BTN);
    pi_pwr_off_armed = 0;
    pi_manual_on = 0;
    UART1_Send("EVENT: button B2 -> Pi 5V power OFF\r\n");
  }
}

/* Schedule the Pi 5V supply to switch OFF after a grace period, so the Pi can
   finish a clean shutdown first. No-op if the Pi is already off or was turned on
   by hand (manual override). Used by the scheduler on non-Process hours and after
   "processing completed". */
static void Pi_ArmGraceOff(void)
{
  if (pi_manual_on) { return; }
  if (HAL_GPIO_ReadPin(PI_PWR_Port, PI_PWR_Pin) != GPIO_PIN_SET) { return; }  /* already off */
  pi_pwr_off_due_ms = HAL_GetTick() + PI_SCHED_POWEROFF_GRACE_MS;
  pi_pwr_off_armed  = 1;
}


/* --- Parse "wake me up in <fp> h/min" --- */
static int parse_wake_hours(const char *line_in, float *out_hours, uint32_t *out_ms)
{
  if (!line_in || !out_hours || !out_ms) return 0;

  const char *key = "wake me up in";
  const char *q = strcasestr(line_in, key);
  if (!q) return 0;

  q += strlen(key);
  while (*q && !( (*q>='0'&&*q<='9') || *q=='-' || *q=='+' || *q=='.' )) q++;

  char *endp = NULL;
  double val = strtod(q, &endp);
  while (*endp == ' ') endp++;

  int unit_is_hours = 1;  /* default hours */
  if (*endp) {
    if      (!strncasecmp(endp, "h",     1)) unit_is_hours = 1;
    else if (!strncasecmp(endp, "hr",    2)) unit_is_hours = 1;
    else if (!strncasecmp(endp, "hrs",   3)) unit_is_hours = 1;
    else if (!strncasecmp(endp, "hour",  4)) unit_is_hours = 1;
    else if (!strncasecmp(endp, "hours", 5)) unit_is_hours = 1;
    else if (!strncasecmp(endp, "m",     1)  ||
             !strncasecmp(endp, "min",   3)  ||
             !strncasecmp(endp, "mins",  4)  ||
             !strncasecmp(endp, "minute",6)  ||
             !strncasecmp(endp, "minutes",7)) {
      unit_is_hours = 0;
    }
  }

  if (val <= 0.0) return 0;

  double msd = unit_is_hours ? (val * 3600000.0) : (val * 60000.0);
  if (msd > 4.29e9) msd = 4.29e9;           // clamp to ~49 days
  uint32_t delay_ms = (uint32_t)(msd + 0.5);

  *out_hours = unit_is_hours ? (float)val : (float)(val / 60.0);
  *out_ms = delay_ms;
  return 1;
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
    "SYNTAX: KoreroNet 2 command reference (v" KORERO_FW_VERSION ")\r\n",
    "-- AudioMoth --\r\n",
    "nucleo start recording\r\n",
    "nucleo i need files\r\n",
    "nucleo i need ultrasound\r\n",
    "nucleo i do not need ultrasound\r\n",
    "-- Power / battery --\r\n",
    "nucleo power stats\r\n",
    "nucleo power history\r\n",
    "nucleo send power\r\n",
    "nucleo send power history\r\n",
    "nucleo i2c scan\r\n",
    "-- Status LED (D8) --\r\n",
    "nucleo internet good   (LED steady-blink 60s)\r\n",
    "nucleo internet bad    (LED rapid-blink 60s)\r\n",
    "-- Time / RTC --\r\n",
    "nucleo tell me time\r\n",
    "nucleo time is DD/MM/YYYY HH:MM:SS\r\n",
    "-- Schedule --\r\n",
    "nucleo timetable {m0,..,m23}{rec_s,sleep_s}\r\n",
    "nucleo give me timetable\r\n",
    "nucleo print timetable\r\n",
    "nucleo processing completed\r\n",
    "-- Pi wake / power --\r\n",
    "wake me up in <X> h|min\r\n",
    "goodnight\r\n",
    "nucleo pi power on\r\n",
    "nucleo pi power off\r\n",
    "nucleo turn me off <X>\r\n",
    "-- LoRaWAN keys / join --\r\n",
    "nucleo lorawan deveui <16 hex>\r\n",
    "nucleo lorawan appeui <16 hex>\r\n",
    "nucleo lorawan appkey <32 hex>\r\n",
    "nucleo lorawan join\r\n",
    "nucleo lorawan forget   (clear persisted keys)\r\n",
    "-- LoRaWAN data --\r\n",
    "nucleo det <class>,<time>,<conf>\r\n",
    "nucleo det flush\r\n",
    "nucleo, lorawan: <text>\r\n",
    "nucleo get downlink\r\n",
    "-- Info / diagnostics --\r\n",
    "nucleo version\r\n",
    "nucleo deveui\r\n",
    "nucleo report   (Pi power + reset event log, timestamped)\r\n",
    "nucleo list message syntax\r\n",
    "END SYNTAX\r\n",
  };
  for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    UART1_Send(lines[i]);
  }
}

static void RPi_HandleLine(const char *line_in) {
  char cmd[128];
  normalize_cmd(line_in, cmd, sizeof(cmd));

  /* Firmware version + full command list (first, so nothing else shadows them). */
  if (strstr(cmd, "nucleoversion")) {
    UART1_Send("VERSION: KoreroNet 2 firmware v" KORERO_FW_VERSION "\r\n");
    return;
  }
  /* Persistent event log dump: Pi power ON/OFF (+reason) and every reset cause,
     each with a timestamp. The primary tool for diagnosing "the Pi went off". */
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

  /* Internet-status indicator on the D8 LED (the Pi reports its connectivity):
       nucleo internet good  -> 5 s on / 0.5 s off, repeating for 60 s
       nucleo internet bad   -> ~10 Hz strobe for 60 s (distinct from processing)
     After 60 s the LED reverts to the normal state indicator. */
  if (strstr(cmd, "nucleointernetgood") || strstr(cmd, "nucleointernetbad")) {
    uint8_t good = strstr(cmd, "nucleointernetgood") ? 1u : 0u;
    g_net_led_start_ms = HAL_GetTick();
    g_net_led_until_ms = g_net_led_start_ms + 60000u;
    g_net_led_mode     = good ? 1u : 2u;
    UART1_Send(good ? "ACK: internet good -- LED steady-blink 60s\r\n"
                    : "ACK: internet bad -- LED rapid-blink 60s\r\n");
    return;
  }

  /* --- AudioMoth controls --- */
  if (strstr(cmd, "nucleostartrecording")) {
#ifdef AM_REC_Port
    HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_RESET);
#endif
    UART1_Send("ACK: AM_REC -> LOW (Start Recording)\r\n");
    return;
  }
  if (strstr(cmd, "nucleoineedfiles")) {
#ifdef AM_REC_Port
    HAL_GPIO_WritePin(AM_REC_Port, AM_REC_Pin, GPIO_PIN_SET);
#endif
    UART1_Send("ACK: AM_REC -> HIGH (Need files)\r\n");
    return;
  }
  if (strstr(cmd, "nucleoidonotneedultrasound")) {
#ifdef AM_CONFIG_Port
    HAL_GPIO_WritePin(AM_CONFIG_Port, AM_CONFIG_Pin, GPIO_PIN_SET);
#endif
    UART1_Send("ACK: AM_CONFIG -> HIGH (No ultrasound)\r\n");
    return;
  }
  if (strstr(cmd, "nucleoineedultrasound")) {
#ifdef AM_CONFIG_Port
    HAL_GPIO_WritePin(AM_CONFIG_Port, AM_CONFIG_Pin, GPIO_PIN_RESET);
#endif
    UART1_Send("ACK: AM_CONFIG -> LOW (Need ultrasound)\r\n");
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

  /* Print timetable saved in BKP or runtime */
  if (strstr(cmd, "nucleogivemetimetable") || strstr(cmd, "nucleoprinttimetable")) {
    UART1_Send("ACK: timetable dump\r\n");
    TT_PrintCurrent();
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

  /* Time set: "nucleo time is DD/MM/YYYY HH:MM:SS" (4-digit; 2-digit still accepted) */
  /* Time set: "nucleo time is DD/MM/YYYY HH:MM:SS" */
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
        /* >>> set a valid weekday (compute or set any 1..7) <<< */
        d_bcd.WeekDay = RTC_WEEKDAY_MONDAY;   // TODO: compute real weekday if you care

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


  /* Wake me up in ... */
  {
    float hours = 0.0f;
    uint32_t delay_ms = 0;
    if (parse_wake_hours(line_in, &hours, &delay_ms)) {
      /* runtime relative schedule */
      RPi_ScheduleWake(delay_ms);

#if defined(HAL_RTC_MODULE_ENABLED) && HAVE_PERSIST_WAKE
      /* persistent absolute time */
      uint32_t now_epoch = rtc_now_epoch2000();
      uint32_t due_epoch = now_epoch + (delay_ms / 1000u);
      Persist_SaveWakeEpoch(due_epoch);
      wake_epoch = due_epoch;
      wake_active = 1;
#else
      UART1_Send("WARN: persisted wake not available; keeping in RAM only\r\n");
#endif

      char resp[96];
      snprintf(resp, sizeof(resp),
               "OK, wake scheduled in %.3f h (~%lu ms).\r\n",
               (double)hours, (unsigned long)delay_ms);
      UART1_Send(resp);
      return;
    }
  }

  /* Goodnight shortcut */
  if (strcmp(cmd, "goodnight") == 0 || strcmp(cmd, "goodnight!") == 0
      || strcmp(cmd, "goodnightnow") == 0) {
    UART1_Send("OK, sleeping now. I will wake you in 60s.\r\n");
    RPi_ScheduleWake(RPI_WAKE_DELAY_MS);
#if defined(HAL_RTC_MODULE_ENABLED) && HAVE_PERSIST_WAKE
    Persist_SaveWakeEpoch(rtc_now_epoch2000() + (RPI_WAKE_DELAY_MS/1000u));
    wake_active = 1;
#endif
    return;
  }

  /* Set daily timetable: expect "nucleotimetable{<24 chars>}{rec,sleep}" */
  if (strstr(cmd, "nucleotimetable")) {
    if (parse_timetable(line_in)) {
      UART1_Send("ACK: timetable set\r\n");
    } else {
      UART1_Send("ERR: invalid timetable format\r\n");
    }
    return;
  }

  /* Resume schedule after processing: "nucleoprocessingcompleted" */
  if (strstr(cmd, "nucleoprocessingcompleted")) {
    if (g_sched_enabled) {
      g_sched_paused = 0;
      /* Resume the schedule. Arm the grace power-off -- but Pi_ArmGraceOff is a
         no-op while a B1 manual override is active, so the Pi stays on until
         B2 or a restart. */
      Pi_ArmGraceOff();
      if (pi_manual_on) {
        UART1_Send("ACK: processing complete, schedule resumed; Pi kept ON (B1 override)\r\n");
      } else {
        UART1_Send("ACK: processing complete, schedule resumed; Pi power-off armed\r\n");
      }
    } else {
      UART1_Send("WARN: no active schedule\r\n");
    }
    return;
  }

  /* Raspberry Pi 5V power supply via optocoupler (PC1 / Arduino D7).
     HIGH = supply ON, LOW = OFF. Check "off" before "on" (distinct strings). */
  if (strstr(cmd, "nucleopipoweroff")) {
    Pi_SetPwr(0u, POFF_CMD);
    pi_pwr_off_armed = 0;                     /* already off; cancel timer */
    pi_manual_on = 0;
    UART1_Send("ACK: Pi 5V power OFF\r\n");
    return;
  }
  if (strstr(cmd, "nucleopipoweron")) {
    Pi_SetPwr(1u, PON_CMD);
    pi_pwr_off_armed = 0;                     /* fresh on; cancel any timed-off */
    g_pi_serviced = 0;                        /* manual power-on -> allow re-serve */
    /* Deliberately does NOT set the sticky B1 maintenance override: only the
       physical B1 button holds the Pi on indefinitely. That keeps a later
       "nucleo turn me off <X>" working after a software power-on. */
    UART1_Send("ACK: Pi 5V power ON\r\n");
    return;
  }

  /* "nucleo turn me off <X>" -> the Pi asks to be powered down in X seconds,
     giving it time to shut down gracefully before the 5V supply is cut. */
  if (strstr(cmd, "nucleoturnmeoff")) {
    if (pi_manual_on) {
      /* B1 manual override active: ignore the Pi's self-timed off so the 5V
         rail stays up until B2 or a board restart. */
      UART1_Send("ACK: ignored -- B1 manual override active (press B2 to release)\r\n");
      return;
    }
    const char *k = strcasestr(line_in, "off");
    unsigned long secs = 0;
    if (k) { k += 3; while (*k && (*k < '0' || *k > '9')) k++; secs = strtoul(k, NULL, 10); }
    if (secs > 86400UL) secs = 86400UL;       /* clamp to 1 day */
    pi_pwr_off_due_ms = HAL_GetTick() + (uint32_t)(secs * 1000UL);
    pi_pwr_off_armed  = 1;
    pi_manual_on = 0;                          /* honor the Pi's own off request */
    char r[64];
    snprintf(r, sizeof(r), "ACK: Pi 5V power OFF in %lu s\r\n", secs);
    UART1_Send(r);
    return;
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

  /* --- Runtime OTAA key provisioning from the Pi (sent to CM0+ via mailbox) ---
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
    UART1_Send("ACK: stored LoRaWAN keys cleared (placeholder used after reset)\r\n");
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

  /* Serve gateway downlinks (e.g. a timetable the gateway pushed) to the Pi.
       nucleo get downlink
     Each stored message is printed as "DL: <text>" then "ACK: downlink end".
     A timetable in the downlink is also applied to OUR scheduler here. */
  if (strstr(cmd, "nucleogetdownlink")) {
    /* NB: leading marker must NOT contain "ACK:" — the Pi stops reading at the
       first "ACK:"/"downlink end" line, so the DL: lines must come before it. */
    UART1_Send("INFO: downlink dump (DL: lines follow)\r\n");
    Korero_ServeDownlinks();
    return;
  }

  /* Push battery data to TTN on the Pi's request (uplink, fire-and-forget):
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

  /* Detection relay: "nucleo det <class>,<time>,<conf>" -> pack + uplink to TTN.
     "nucleo det flush" sends a partial batch immediately. The ACK comes AFTER
     the LoRaWAN send completes, so the Pi paces itself one detection at a time. */
  if (strstr(cmd, "nucleodet")) {
    if (strstr(cmd, "nucleodetflush")) {
      int ok = Korero_LoraSendBatch();
      UART1_Send(ok ? "ACK: det batch sent\r\n" : "NAK: det send failed (not joined/busy)\r\n");
      return;
    }
    /* parse "<class>,<time>,<conf>" from the RAW line (after "det") */
    char *p = strcasestr(line_in, "det");
    long cls = -1, conf = -1;
    unsigned long tm = 0;
    if (p) {
      char *e;
      p += 3;
      cls = strtol(p, &e, 10); p = e;
      while (*p && !(*p >= '0' && *p <= '9')) p++;
      tm  = strtoul(p, &e, 10); p = e;
      while (*p && !(*p >= '0' && *p <= '9')) p++;
      conf = strtol(p, &e, 10);
    }
    if (cls < 0 || conf < 0) {
      UART1_Send("ERR: use 'nucleo det <class>,<time>,<conf>'\r\n");
      return;
    }
    uint8_t *r = &det_batch[det_count * DET_REC_BYTES];
    r[0] = (uint8_t)cls;
    r[1] = (uint8_t)(tm & 0xFF);
    r[2] = (uint8_t)((tm >> 8) & 0xFF);
    r[3] = (uint8_t)((tm >> 16) & 0xFF);
    r[4] = (uint8_t)((tm >> 24) & 0xFF);
    r[5] = (uint8_t)conf;
    det_count++;

    if (det_count >= DET_BATCH_MAX) {
      int ok = Korero_LoraSendBatch();
      UART1_Send(ok ? "ACK: det sent\r\n" : "NAK: det send failed (not joined/busy)\r\n");
    } else {
      char m[40];
      snprintf(m, sizeof(m), "ACK: det queued %u/%u\r\n", det_count, (unsigned)DET_BATCH_MAX);
      UART1_Send(m);
    }
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

/* --- Runtime relative scheduler (ms-based) --- */
static void RPi_ScheduleWake(uint32_t delay_ms) {
#ifdef RPI_WAKE_GPIO_Port
  rpi_wake_state  = RPI_WAKE_SCHEDULED;
  rpi_wake_due_ms = HAL_GetTick() + delay_ms;
#else
  UART1_Send("WARN: RPISwitch pin not mapped in .ioc\r\n");
#endif
}

/* --- Wake tick: handles persisted & runtime schedules --- */
static void RPi_WakeTick(void) {
#ifdef RPI_WAKE_GPIO_Port
  /* Persisted absolute schedule: fire when due & SoC ok */
#if defined(HAL_RTC_MODULE_ENABLED) && HAVE_PERSIST_WAKE
  if (wake_active) {
    uint32_t now_epoch = rtc_now_epoch2000();
    if ((int32_t)(now_epoch - wake_epoch) >= 0) {
      float v   = Battery_ReadVoltage(8);
      float soc = soc_from_voltage(v);
      if (soc >= SOC_WAKE_THRESHOLD_PCT) {
        /* Fire wake pulse now */
        HAL_GPIO_WritePin(RPI_WAKE_GPIO_Port, RPI_WAKE_Pin, GPIO_PIN_RESET);
        rpi_wake_pulse_end_ms = HAL_GetTick() + RPI_WAKE_PULSE_MS;
        rpi_wake_state = RPI_WAKE_PULSING;
        Persist_ClearWake();
        wake_active = 0;
        UART1_Send("Wake pulse (persisted) sent.\r\n");
      } /* else: wait until SoC improves */
    }
  }
#endif

  /* Runtime relative schedule (as before) */
  uint32_t now = HAL_GetTick();
  switch (rpi_wake_state) {
    case RPI_WAKE_SCHEDULED:
      if ((int32_t)(now - rpi_wake_due_ms) >= 0) {
        HAL_GPIO_WritePin(RPI_WAKE_GPIO_Port, RPI_WAKE_Pin, GPIO_PIN_RESET); // pull low
        rpi_wake_pulse_end_ms = now + RPI_WAKE_PULSE_MS;
        rpi_wake_state = RPI_WAKE_PULSING;
      }
      break;
    case RPI_WAKE_PULSING:
      if ((int32_t)(now - rpi_wake_pulse_end_ms) >= 0) {
        HAL_GPIO_WritePin(RPI_WAKE_GPIO_Port, RPI_WAKE_Pin, GPIO_PIN_SET);   // release (high via pull-up)
        rpi_wake_state = RPI_WAKE_IDLE;
        UART1_Send("Wake pulse sent.\r\n");
      }
      break;
    default: break;
  }
#endif
}

/* --- Restart notifier --- */
static void RestartNotifier_Tick(void) {
  if (!restart_left) return;
  uint32_t now = HAL_GetTick();
  if ((int32_t)(now - restart_next_ms) >= 0) {
    UART1_Send("EVENT: MCU_RESTARTED\r\n");
    restart_left--;
    restart_next_ms = now + RESTART_BROADCAST_PERIOD_MS;
  }
}

/* --- Timed Pi 5V power-off: cut PI_PWR once the armed deadline passes --- */
static void Pi_PowerTick(void) {
  if (!pi_pwr_off_armed) return;
  if ((int32_t)(HAL_GetTick() - pi_pwr_off_due_ms) >= 0) {
    Pi_SetPwr(0u, POFF_TIMED);                                   /* Pi 5V OFF */
    pi_pwr_off_armed = 0;
    /* If the max-on backstop fired while still paused (Pi never signalled done),
       resume the schedule so recording restarts instead of idling to the 5 h. */
    if (g_sched_paused) { g_sched_paused = 0; }
    UART1_Send("EVENT: Pi 5V power OFF (timed)\r\n");
  }
}

/* -------------------- RTC helpers & persistence (only if enabled) -------------------- */
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

/* *** CHANGED: parser that accepts DD/MM/YYYY HH:MM:SS; falls back to DD/MM/YY *** */
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

#if HAVE_PERSIST_WAKE
/* Persisted wake using RTC backup registers */
static void Persist_SaveWakeEpoch(uint32_t epoch) {
  HAL_PWR_EnableBkUpAccess();
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_WAKE_EPOCH, epoch);
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_FLAGS, BKP_FLAG_WAKE_ACTIVE);
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_MAGIC, BKP_MAGIC);
}

static int Persist_LoadWakeEpoch(uint32_t *epoch) {
  HAL_PWR_EnableBkUpAccess();
  uint32_t magic = HAL_RTCEx_BKUPRead(&hrtc, BKP_REG_MAGIC);
  uint32_t flags = HAL_RTCEx_BKUPRead(&hrtc, BKP_REG_FLAGS);
  if (magic == BKP_MAGIC && (flags & BKP_FLAG_WAKE_ACTIVE)) {
    if (epoch) *epoch = HAL_RTCEx_BKUPRead(&hrtc, BKP_REG_WAKE_EPOCH);
    return 1;
  }
  return 0;
}

static void Persist_ClearWake(void) {
  HAL_PWR_EnableBkUpAccess();
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_FLAGS, 0);
}
#endif /* HAVE_PERSIST_WAKE */

/* -------- LoRaWAN OTAA key persistence in RTC backup registers ------------- */
#if defined(HAL_RTC_MODULE_ENABLED) && LK_HAVE_PERSIST
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

/* Snapshot the OTAA keys currently in the mailbox into backup registers.
   Only persists when a non-zero AppKey is present. */
static void Persist_SaveLoraKeys(void) {
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
  UART1_Send("INFO: LoRaWAN keys saved to backup registers (persist across reset)\r\n");
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
}
#endif /* LK_HAVE_PERSIST */

/* ---------------- Timetable persistence ---------------- */
static uint32_t TT_CalcCRC32(const uint8_t *data, size_t len)
{
  /* Simple 32-bit rolling checksum */
  uint32_t s = 0x12345678u;
  for (size_t i = 0; i < len; ++i) {
    s += data[i];
    s = (s << 5) | (s >> 27);
  }
  return s;
}

static uint16_t TT_CalcCRC16(const uint8_t *data, size_t len)
{
  /* CRC-16/CCITT-FALSE like (simple variant) */
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000u) crc = (crc << 1) ^ 0x1021u;
      else               crc = (crc << 1);
    }
  }
  return crc;
}

static uint8_t TT_ModeToBits(char c)
{
  switch (c) {
    case 'S': case 's': return 1u;
    case 'U': case 'u': return 2u;
    case 'P': case 'p': return 3u;
    default: return 0u;
  }
}

static char TT_BitsToMode(uint8_t b)
{
  switch (b & 3u) {
    case 1u: return 'S';
    case 2u: return 'U';
    case 3u: return 'P';
    default: return '0';
  }
}

static void TT_SaveToBKP(const char modes[24], uint32_t rec_ms, uint32_t sleep_ms)
{
#if (defined(HAL_RTC_MODULE_ENABLED) && (BKP_COUNT >= 5))
  HAL_PWR_EnableBkUpAccess();

  /* Pack 24 modes into 48 bits (2 bits/hour): 00=0,01=S,10=U,11=P */
  uint64_t bits = 0;
  for (int i = 0; i < 24; ++i) {
    uint8_t b = TT_ModeToBits(modes[i]) & 3u;
    bits |= ((uint64_t)b) << (i * 2);
  }
  uint32_t pack0 = (uint32_t)(bits & 0xFFFFFFFFu);
  uint32_t pack1 = (uint32_t)((bits >> 32) & 0xFFFFFFFFu); /* only low 16 bits used */

#if TT_HAVE_RICH
  /* Invalidate first */
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_MAGIC, 0);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_FLAGS, 0);

  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_PACK0,   pack0);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_PACK1,   pack1);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_REC_MS,  rec_ms);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_SLEEP_MS, sleep_ms);

  /* CRC32 over [pack0 | pack1 | rec_ms | sleep_ms] */
  uint8_t blob[16];
  memcpy(blob+0,  &pack0,   4);
  memcpy(blob+4,  &pack1,   4);
  memcpy(blob+8,  &rec_ms,  4);
  memcpy(blob+12, &sleep_ms,4);
  uint32_t crc = TT_CalcCRC32(blob, sizeof(blob));
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_CRC32, crc);

  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_MAGIC, TT_MAGIC);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_FLAGS, TT_FLAG_VALID);
#else
  /* Compact layout: store CRC16 in high 16 bits of PACK1; durations packed as seconds in TT_REG_DUR_COMBO */
  uint8_t blob[8];
  memcpy(blob+0,  &pack0,   4);
  memcpy(blob+4,  &pack1,   4);
  uint16_t crc16 = TT_CalcCRC16(blob, sizeof(blob));
  uint32_t pack1_crc = (pack1 & 0x0000FFFFu) | ((uint32_t)crc16 << 16);

  /* Pack durations as seconds into a single 32-bit word */
  uint32_t rec_s   = (rec_ms   + 500u) / 1000u;
  uint32_t sleep_s = (sleep_ms + 500u) / 1000u;
  if (rec_s   > 65535u) rec_s   = 65535u;
  if (sleep_s > 65535u) sleep_s = 65535u;
  uint32_t dur_combo = (sleep_s << 16) | (rec_s & 0xFFFFu);

  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_MAGIC,      TT_MAGIC);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_PACK0,      pack0);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_PACK1,      pack1_crc);
  HAL_RTCEx_BKUPWrite(&hrtc, TT_REG_DUR_COMBO,  dur_combo);
#endif

#else
  (void)modes; (void)rec_ms; (void)sleep_ms;
#endif
}

static int TT_LoadFromBKP(char modes_out[24], uint32_t *rec_ms, uint32_t *sleep_ms)
{
#if (defined(HAL_RTC_MODULE_ENABLED) && (BKP_COUNT >= 5))
  HAL_PWR_EnableBkUpAccess();
  /* Check magic */
  if (HAL_RTCEx_BKUPRead(&hrtc, TT_REG_MAGIC) != TT_MAGIC) return 0;

  uint32_t pack0 = 0, pack1 = 0, r = 0, s = 0;

#if TT_HAVE_RICH
  if (!(HAL_RTCEx_BKUPRead(&hrtc, TT_REG_FLAGS) & TT_FLAG_VALID)) return 0;

  pack0 = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_PACK0);
  pack1 = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_PACK1);
  r     = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_REC_MS);
  s     = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_SLEEP_MS);

  uint32_t crc_s = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_CRC32);
  uint8_t blob[16];
  memcpy(blob+0,  &pack0, 4);
  memcpy(blob+4,  &pack1, 4);
  memcpy(blob+8,  &r,     4);
  memcpy(blob+12, &s,     4);
  uint32_t crc_c = TT_CalcCRC32(blob, sizeof(blob));
  if (crc_c != crc_s) return 0;
#else
  pack0 = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_PACK0);
  pack1 = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_PACK1);
  uint32_t dur_combo = HAL_RTCEx_BKUPRead(&hrtc, TT_REG_DUR_COMBO);

  /* Validate CRC16 from high 16 bits of PACK1 */
  uint16_t crc_s = (uint16_t)((pack1 >> 16) & 0xFFFFu);
  uint32_t pack1_low16 = (pack1 & 0x0000FFFFu);
  uint8_t blob[8];
  memcpy(blob+0, &pack0, 4);
  memcpy(blob+4, &pack1_low16, 4);
  uint16_t crc_c = TT_CalcCRC16(blob, sizeof(blob));
  if (crc_c != crc_s) return 0;

  /* Unpack durations (seconds) */
  uint32_t rec_s   = (dur_combo & 0x0000FFFFu);
  uint32_t sleep_s = (dur_combo >> 16) & 0x0000FFFFu;
  r = rec_s * 1000u;
  s = sleep_s * 1000u;
#endif

  /* Unpack to 24 chars */
  uint64_t bits = ((uint64_t)pack0) | (((uint64_t)pack1) << 32);
  for (int i = 0; i < 24; ++i) {
    modes_out[i] = TT_BitsToMode((uint8_t)((bits >> (i*2)) & 3u));
  }
  if (rec_ms)   *rec_ms   = r;
  if (sleep_ms) *sleep_ms = s;
  return 1;
#else
  (void)modes_out; (void)rec_ms; (void)sleep_ms;
  return 0;
#endif
}

/* Pretty-printer: show timetable from BKP (if present) or current runtime */
static void TT_PrintCurrent(void)
{
#if defined(HAL_RTC_MODULE_ENABLED)
  char m[24]; uint32_t r=0, s=0;
  if (TT_LoadFromBKP(m, &r, &s)) {
    UART1_Send("TTABLE FROM BKP: ");
    /* Print as "{...}{rec_s,sleep_s}" */
    UART1_Send("{");
    for (int i=0;i<24;i++){
      char c = m[i];
      char out[4];
      if (i<23) snprintf(out, sizeof(out), "%c,", c);
      else      snprintf(out, sizeof(out), "%c",  c);
      Console_Tx((uint8_t*)out, (uint16_t)strlen(out));
    }
    UART1_Send("},{");
    char num[32];
    snprintf(num, sizeof(num), "%lu,%lu", (unsigned long)((r+500)/1000), (unsigned long)((s+500)/1000));
    UART1_Send(num);
    UART1_Send("}\r\n");
    return;
  }
#endif
  /* Fallback: print current runtime timetable if enabled */
  if (g_sched_enabled) {
    UART1_Send("TTABLE (RUNTIME ONLY): ");
    UART1_Send("{");
    for (int i=0;i<24;i++){
      char c = g_day_schedule[i];
      char out[4];
      if (i<23) snprintf(out, sizeof(out), "%c,", c);
      else      snprintf(out, sizeof(out), "%c",  c);
      Console_Tx((uint8_t*)out, (uint16_t)strlen(out));
    }
    UART1_Send("},{");
    char num[32];
    snprintf(num, sizeof(num), "%lu,%lu", (unsigned long)((g_rec_ms+500)/1000), (unsigned long)((g_sleep_ms+500)/1000));
    UART1_Send(num);
    UART1_Send("}\r\n");
  } else {
    UART1_Send("TTABLE: none saved; Plan B active (wake Pi at 11:00 local time).\r\n");
  }
}

#endif /* HAL_RTC_MODULE_ENABLED */

/* USER CODE END 4 */

/**
  * @brief  BSP Push Button callback
  */


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
