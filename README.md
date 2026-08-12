# EnviroNode-WL55

**A solar/battery agrometeorological sensor node on the STM32WL55JC, reporting
over LoRaWAN (bidirectional).** Every interval it samples a full weather-and-soil
picture, packs a fixed 30-byte frame and uplinks it to The Things Network on
FPort 1. It is reconfigured remotely: which sensors run and how often is one
ASCII string in braces, sent as a downlink or typed on the ST-Link console.

Built on the proven **dual-core WL55JC1 + LoRaWAN** platform from the KoreroNet 2
acoustic node — the radio core, OTAA provisioning, key persistence, serial
command server, event log and watchdog are reused. The application layer is new:
environmental sensors, no acoustic payload, **no Raspberry Pi and no recording
timetable**.

---

## What it measures

| Config key | Quantity | Sensor | Interface |
|---|---|---|---|
| `T1` / `T2` | Air temp / humidity / pressure ×2 | 2× **BME280** | **I²C2** (Grove shield) and **I²C1** (board pins) |
| `SM` | Soil moisture | **Decagon 10HS** (capacitance/FDR) | ADC (A1) |
| `LW` | Leaf wetness | **Decagon LWS** (dielectric) | ADC (A0) |
| `WD` | Wind direction | **Davis 7911** vane potentiometer | ADC (A3) |
| `ST` | Soil temperature | **PT1000** RTD via MAX31865 | *dropped from this node — driver kept* |
| `R` | Rainfall | tipping bucket | GPIO pulse count (EXTI) |
| `WS` | Wind speed + gust | **Davis 7911** contact closure | ADC burst-sampled (A4) |
| — | Battery voltage | INA219 | I²C — always sent |

## Configuring it

One string, over a LoRaWAN downlink (any FPort, first byte `{`) or the console
(`nucleo set {…}`, or a bare `{…}` line):

```
{ALL,15}          every sensor, 15-minute cycle
{T1,T2,ST,60}     air x2 + soil temp, hourly
{+R}              add rainfall to whatever is already selected
{-LW,-WD}         drop leaf wetness and wind direction
{5}               keep the set, cycle every 5 minutes
{?}               report the current configuration
```

A frame is applied in full or rejected in full, and the sensor set + interval are
persisted in flash. Full reference → **[docs/CONFIG.md](docs/CONFIG.md)**.

Full pin/peripheral map → **[docs/PINOUT.md](docs/PINOUT.md)**.
On-air byte formats (uplink + downlink) → **[docs/PAYLOAD.md](docs/PAYLOAD.md)**.
Sensor bring-up / calibration → **[docs/SENSORS.md](docs/SENSORS.md)**.
System design → **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.
Plan of work → **[docs/ROADMAP.md](docs/ROADMAP.md)**.

---

## Repository layout

```
EnviroNode-WL55/
├── CM4/EnviroNode_CM4/          # Cortex-M4 application (sensors, sampling, uplink)
│   └── Core/WL55JC1/…           #   ← app source: drivers, config/keystore, main.c
├── CM0/EnviroNode_CM0PLUS/      # Cortex-M0+ LoRaWAN radio core (reused ~as-is)
├── docs/                        # PINOUT, PAYLOAD, CONFIG, SENSORS, ARCHITECTURE, ROADMAP
└── CLAUDE.md                    # context to continue the build in a fresh session
```

Two independent STM32 projects (one per core) flashed to a single WL55JC:
CM4 @ `0x08000000`, CM0+ @ `0x08020000`. They share a 1 KB mailbox in SRAM2.

---

## Build

Requires **STM32CubeCLT** (CMake + Ninja + arm-none-eabi-gcc). From
`CM4/EnviroNode_CM4/`:

```powershell
.\flash.ps1 -Build -NoFlash     # build BOTH cores, don't flash
.\flash.ps1 -Build              # build + flash both cores over ST-Link
```

The build is currently **green** (both cores link). See ROADMAP for what the
firmware does / doesn't do yet.

> **Flashing note (inherited):** a board at **RDP Level 1** or with a flaky NRST
> won't take a connect-under-reset flash — use **HotPlug** and, if read-protected,
> regress RDP on **ST-Link-USB-only power**. This is documented at length in the
> KoreroNet manual and applies identically here.

---

## Project status

🟢 **Phases 0–4 (largely done):** peripherals up (hand-written init — there is no
`.ioc`), all four sensor drivers written, sampling scheduler + 30-byte FPort-1
uplink, FPort-10 binary command table, the `{…}` sensor-set config string, and
OTAA identity + node config persisted in flash so they survive a power cut.

Also done since: **STOP2 sleep** with RTC wake (verified on hardware), the
**offline timestamped sensor log** (`nucleo log dump` = CSV), a switched sensor
excitation rail, Davis 7911 wind by ADC burst sampling, and a dormant SD-card
driver. Start reading at **[docs/MASTER.md](docs/MASTER.md)** (narrative) and
**[docs/LOGBOOK.md](docs/LOGBOOK.md)** (build log + replication manual).

🟡 **Open:** the FPort-2 diagnostic uplink, current-draw measurement, and
bench-testing every driver against real sensors.
See **[docs/ROADMAP.md](docs/ROADMAP.md)**.

> `CM4/EnviroNode_CM4/Core/WL55JC1/Src/main.c` still carries the inherited
> KoreroNet infrastructure — clock, UART command server, RTC, mailbox, OTAA, event
> log, IWDG — which is deliberate reuse. The acoustic, Raspberry-Pi-power and
> recording-timetable logic has been removed; this node has none of those.
