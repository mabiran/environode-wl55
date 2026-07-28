/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    timer_if.c
  * @brief   KoreroNet CM0+ timer back-end for the LoRaWAN UTIL_TIMER server,
  *          implemented on LPTIM1 clocked from the LSE (32.768 kHz).
  *
  * WHY NOT THE RTC:  ST's stock timer_if drives the RTC in 32-bit *binary*
  * mode (Alarm A + backup registers DR0..DR2). On this board the RTC is owned
  * by the CM4 application (BCD calendar + backup registers for the schedule /
  * wake epoch), and the two RTC modes are mutually exclusive. So this core
  * gets its own accurate, low-power timebase on LPTIM1/LSE and never touches
  * the RTC. LSE (±20 ppm) is accurate enough for LoRaWAN RX-window timing;
  * an HSI-clocked timer would not be.
  *
  * TICK RATE:  1 tick = 1/32768 s (LSE, prescaler /1). LPTIM1 is a 16-bit
  * counter, so we extend it to 32 bits in software by counting auto-reload
  * (overflow) events. The 32-bit tick wraps every 2^32/32768 ≈ 36.4 h; the
  * UTIL_TIMER server only ever uses *relative* timeouts (all well under that),
  * so wrap is harmless.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include <stdbool.h>
#include "timer_if.h"
#include "main.h"
#include "utilities_def.h"
#include "stm32wlxx.h"

/* External / exported driver tables ----------------------------------------*/
/**
  * @brief Timer driver callbacks handler (consumed by stm32_timer.c)
  */
const UTIL_TIMER_Driver_s UTIL_TimerDriver =
{
  TIMER_IF_Init,
  NULL,

  TIMER_IF_StartTimer,
  TIMER_IF_StopTimer,

  TIMER_IF_SetTimerContext,
  TIMER_IF_GetTimerContext,

  TIMER_IF_GetTimerElapsedTime,
  TIMER_IF_GetTimerValue,
  TIMER_IF_GetMinimumTimeout,

  TIMER_IF_Convert_ms2Tick,
  TIMER_IF_Convert_Tick2ms,
};

/**
  * @brief SysTime driver callbacks handler (consumed by stm32_systime.c)
  */
const UTIL_SYSTIM_Driver_s UTIL_SYSTIMDriver =
{
  TIMER_IF_BkUp_Write_Seconds,
  TIMER_IF_BkUp_Read_Seconds,
  TIMER_IF_BkUp_Write_SubSeconds,
  TIMER_IF_BkUp_Read_SubSeconds,
  TIMER_IF_GetTime,
};

/* Private defines -----------------------------------------------------------*/
/** @brief LSE tick rate: 2^15 ticks per second. */
#define TIMER_IF_TICKS_PER_SEC_LOG2   15u
#define TIMER_IF_TICKS_PER_SEC        (1u << TIMER_IF_TICKS_PER_SEC_LOG2) /* 32768 */

/** @brief Smallest timeout we will program, in ticks (~150 us). */
#define MIN_ALARM_DELAY               5u

/** @brief Map UTIL_TIMER_IRQ onto the timer server handler (overridable). */
#ifndef UTIL_TIMER_IRQ_MAP_INIT
#define UTIL_TIMER_IRQ_MAP_INIT()
#endif
#ifndef UTIL_TIMER_IRQ_MAP_PROCESS
#define UTIL_TIMER_IRQ_MAP_PROCESS() UTIL_TIMER_IRQ_Handler()
#endif

/* Private variables ---------------------------------------------------------*/
static volatile bool     TimerInitialized = false;
static volatile uint32_t TimerContext     = 0;     /* reference tick           */
static volatile uint16_t CounterMSB       = 0;     /* high 16 bits (overflows) */
static volatile uint32_t AlarmTarget      = 0;     /* absolute tick to fire at */
static volatile bool     AlarmArmed       = false;

/* SysTime backup (held in RAM; no RTC backup domain on this core) */
static volatile uint32_t BkUpSeconds      = 0;
static volatile uint32_t BkUpSubSeconds   = 0;

