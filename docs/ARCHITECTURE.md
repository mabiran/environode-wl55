# EnviroNode-WL55 — Architecture & Program Flow

How the firmware actually runs: boot, the main loop, where the LoRaWAN identity
comes from, how a measurement becomes a radio frame, and how a downlink becomes
configuration. Companion to [MASTER.md](MASTER.md) (narrative) and
[LOGBOOK.md](LOGBOOK.md) (procedures); byte-level truth lives in
[PAYLOAD.md](PAYLOAD.md) and [CONFIG.md](CONFIG.md).

## 1 · Dual-core split

```
        ┌──────────────── STM32WL55JC ─────────────────┐
  I²C1 ─┤ CM4 (application core)                       │
  I²C2 ─┤   sensor drivers · scheduler · sleep         │
   ADC ─┤   payload codec · config · offline log       │
  SPI1 ─┤   console · event log · IWDG                 │
  EXTI ─┤                                              │
        │        ▲  SRAM2 mailbox @ 0x2000FC00  ▼      │
        │ CM0+ (radio core): LoRaMAC · OTAA · SubGHz ──┼──► AU915 ──► TTN
        └──────────────────────────────────────────────┘
```

CM4 owns every application peripheral; CM0+ owns only the radio and is reused
from KoreroNet essentially unchanged. The **mailbox** (`korero_mailbox.h`, must
stay byte-identical in both projects) carries four channels:

| Channel | Direction | Handshake |
|---|---|---|
| Uplink command (payload + FPort) | CM4 → CM0+ | CM4 bumps `req_seq`; CM0+ transmits, writes `status`, sets `ack_seq` |
| Trace ring | CM0+ → CM4 | byte ring; CM4 drains it onto the console so one serial port shows both cores |
| Downlink ring | CM0+ → CM4 | **every** received downlink (port + bytes), 4 slots, newest wins on overflow |
| OTAA keys | CM4 → CM0+ | CM4 writes DevEUI/JoinEUI/AppKey, bumps `key_seq`; CM0+ applies + re-joins, sets `key_ack` |

## 2 · Boot sequence (CM4, `main()`)

What happens, in order, with the console line each step prints:

| # | Step | Console evidence |
|---|---|---|
| 1 | Clocks: HSI 16 MHz core, LSE→RTC (LSI fallback) | — |
| 2 | Peripherals: GPIO, ADC (+calibration), I²C1, I²C2, SPI1, USART1/2, RTC | — |
| 3 | RTC status | `BOOT: BKP_DR0=… <date> <time>` |
| 4 | Config from flash page 62 (or defaults) | `CONFIG: {LW,T1,T2,1}` |
| 5 | Sleep subsystem: RTC wake-up IRQ armed | — |
| 6 | Offline log: scan ring, find head | `LOG  : n of 153 records` + `SDLOG:` status |
| 7 | Sensor drivers probed (absent ≠ fatal) | `SENSORS: partial (air1=y … ina219=n)` |
| 8 | Mailbox zeroed, **then** CM0+ released | `BOOT: CM0+ (radio core) released` |
| 9 | **Identity restore** (§3) → mailbox → CM0+ joins | `BOOT: restored LoRaWAN keys from …` |
| 10 | First loop pass: event log entry, reset-cause, **IWDG started** (~15 s, /256, RLR 1875) | `nucleo report` shows the boot |

The first uplink is scheduled ~60 s after boot so the OTAA join can finish
first.

## 3 · Where the LoRaWAN identity comes from

Two layers exist, and the CM4 layer always wins:

**CM0+ compiled-in defaults** — the stock LoRaMAC key table
(`00:11:22…` AppKey and the FIPS test-vector session keys) printed in the CM0+
boot banner. **This table is never what the node uses**: moments later CM4
provisions over the mailbox and forces a re-join. Treat that banner as noise;
`info` on the console is the authority.

**CM4 identity chain**, tried in order at boot (step 9):

```
 0. SD card CONFIG.INI    (highest)    applied + persisted when present —
        │ miss                          field provisioning by prepared card
        ▼
 1. RTC backup registers  DR9–DR18     survives reset; power loss only with VBAT
        │ miss                          (coin cell, SB21 removed — LOGBOOK §7.2)
        ▼
 2. Flash page 63         0x0801F800   survives any power loss and re-flash
        │ miss                          (envnode_keystore.c; magic+checksum)
        ▼
 3. Compiled-in default   envnode_identity.c   AppKey "ENVNODE-PLACEHLD",
                                       DevEUI all-zero = use the chip-UID DevEUI
```

Whichever layer hits is copied into the mailbox, `key_seq` is bumped, and CM0+
applies the keys and (re)joins. So: **hard-coded during programming = layer 3
only**, deliberately a recognisable placeholder so a virgin board joins and is
visible; real keys are provisioned per node over the console —

