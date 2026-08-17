# EnviroNode-WL55: A Low-Power LoRaWAN Agrometeorological Sensor Node

**Master document** — the narrative account of this project, written to be read
front-to-back like a thesis: what was built, why each decision fell the way it
did, what is proven, and what remains open.

> **Where this document sits.** The specifications are normative and this
> document is descriptive: if they ever disagree, [PINOUT.md](PINOUT.md),
> [PAYLOAD.md](PAYLOAD.md), [CONFIG.md](CONFIG.md) and the source code win, and
> this file has the bug. Procedures, bench records and the dated change history
> live in [LOGBOOK.md](LOGBOOK.md), the companion replication manual.

| | |
|---|---|
| Revision | m1 — 2026-08-04, tracks LOGBOOK r14 |
| Author | Mabiran (AUT University, Ecology Project) with Claude (Anthropic) as firmware co-author |
| Platform | NUCLEO-WL55JC1 (STM32WL55JC) · Grove Base Shield V2 · LoRaWAN AU915 / The Things Network |

## Abstract

EnviroNode-WL55 is a solar-capable field node that measures up to nine
agrometeorological channels — paired air temperature/humidity/pressure, leaf
wetness, soil moisture, soil temperature, wind speed and direction, rainfall,
and battery state — and reports them over LoRaWAN in a single 32-byte frame.
The node is reconfigurable in the field and over the air by a human-readable
configuration string (`{LW,T1,T2,15}`), sleeps in STOP2 between measurement
cycles, survives total power loss without losing its network identity or its
configuration, and keeps a timestamped offline log of every measurement so data
outlives network outages. The firmware was migrated from a working acoustic
monitoring node (KoreroNet 2), retaining its proven radio core and dual-core
mailbox while replacing the entire application layer. Every load-bearing claim
in this document is either verified on hardware or explicitly marked as
pending; the distinction is maintained deliberately.

**Keywords:** LoRaWAN, STM32WL55, agrometeorology, low-power sensing, leaf
wetness, Class A, dual-core, The Things Network.

---

## 1 · Introduction

### 1.1 Motivation

Microclimate drives ecology at scales weather stations do not resolve: leaf
wetness duration governs fungal infection windows, soil moisture and
temperature gate germination and invertebrate activity, and wind exposure
shapes canopy structure. Commercial agrometeorological stations exist but are
costly per site, closed, and awkward to fleet. This project builds an open,
replicable node cheap enough to deploy in numbers, using long-range low-power
radio (LoRaWAN) so sites need no mains power and no cellular coverage.

### 1.2 Objectives

1. Measure the nine channels above on a configurable interval, with per-sensor
   selection changeable **remotely** (downlink) and **locally** (console).
2. Survive field reality: power loss without identity loss, network outage
   without data loss, a missing sensor without a corrupted frame.
3. Battery/solar operation: sleep between cycles, spend energy only on demand.
4. Be replicable from documentation alone — the test for
   [LOGBOOK.md](LOGBOOK.md) is that a stranger with the parts can build node 2.

### 1.3 Heritage and contributions

The node descends from **KoreroNet 2**, a bioacoustic LoRaWAN node on the same
MCU. Retained essentially unchanged: the CM0+ radio core (LoRaMAC, OTAA,
SubGHz), the inter-core mailbox, the UART command server, RTC helpers, event
log, and watchdog. Contributed by this project: the sensor subsystem and its
drivers, the sensor-set configuration language, STOP2 sleep, flash-backed
identity and configuration, the offline log, and the measurement scheduler.
The Raspberry-Pi power management, recording timetable and acoustic relay of
the parent project were removed entirely.

---

## 2 · Background

**LoRaWAN Class A** is the lowest-power device class: a node may transmit at
will (duty-cycle limited), but can only *receive* in two short windows following
its own uplink. Every design choice about downlinks in this project — queued
configuration, the post-transmit awake window — follows from that asymmetry.
The network side is **The Things Network** on **AU915 FSB2** (New Zealand);
addressing, encryption and delivery are the network's business: every device
holds a unique DevEUI and per-session keys, so a downlink physically cannot be
decrypted by the wrong node (§6.4).

**The STM32WL55** is a dual-core system-on-chip: a Cortex-M4 for the
application and a Cortex-M0+ bonded to a sub-GHz radio. The two communicate
here through a 1 KB shared-memory mailbox with sequence-number handshakes
(§3.2). The die has **no USB peripheral** — a fact that shaped the data-
retrieval design (§8) and is worth stating early because the development
board's USB connector (which belongs to the ST-LINK programmer) invites the
opposite assumption.

---

## 3 · System architecture

### 3.1 Overview