/* Private function prototypes -----------------------------------------------*/
static uint32_t GetTimerTicks(void);
static uint16_t LPTIM_ReadCNT(void);
static void     LPTIM_WriteCMP(uint16_t value);

/* Exported functions --------------------------------------------------------*/

UTIL_TIMER_Status_t TIMER_IF_Init(void)
{
  if (TimerInitialized == false)
  {
    /* Kernel clock = LSE (enabled by CM4); enable the peripheral clock. */
    __HAL_RCC_LPTIM1_CONFIG(RCC_LPTIM1CLKSOURCE_LSE);
    __HAL_RCC_LPTIM1_CLK_ENABLE();

    /* Configure while disabled: internal clock, prescaler /1, sw trigger. */
    LPTIM1->CR   = 0u;                 /* ensure disabled */
    LPTIM1->CFGR = 0u;                 /* CKSEL=0 (kernel clk), PRESC=/1, cont. */
    LPTIM1->IER  = LPTIM_IER_ARRMIE | LPTIM_IER_CMPMIE;

    /* Enable, then program ARR for full-scale free-run, then start. */
    LPTIM1->CR  = LPTIM_CR_ENABLE;
    LPTIM1->ICR = LPTIM_ICR_ARROKCF | LPTIM_ICR_CMPOKCF;
    LPTIM1->ARR = 0xFFFFu;
    while ((LPTIM1->ISR & LPTIM_ISR_ARROK) == 0u) { /* wait ARR written */ }
    LPTIM1->ICR = LPTIM_ICR_ARROKCF;

    CounterMSB = 0u;
    AlarmArmed = false;

    /* Start continuous counting. */
    LPTIM1->CR |= LPTIM_CR_CNTSTRT;

    HAL_NVIC_SetPriority(LPTIM1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(LPTIM1_IRQn);

    TimerContext = GetTimerTicks();
    UTIL_TIMER_IRQ_MAP_INIT();
    TimerInitialized = true;
  }
  return UTIL_TIMER_OK;
}

UTIL_TIMER_Status_t TIMER_IF_StartTimer(uint32_t timeout)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  AlarmTarget = TimerContext + timeout;
  AlarmArmed  = true;

  /* Program CMP to the low 16 bits of the target. The match fires once per
     2 s wrap; the ISR confirms the *32-bit* target has been reached before
     calling the timer-server handler (handles multi-wrap timeouts). */
  LPTIM_WriteCMP((uint16_t)(AlarmTarget & 0xFFFFu));

  __set_PRIMASK(primask);
  return UTIL_TIMER_OK;
}

UTIL_TIMER_Status_t TIMER_IF_StopTimer(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  AlarmArmed = false;
  LPTIM1->ICR = LPTIM_ICR_CMPMCF;
  __set_PRIMASK(primask);
  return UTIL_TIMER_OK;
}

uint32_t TIMER_IF_SetTimerContext(void)
{
  TimerContext = GetTimerTicks();
  return TimerContext;
}

uint32_t TIMER_IF_GetTimerContext(void)
{
  return TimerContext;
}

uint32_t TIMER_IF_GetTimerElapsedTime(void)
{
  return (uint32_t)(GetTimerTicks() - TimerContext);
}

uint32_t TIMER_IF_GetTimerValue(void)
{
  if (TimerInitialized == true)
  {
    return GetTimerTicks();
  }
  return 0u;
}

uint32_t TIMER_IF_GetMinimumTimeout(void)
{
  return MIN_ALARM_DELAY;
}

uint32_t TIMER_IF_Convert_ms2Tick(uint32_t timeMilliSec)
{
  return (uint32_t)((((uint64_t)timeMilliSec) << TIMER_IF_TICKS_PER_SEC_LOG2) / 1000u);
}

uint32_t TIMER_IF_Convert_Tick2ms(uint32_t tick)
{
  return (uint32_t)((((uint64_t)tick) * 1000u) >> TIMER_IF_TICKS_PER_SEC_LOG2);
}

void TIMER_IF_DelayMs(uint32_t delay)
{
  uint32_t delayTicks = TIMER_IF_Convert_ms2Tick(delay);
  uint32_t start      = GetTimerTicks();
  while ((GetTimerTicks() - start) < delayTicks)
  {
    __NOP();
  }
}