```
nucleo lorawan appkey <32 hex>     → backup registers + flash mirror
nucleo lorawan forget              → wipe both, revert to layer 3 immediately
```

— and `info` reports which layer is active (`Key store : flash + backup
registers (survives power loss)`). The DevEUI to register on TTN is derived
from the chip's factory UID and printed by `info`.

## 4 · Main loop

Every pass (~50 ms pacing), in order:

```
 IWDG refresh
 UART RX re-arm (both consoles)
 Korero_DrainTrace()        CM0+ log → VCP
 Korero_JoinTick()          announce join state changes
 PowerStats_Tick()          INA219 sample + coulomb counter
 EnvNode_DrainDownlinks()   §6 — config strings + binary commands
 EnvNode_ScheduleTick()     §5 — measure + uplink when due
 Console_HandleLine()       if a command line arrived
 Status_Led_Tick()
 EnvNode_SleepTick()        §7 — STOP2 until next deadline, if permitted
```

## 5 · Measurement → uplink flow

`EnvNode_ScheduleTick()` fires when the interval elapses (or `uplink_now`):

```
 selected sensors sampled ──► 32-byte frame packed (PAYLOAD.md)
     │  (VSENS rail: on → 15 ms settle → convert → off)
     │  (WS selected: 3 s ADC burst on the speed contact)
     ▼
 OFFLINE LOG append (flash ring) + SD CSV row       ← before radio, always
     ▼
 mailbox post, req_seq++  ──► CM0+ transmits FPort 1 ──► RX1/RX2 windows
     ▼                                                       │
 on KORERO_ST_SENT only: rain accumulator cleared            ▼
                                              downlinks → mailbox ring
```

Failure handling: radio refusal (not joined / duty cycle) → retry in 60 s, rain
totals preserved; sensor failure → sentinel fields + fault bit, frame still
goes; sampling gates on the sensor mask, so a deselected sensor is skipped
silently (no fault).

## 6 · Downlink flow

```
 TTN queues payload ──► delivered in RX window after our next uplink
     ▼
 CM0+ OnRxData: EVERY port → mailbox ring (demo handling for ports 2/3 kept,
     ▼           but they store first — LOGBOOK r7 fixed the swallow)
 CM4 EnvNode_DrainDownlinks():
     first byte '{'  → sensor-set config string, ANY FPort (CONFIG.md)
     FPort 10        → binary command table (PAYLOAD.md)
     anything else   → printed as hex, never silently dropped
     ▼
 accepted config → RAM → flash page 62 (only if changed) → canonical echo
```

The console accepts the identical `{…}` strings, parsed from the **raw** line
(the command normalizer strips commas, which would destroy them).

## 7 · Sleep cycle

When no edge-counted sensor is selected (today: only rain `R` blocks) and
`nucleo sleep` is on:

```
 deadline − now > guard? ──► "SLEEP: STOP2 for Ns" ──► chunks of ≤8 s:
      IWDG refresh → RTC wake-up timer → STOP2/WFI → wake →
      SystemClock_Config() (STOP2 leaves MSI) → uwTick += slept
 ──► "WAKE : after Ns" → normal loop resumes, deadlines undrifted
```

The ~10 s post-transmit awake window exists because Class A can only receive
after its own uplink — sleeping through RX1/RX2 would make the node deaf.
Details and hazards: LOGBOOK [§6.4](LOGBOOK.md#64-sleep-and-power).

## 8 · Persistence map

| What | Where | Written when |
|---|---|---|
| Sensor set, interval, calibration, vane offset | flash page 62 | accepted config change (only if changed) |
| OTAA identity | backup registers DR9–18 **and** flash page 63 | provisioning only |
| Offline sensor log | flash pages 59–61, ring (153) | every measurement |
| SD CSV log | `YYYYMMDD.CSV` on the card, f_sync per row | every measurement, when a card is mounted |
| Event log (boots, reset causes) | `.noinit` RAM | warm-reset survivable only |
| RTC time | RTC + VBAT domain | `nucleo time is …` |

Two rules govern all flash writes: separate pages by blast radius (a config
erase must never endanger the identity), and never write on a timer or near an
RX window (an erase stalls CM0+ fetches 20–40 ms).

## 9 · Reuse map (from KoreroNet 2) — final state

| Subsystem | Fate |
|---|---|
| SubGHz + LoRaMAC + OTAA (CM0+) | reused; one fix (store *all* downlink ports) |
| Mailbox | reused as-is |
| Key persistence | extended: backup regs + **flash mirror** + compiled fallback |
| UART command server | reused; renamed `Console_HandleLine`, commands replaced |
| RTC/epoch helpers, event log, IWDG | reused as-is |
| Recording timetable, Pi power/wake, AudioMoth, detection relay | **deleted** — replaced by the sensor-set config string and the measurement scheduler |
