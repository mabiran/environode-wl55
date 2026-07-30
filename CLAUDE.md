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
Which sensors run, and how often, is set by one ASCII **sensor-set config string**
in braces — `{T1,T2,ST,60}` — over downlink or console (`docs/CONFIG.md`).

## Origin / reuse
Migrated from the **KoreroNet 2** acoustic node (same WL55JC1 + LoRaWAN platform),
which lives at `../KN-1.1/VSCODE Deploy` (firmware) — a rich, working reference.
**Reused ~as-is:** the CM0+ radio core (LoRaMAC/OTAA/SubGHz), the CM4↔CM0+ SRAM2
mailbox (`korero_mailbox.h`), OTAA key persistence in RTC backup registers, the
UART command server + `normalize_cmd` parser, the RTC/epoch helpers, the event
log (`nucleo report`), and the IWDG watchdog. **Replaced:** the acoustic /
Pi-power / recording-timetable application logic in CM4 `main.c` — gone, with the
sensor-set config string in place of the timetable.

## Ground-truth design (do not drift from these)
- `docs/LOGBOOK.md` — **the living build logbook + replication manual.** Entry
  point for any reader; updated on every change (see Working rules).
- `docs/PINOUT.md`  — sensor→peripheral map + GPIOs (**LOCKED**, hand-written init).
- `docs/PAYLOAD.md` — uplink frame (FPort 1, 30 bytes) + downlink command table (FPort 10).
- `docs/CONFIG.md`  — the `{…}` sensor-set / interval config string (SPEC v1):
  grammar, keys, replace-vs-edit, all-or-nothing rejection, canonical rendering.
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

## State (Phases 1–2 done, 3–4 largely done)
- [x] Repo scaffolded from the WL55 dual-core skeleton; renamed KN-1.1_* → EnviroNode_*; **builds green**.
- [x] Design specs written (PINOUT, PAYLOAD); **pin map now LOCKED and verified**
      against UM2592 Table 17 + the mbed board file (no `.ioc` — the peripheral
      init is hand-written in `i2c.c` / `spi.c` / `adc.c` / `gpio.c`, which is
      why CubeMX is not needed; regenerating an `.ioc` would overwrite these).
- [x] Peripherals up: **I²C1** PA9/PA10 (BME280 #2, board pins), **I²C2** PA12/PA11
      (BME280 #1 via Grove shield + INA219), **SPI1** PA5/6/7 + CS PA4 (MAX31865),
      **ADC** PB1/PB2/PB4/PB14, **EXTI3/EXTI5** PB3/PB5 (rain/wind).
- [x] Sensor drivers implemented: `bme280` (Bosch compensation, forced mode),
      `max31865` (PT1000, one-shot + bias off, CVD + sub-zero), `analog_sensors`,
      `pulse_counter` (debounce, atomic snapshot, 3 s gust buckets).
- [x] CM4 app samples sensors, packs the 30-byte FPort-1 frame and uplinks on a
      configurable interval (default 15 min); console: `info`, `nucleo sensors`,
      `nucleo uplink now`, `nucleo set {…}` / bare `{…}`, `nucleo interval <min>`,
      `nucleo reset rain`.
- [x] Downlink command table (FPort 10) wired to the CM0+ ring and auto-applied.
- [x] **Sensor-set config string** (`docs/CONFIG.md`): `{LW,T1,T2,SM,ST,WS,WD,R,15}`
      — downlink on any FPort whose first byte is `{`, or a console line. Selects
      the sensor set + interval, all-or-nothing validation, persisted in the same
      flash config page. It **replaced the inherited recording timetable**.
- [x] OTAA identity + node config persisted in **flash** (pages 62/63, reserved in
      the linker script) so the AppKey survives a full power loss.
- [x] Pi-power / timetable / power-history code **removed** from `main.c`
      (PB10/D6 and PC1/D7 are free). AudioMoth support is gone too.
- [ ] **Low-power sleep (STOP2 + RTC/LPTIM wake) — still to do**, still a busy
      main loop. `docs/CONFIG.md` already defines the may-sleep predicate:
      selecting `R` or `WS` (EXTI edge counting) forces the node to stay awake.
- [ ] FPort-2 diagnostic uplink (`get_config` returns ENV_NOTIMPL until then; a
      downlinked `{?}` is answered on the console only).

## Non-volatile layout (CM4 flash, reserved in STM32WL55JCIX_FLASH.ld)
`FLASH` is declared as **124K** so these two pages are never used by code:
| Page | Address | Contents | Module |
|---|---|---|---|
| 62 | `0x0801F000` | interval, calibration offsets, **sensor-set mask**, vane offset | `envnode_config.c` |
| 63 | `0x0801F800` | AppKey / DevEUI / JoinEUI | `envnode_keystore.c` |
Backup registers are still the fast path for the keys; flash is the fallback that
survives power loss (VBAT rides VDD on this board). `flash.ps1` only erases the
sectors the ELF covers, so re-flashing the app keeps both pages.

## Working rules
- **Update `docs/LOGBOOK.md` on EVERY change — this is not optional.** It is the
  living catalogue: someone with the parts and that file alone must be able to
  duplicate this node. In the same edit as the change itself: update the affected
  section, add a dated entry to its Build log (what changed, why, what it means
  for a replicator), record any new decision in its Decision register, bump the
  revision, then **verify it** (anchors resolve, companion links resolve, every
  quoted pin/address/constant/command still matches its source file, figure and
  table numbering contiguous). Do not defer the logbook to "later".
- Keep the build green after every change (`flash.ps1 -Build -NoFlash`).
- **No AudioMoth, no audio, no Raspberry Pi, no recording timetable** in this
  project — it is sensors + bidirectional LoRaWAN. That code has been removed;
  never reintroduce it, and treat any surviving fragment as removal work.
- **Braces mean the sensor-set config string** (`docs/CONFIG.md`), not a
  timetable. Parse it from the **raw** console line: `normalize_cmd()` strips
  commas *and* `?` and lowercases everything, so it destroys the syntax.
- `main.c` is the inherited KoreroNet base — replace incrementally, don't rip out
  the reusable infrastructure (clock/UART/RTC/mailbox/OTAA/event-log/IWDG).
- Peripheral init is **hand-written**, not CubeMX-generated. Don't introduce an
  `.ioc` without porting `i2c.c`/`spi.c`/`adc.c`/`gpio.c` by hand afterwards.
- Flash writes (config/key save) stall the CM0+ core's instruction fetches for
  the duration of the erase (~20–40 ms), so only ever write on an explicit
  command — never on a timer or inside a LoRaWAN RX window.
- Match TTN region settings (KoreroNet used **AU915 FSB2**) to your gateway.
- Do **not** modify the sibling KoreroNet repo from here.
