# EnviroNode-WL55

**A solar/battery agrometeorological sensor node on the STM32WL55JC, reporting
over LoRaWAN (bidirectional).** It periodically measures a full weather-and-soil
picture and uplinks a compact frame to The Things Network; it accepts downlink
commands for configuration and calibration.

Built on the proven **dual-core WL55JC1 + LoRaWAN** platform from the KoreroNet 2
acoustic node — the radio core, OTAA provisioning, backup-register persistence,
serial command server, event log and watchdog are reused; the application layer
is new (environmental sensors instead of an acoustic payload).

---

## What it measures

| Quantity | Sensor | Interface |
|---|---|---|
| Air temp / humidity / pressure ×2 | 2× **BME280** | **I²C1** and **I²C2** (separate buses) |
| Soil moisture | analog probe | ADC |
| Leaf wetness | resistive grid | ADC |
| Battery voltage | divider | ADC |
| Wind direction | vane potentiometer | ADC |
| Soil temperature | **PT1000** RTD | **MAX31865** over SPI |
| Rainfall | tipping bucket | GPIO pulse count |
| Wind speed | anemometer | GPIO pulse count |

Full pin/peripheral map → **[docs/PINOUT.md](docs/PINOUT.md)**.
On-air byte formats (uplink + downlink) → **[docs/PAYLOAD.md](docs/PAYLOAD.md)**.
System design → **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.
Plan of work → **[docs/ROADMAP.md](docs/ROADMAP.md)**.

---

## Repository layout

```
EnviroNode-WL55/
├── CM4/EnviroNode_CM4/          # Cortex-M4 application (sensors, sampling, uplink)
│   └── Core/WL55JC1/…           #   ← app source; main.c is the inherited base (see below)
├── CM0/EnviroNode_CM0PLUS/      # Cortex-M0+ LoRaWAN radio core (reused ~as-is)
├── docs/                        # PINOUT, PAYLOAD, ARCHITECTURE, ROADMAP, SENSORS
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

🟢 **Phase 0 — scaffold (done):** repo created; WL55 dual-core + LoRaWAN skeleton
migrated and building green under the new name; design specs written.

🟡 **Next:** finalize peripherals in CubeMX, add the sensor drivers, replace the
inherited CM4 application with the sensor-sampling + payload logic, wire the
downlink command table. See **[docs/ROADMAP.md](docs/ROADMAP.md)**.

> **Heads-up:** `CM4/EnviroNode_CM4/Core/WL55JC1/Src/main.c` is still the
> **inherited KoreroNet application** (kept so the project builds and to reuse its
> infrastructure — clock, UART command server, RTC, mailbox, OTAA, event log,
> IWDG). Its acoustic/Pi-power logic will be progressively replaced by the
> EnviroNode sensor logic. It is clearly marked at the top of the file.
