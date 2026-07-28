# CLAUDE.md — EnviroNode-WL55

Context for continuing this build in a fresh Claude Code session opened at this
repo root. Read this first, then `docs/`.

## What this is
A new IoT **agrometeorological sensor node** on **STM32WL55JC** over **LoRaWAN**,
bidirectional. It periodically measures: air temp/humidity/pressure ×2 (2× BME280
on **two separate I²C buses**), soil moisture (ADC), leaf wetness (ADC), battery
voltage (ADC), wind direction (vane potentiometer, ADC), soil temperature
(**PT1000 via MAX31865 SPI**), rainfall (tipping-bucket pulse count), and wind
speed (anemometer pulse count). Uplinks a compact frame; accepts downlink config.

## Origin / reuse
Migrated from the **KoreroNet 2** acoustic node (same WL55JC1 + LoRaWAN platform),
which lives at `../KN-1.1/VSCODE Deploy` (firmware) — a rich, working reference.
**Reused ~as-is:** the CM0+ radio core (LoRaMAC/OTAA/SubGHz), the CM4↔CM0+ SRAM2
mailbox (`korero_mailbox.h`), OTAA key persistence in RTC backup registers, the
UART command server + `normalize_cmd` parser, the RTC/epoch helpers, the event
log (`nucleo report`), and the IWDG watchdog. **To replace:** the acoustic/Pi-power
application logic in CM4 `main.c`.

## Ground-truth design (do not drift from these)
- `docs/PINOUT.md`  — sensor→peripheral map + proposed GPIOs (finalize in CubeMX).
- `docs/PAYLOAD.md` — uplink frame (FPort 1, 30 bytes) + downlink command table (FPort 10).
- `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `docs/SENSORS.md`.

## Build / verify
STM32CubeCLT toolchain. From `CM4/EnviroNode_CM4/`:
```
.\flash.ps1 -Build -NoFlash      # build both cores (CM0+ then CM4), no flash
.\flash.ps1 -Build               # build + flash both over ST-Link (HotPlug for RDP1/flaky-NRST boards)
```
CMake project/target names are `EnviroNode_CM4` / `EnviroNode_CM0PLUS`. The build
is currently green. `flash.ps1` uses connect-under-reset by default; for RDP1 or
flaky-NRST boards use HotPlug (`-c port=SWD mode=HOTPLUG -d <elf> -rst`) and, if
read-protected, regress RDP on ST-Link-USB-only power (see KoreroNet manual).

## State (Phase 0 complete)
- [x] Repo scaffolded from the WL55 dual-core skeleton; renamed KN-1.1_* → EnviroNode_*; **builds green**.
- [x] Design specs written (PINOUT, PAYLOAD).
- [ ] Peripherals configured in CubeMX (2×I²C, ADC×4, SPI1, 2×EXTI, LPTIM) — **the `.ioc` is not yet created**.
- [ ] Sensor drivers (bme280, max31865, analog, pulse-counter) — skeletons only.
- [ ] CM4 app replaced with sensor sampling + payload packing.
- [ ] Downlink command table wired to the CM0+ downlink ring.
- [ ] Low-power sample/uplink scheduler (RTC/LPTIM wake).

## Working rules
- Keep the build green after every change (`flash.ps1 -Build -NoFlash`).
- `main.c` is the inherited KoreroNet base — replace incrementally, don't rip out
  the reusable infrastructure (clock/UART/RTC/mailbox/OTAA/event-log/IWDG).
- Match TTN region settings (KoreroNet used **AU915 FSB2**) to your gateway.
- Do **not** modify the sibling KoreroNet repo from here.
