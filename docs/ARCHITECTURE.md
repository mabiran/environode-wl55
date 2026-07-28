# EnviroNode-WL55 — Architecture

## Dual-core split (STM32WL55JC)
```
        ┌──────────────── STM32WL55JC ────────────────┐
  I²C1 ─┤ CM4 (application)                            │
  I²C2 ─┤   • sensor drivers (BME280×2, MAX31865,      │
   ADC ─┤     analog probes, pulse counters)           │
  SPI1 ─┤   • sampling scheduler (RTC/LPTIM wake)      │
  EXTI ─┤   • payload pack/parse                        │
        │   • UART command server · event log · IWDG   │
        │            ▲  SRAM2 mailbox @0x2000FC00  ▼    │
        │ CM0+ (radio): LoRaMAC · OTAA · SubGHz ───────┼──► LoRaWAN ──► TTN
        └──────────────────────────────────────────────┘
```
- **CM4** owns every sensor peripheral and the sampling/uplink logic.
- **CM0+** owns only the radio (LoRaWAN MAC + SubGHz). It is reused ~unchanged
  from KoreroNet.
- They communicate through the **1 KB shared mailbox in SRAM2** (`korero_mailbox.h`):
  CM4 posts an uplink payload + FPort and bumps a sequence; CM0+ transmits and
  ACKs. Downlinks flow back through the mailbox's downlink ring.

## Measurement cycle (target design)
```
 RTC/LPTIM wake ─► power up sensor rails ─► sample all sensors ─►
   pack 30-byte frame ─► hand to CM0+ (mailbox) ─► CM0+ uplinks (FPort 1) ─►
   drain any downlink (FPort 10) ─► apply config ─► sleep until next interval
```
Rain and wind-speed are **event-driven** (pulse ISRs accumulate between wakes);
everything else is **sampled on wake**. Wind gust = max instantaneous speed over
the interval.

## Reuse map (from KoreroNet 2)
| Subsystem | Reuse | Where |
|---|---|---|
| SubGHz + LoRaMAC + OTAA join | as-is | CM0+ core |
| CM4↔CM0+ mailbox | as-is | `korero_mailbox.h` (both cores) |
| OTAA key persistence (backup regs) | as-is | CM4 `Persist_*` |
| Downlink ring → CM4 handler | adapt | `Korero_ServeDownlinks` → command table |
| UART command server + `normalize_cmd` | reuse | CM4 `RPi_HandleLine` |
| RTC / epoch helpers | reuse | CM4 |
| Event log + `report` command | reuse | CM4 `EvLog_*` |
| IWDG watchdog | reuse | CM4 boot-once block |
| Scheduler / AudioMoth / Pi-power | **replace** | CM4 (new sensor scheduler) |

## Power strategy
Solar + battery like KoreroNet. Sample in short bursts and sleep between; gate
sensor excitation rails where possible; keep the LoRa duty low (default 15-min
uplink). Battery voltage is itself a telemetry channel; a low-battery threshold
can trigger a longer interval or an alarm uplink (FPort 3).

## Persistence
Config that must survive a reset (uplink interval, calibration offsets, wind-vane
offset, rain accumulator) lives in **RTC backup registers**, exactly as KoreroNet
persists its timetable and LoRaWAN keys. A cold power-loss clears them; defaults
are re-applied on first boot.