uint32_t TIMER_IF_GetTime(uint16_t *mSeconds)
{
  uint32_t ticks   = GetTimerTicks();
  uint32_t seconds = ticks >> TIMER_IF_TICKS_PER_SEC_LOG2;
  uint32_t sub     = ticks & (TIMER_IF_TICKS_PER_SEC - 1u);
  if (mSeconds != NULL)
  {
    *mSeconds = (uint16_t)TIMER_IF_Convert_Tick2ms(sub);
  }
  return seconds;
}

void TIMER_IF_BkUp_Write_Seconds(uint32_t Seconds)       { BkUpSeconds = Seconds; }
uint32_t TIMER_IF_BkUp_Read_Seconds(void)                { return BkUpSeconds; }
void TIMER_IF_BkUp_Write_SubSeconds(uint32_t SubSeconds) { BkUpSubSeconds = SubSeconds; }
uint32_t TIMER_IF_BkUp_Read_SubSeconds(void)             { return BkUpSubSeconds; }

/* Interrupt handler ---------------------------------------------------------*/
/**
  * @brief LPTIM1 interrupt: maintains the 32-bit counter and fires the alarm.
  * @note  Referenced from stm32wlxx_it.c (LPTIM1_IRQHandler).
  */
void TIMER_IF_LPTIM1_IRQHandler(void)
{
  uint32_t isr = LPTIM1->ISR;

  /* Auto-reload (overflow) -> extend the counter. */
  if ((isr & LPTIM_ISR_ARRM) != 0u)
  {
    LPTIM1->ICR = LPTIM_ICR_ARRMCF;
    CounterMSB++;
  }

  /* Compare match (once per wrap). */
  if ((isr & LPTIM_ISR_CMPM) != 0u)
  {
    LPTIM1->ICR = LPTIM_ICR_CMPMCF;
  }

  /* Fire only when the full 32-bit target has actually been reached. */
  if (AlarmArmed && ((int32_t)(GetTimerTicks() - AlarmTarget) >= 0))
  {
    AlarmArmed = false;
    UTIL_TIMER_IRQ_MAP_PROCESS();
  }
}

/* Private functions ---------------------------------------------------------*/

/** @brief Read the 16-bit LPTIM counter reliably (async domain). */
static uint16_t LPTIM_ReadCNT(void)
{
  uint16_t c1 = (uint16_t)LPTIM1->CNT;
  uint16_t c2 = (uint16_t)LPTIM1->CNT;
  while (c1 != c2)
  {
    c1 = c2;
    c2 = (uint16_t)LPTIM1->CNT;
  }
  return c1;
}

/** @brief Compose the 32-bit tick from the software MSB and 16-bit CNT,
  *        correcting for an overflow that has occurred but not yet been
  *        serviced by the ISR (e.g. when called with IRQs masked). */
static uint32_t GetTimerTicks(void)
{
  uint16_t msb;
  uint16_t cnt;
  bool     carry;
  do
  {
    msb   = CounterMSB;
    cnt   = LPTIM_ReadCNT();
    /* Overflow has occurred but the ISR has not yet incremented CounterMSB
       (e.g. we are in a critical section): the counter has wrapped to a small
       value while ARRM is still pending -> add the missing carry. */
    carry = (((LPTIM1->ISR & LPTIM_ISR_ARRM) != 0u) && (cnt < 0x8000u));
  } while (msb != CounterMSB);          /* retry if the ISR raced our read */

  if (carry)
  {
    msb++;
  }
  return ((uint32_t)msb << 16) | cnt;
}

/** @brief Write the LPTIM compare register and wait for CMPOK. */
static void LPTIM_WriteCMP(uint16_t value)
{
  LPTIM1->ICR = LPTIM_ICR_CMPOKCF;
  LPTIM1->CMP = value;
  while ((LPTIM1->ISR & LPTIM_ISR_CMPOK) == 0u) { /* wait CMP written */ }
  LPTIM1->ICR = LPTIM_ICR_CMPOKCF;
}