```
 sensors ──► CM4: sample → pack → log → mailbox ──► CM0+: LoRaWAN ──► TTN
                 ▲                                        │
                 └—— config string / binary commands ◄────┘  (downlink)
```

CM4 owns every sensor peripheral, the scheduler, persistence and the console.
CM0+ owns only the radio. Full diagrams: LOGBOOK
[Figure 1](LOGBOOK.md#11-what-the-node-does) and
[Figure 3](LOGBOOK.md#61-dual-core-split).

### 3.2 The inter-core mailbox

A fixed `struct` at `0x2000FC00` (SRAM2), byte-identical in both firmwares.
Four channels: uplink command (CM4→CM0+, committed by sequence-number bump),
trace ring (CM0+→CM4, so one serial port shows both cores), downlink ring
(CM0+→CM4 — **every** received downlink is stored, a property that had to be
restored after inherited demo code was found swallowing two ports, LOGBOOK r7),
and OTAA key provisioning with an apply-and-rejoin handshake.

### 3.3 Sensor subsystem

One umbrella call fans out to per-sensor drivers and merges results into a
single reading structure plus a status byte: one OK-bit per sensor, one fault
bit. The contract that matters: **a failed or deselected sensor never aborts
the frame** — its fields carry sentinels and its OK-bit stays clear, so the
decoder renders "no data" rather than a plausible zero. Deselected ≠ failed:
only a *failed* read raises the fault bit.

---

## 4 · Hardware design

### 4.1 Sensor complement

| Channel | Sensor | Interface | Key design fact |
|---|---|---|---|
| Air T/RH/P ×2 | 2 × Bosch BME280 | I²C2 (shield) + I²C1 (board pins) | both answer at 0x76/0x77 → **two buses**, not straps |
| Leaf wetness | Decagon LWS (METER PHYTOS 31) | ADC, A0 | **dielectric**, not resistive; ratiometric output 10–50 % of excitation; wants *pulsed* excitation |
| Soil moisture | analog probe | ADC, A1 | raw counts on air; curve applied off-node |
| Soil temperature | PT1000 + **~900 Ω divider** | ADC (A2/PA10) | ratiometric off the 3V3 reference, same CVD math; MAX31865 dropped (LOGBOOK r18), divider live-verified 19.5–20.4 °C (r20). A2 doubles as I²C1 SDA — `T2` unusable while fitted |
| Wind speed | Davis 7911 contact | ADC, A4 (burst-sampled) | 1 Hz = 2.25 mph from the datasheet; see §7.3 |
| Wind direction | Davis 7911 vane pot | ADC, A3 | 20 kΩ linear, 0 Ω = north; 1 MΩ dead-band pull-down |
| Rainfall | tipping bucket | EXTI, D3 | edge-counted; the one sensor that forbids sleep |
| Battery | INA219 @0x45 | I²C2 | measures current *and* voltage; replaced a leaky divider |

Authoritative pin map and the assembly bill: [PINOUT.md](PINOUT.md), LOGBOOK
[§3.4](LOGBOOK.md#34-component-and-placement-summary-the-assembly-bill).

### 4.2 Design decisions with teeth

Three hardware findings cost real debugging time and are recorded so nobody
pays twice (full register: LOGBOOK [§15](LOGBOOK.md#15-decision-register)):

- **The board manual mislabels the I²C bus.** UM2592 marks D14/D15 as "I2C1";
  on this MCU those pins are **I²C2**. Everything on the Grove shield's I²C
  sockets is therefore I²C2 (D-02, [R4]).
- **Excitation is gated on the high side only** (P-MOSFET or NPN+PNP on D4).
  The obvious single-NPN low-side switch lifts sensor grounds by V_ce(sat) —
  ~150 ADC counts, when the LWS wet threshold sits ~3 % above dry. It would
  read permanently wet (D-19).
- **A wrong generic constant under-read wind 3×.** The inherited
  `ANEMO_MS_PER_HZ = 0.34` placeholder survived until the 7911 datasheet's
  `V = P(2.25/T)` was actually read: the correct value is 1.00584 m/s per Hz
  (LOGBOOK r9).

---

## 5 · Firmware design

### 5.1 The sensor-set configuration language

The single runtime configuration surface is an ASCII string in braces —
`{LW,T1,T2,15}` — accepted identically from the console and from any LoRaWAN
downlink whose first byte is `{`. Grammar, aliases (`ALL`/`NONE`), incremental
edits (`+R`, `-WD`), the 1–999 minute interval and the query form `{?}` are
specified in [CONFIG.md](CONFIG.md). Two properties are load-bearing:

- **All-or-nothing.** One bad token rejects the whole frame and writes nothing.
  A half-applied configuration on a node a day's drive away is strictly worse
  than a rejected one (D-09).
- **Canonical echo.** Every accepted string is echoed back *rendered from the
  stored configuration*, so what the operator reads is proof of what the node
  holds, not an optimistic copy of the input.

### 5.2 Persistence and identity

Flash pages are partitioned by blast radius (D-07): configuration (page 62) and
OTAA identity (page 63) are **separate** pages because a page must be erased to
be rewritten, and a routine config change must never be able to take the
AppKey with it. Identity resolves in order: RTC backup registers (fast, free to
write) → flash (survives power loss) → a compiled-in placeholder
(`envnode_identity.c`), so a virgin board joins and is visible rather than
sitting silent (D-15). Flash is written only on explicit command and only when
the value changed — an erase stalls the *other* core's instruction fetches for
20–40 ms, so writes never happen on a timer or near an RX window.

### 5.3 Sleep

Between cycles the CM4 enters **STOP2**, woken by the RTC. Three hazards are
handled explicitly (D-12, D-13, D-14): the independent watchdog keeps running
in STOP2, so sleep is taken in 8-second chunks with a refresh between; the HAL
tick freezes, so elapsed sleep is added back on wake (no deadline drift —
verified on hardware to the millisecond); and edge-counted sensors are
incompatible with a stopped core, so selecting rainfall blocks sleep and the
node says so. A ~10 s awake window after each transmission covers the Class A
receive windows and gives a console operator a hand-hold each cycle.

### 5.4 The measurement cycle

Wake → sample selected sensors → pack the 32-byte frame → **append to the
offline log** → hand to the radio → awake window (downlinks drained, commands
served) → sleep. The log append is deliberately *before* radio involvement: a
node that never joined still records (§8), and the radio is guaranteed idle
during the occasional log page erase. Rain totals are cleared only after the
radio confirms transmission, so a refused uplink cannot destroy an interval's
rainfall (D-17).

---

## 6 · Communication protocol

### 6.1 Uplink

FPort 1, fixed 32 bytes (fmt 0x02 — battery voltage and current always aboard), little-endian scaled integers, one frame per cycle;
sentinels for absent data; battery always present. Byte-exact layout and the
TTN JavaScript decoder: [PAYLOAD.md](PAYLOAD.md). A planned revision (fmt
0x01→0x02) appends a u16 node id at offset 30 (§10).

### 6.2 Downlink

Two encodings coexist: the configuration string (any FPort, self-describing by
its leading `{`) and a compact binary command table on FPort 10 (interval,
calibration offsets, vane offset, sensor mask, reboot, one-shot uplink). Every
applied downlink is echoed on the console. Worked examples with exact hex and
base64 payloads: LOGBOOK [§9.4](LOGBOOK.md#94-downlink-cookbook--what-to-paste-into-ttn).

### 6.3 Console

The ST-LINK virtual COM port (115200 8N1) carries a command server whose
reference the firmware itself prints (`nucleo list message syntax`); the same
brace strings work verbatim. Command inventory: LOGBOOK
[§9.1](LOGBOOK.md#91-console-reference).

### 6.4 Addressing

Delivery to a specific node is LoRaWAN's job — unique DevEUI (derived from the
chip's factory UID), per-session encryption, unicast downlinks. What the
protocol does not give — a human-usable node number, self-describing exported
frames, an interlock against pasting a config into the wrong device — is the
planned node-id feature (§10).

---

## 7 · Power management

### 7.1 Philosophy

Energy is spent only on demand: sensors are excited for milliseconds per cycle,
the radio transmits one frame per interval, the core sleeps in between, and
flash is written only when something changed. Estimates below are design-time
figures; **no current profile has been measured yet** — that measurement is the
top open bench item.

### 7.2 The switched sensor rail

The LWS draws ~4 mA continuously and its manufacturer requires pulsed
excitation anyway; the vane pot adds 165 µA. Both hang on **VSENS**, a
GPIO-gated high-side rail: on → 15 ms settle → convert → off. Roughly
101 mAh/day becomes ~0.002 (D-19; circuits in [PINOUT.md](PINOUT.md)).

### 7.3 The wind-speed lesson

The interesting result: the expensive part of the anemometer was never its
electricity but its *interrupts*. Edge-counting wind pinned the core awake all
interval (~50–60 mAh/day estimated) — ten times the sensor's own consumption.
Wind speed is therefore **burst-sampled**: the ADC reads the contact at ~1 kHz
for one 3-second WMO gust window per cycle and transitions are counted in
software (D-21). The trade is honest: 3 s of wind per cycle, gust ≡ mean, and
the upgrade path (EXTI wake-from-STOP2 with an RTC/LPTIM time base) is recorded
if the field data proves too coarse. Rainfall stays on interrupts because tips
are rare and must never be missed.

### 7.4 Battery sensing

An INA219 on the shield bus (0x45, 0.1 Ω shunt) measures voltage *and* current,
so charge/discharge direction and coulomb counting come free; the resistor
divider it replaced leaked 184 µA continuously and measured only voltage
(D-20).

---

## 8 · Data management

Every cycle's frame is appended, verbatim and RTC-timestamped, to a flash ring
(pages 59–61; 153 records — shrunk from 7 pages/357 to make room for FatFs;
oldest page recycles when full — roughly a day at a 10-minute interval, scaling
linearly with the interval). The log is the *transmitted bytes*, so log and
radio cannot disagree; decoding happens once, at dump time. Retrieval is
`nucleo log dump` → CSV over the same USB cable that powers the board; torn
writes fail checksum and are skipped.

The WL55's lack of USB (§2) closes the "node as USB drive / node reads a USB
stick" path permanently. The removable medium is an **SD card on SPI1**, now
in service: FatFs writes a daily `YYYYMMDD.CSV` (one row per cycle, synced per
row) and a `CONFIG.INI` on the card can provision the LoRaWAN identity in the
field. The card was brought up the hard way — SPI mode, a dead card, missing
decoupling and a chip-select wire soldered one header pin off (found
electrically, `nucleo cshunt`) — the full account is LOGBOOK r18. The flash
ring remains the card-failed fallback
(LOGBOOK [§12A](LOGBOOK.md#12a-future-functionality--sd-card-mass-logging)).

---

## 9 · Verification

The claim discipline of this project: a thing is *verified* when it ran on the
board and its output was recorded, and *pending* otherwise.

**Verified on hardware** (transcripts in LOGBOOK §14, r6–r12): boot and config
restore from flash; STOP2 sleep and RTC wake with zero schedule drift; the
config string end-to-end including all-or-nothing rejection; the 13-vector
parser self-test and byte-exact packer self-test; AU915 transmission timing;
LW reading live on A0; the sleep predicate correctly naming rain, and only
rain, after the wind rework; reserved flash pages surviving re-flash.

**Pending hardware**: both BME280s and the INA219 responding on their buses
(nothing answers yet — suspected shield seating/selector, LOGBOOK r8); the LWS
wet/dry response; the 7911 spin and vane sweep; the offline log dump (built and
pushed while the board was unplugged); any current measurement; TTN join and
downlink (needs a registered key and a gateway in range).

The self-test (`nucleo selftest`) exists precisely to separate "wiring wrong"
from "firmware wrong" before either is suspected — it earned its keep by
catching a wrong byte in its own expected-frame vector.

---

## 10 · Limitations and future work

| Item | State |
|---|---|
| Bench calibration of every channel (Table 12 of LOGBOOK) | blocked on sensors responding |
| Current profile (sleep / sample / TX) | not measured — top open item |
| Node id: u16, UID-defaulted, in the identity page, on-air at offset 30 (fmt 0x02), `#N` config targeting | designed (D-10), not implemented |
| FPort-2 diagnostic uplink; `get_config` (0x08) | not implemented |
| SD-card mass logging | driver done; FAT layer + flash budget per LOGBOOK §12A |
| EXTI wake-from-STOP2 wind counting (true interval mean + sleep) | upgrade path, needs RTC/LPTIM pulse timing |
| RTC coin cell (remove SB21) | planned; firmware already prefers backup registers |
| TTN fair use | 1-min default is a bench setting — raise to ≥15 min for deployment |
| Gust ≡ mean under burst sampling | accepted trade; revisit with field data |

## 11 · References

Full citation table with local-copy filenames: LOGBOOK
[§16](LOGBOOK.md#16-references) ([R1]–[R12]: UM2592, mbed board file, CubeMX
device database, ST community erratum, BME280 datasheet, MAX31865 datasheet,
IEC 60751, Grove shield wiki, RM0453, TTN documentation, METER/Decagon LWS
manuals, Davis DS7911).

## Appendix — document map

| Read | For |
|---|---|
| this file | the narrative: what, why, how well |
| [LOGBOOK.md](LOGBOOK.md) | procedures, bench records, dated history, replication |
| [PINOUT.md](PINOUT.md) · [PAYLOAD.md](PAYLOAD.md) · [CONFIG.md](CONFIG.md) | normative specs |
| [SENSORS.md](SENSORS.md) · [ARCHITECTURE.md](ARCHITECTURE.md) · [ROADMAP.md](ROADMAP.md) | per-sensor detail, structure, plan |
| [../CLAUDE.md](../CLAUDE.md) | working rules for contributors |
