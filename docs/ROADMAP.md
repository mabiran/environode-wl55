# EnviroNode-WL55 — Roadmap

Phased plan from the current scaffold to a field-ready node. Keep the build green
at every step (`flash.ps1 -Build -NoFlash`).

## Phase 0 — Scaffold ✅ (done)
- [x] New repo from the WL55 dual-core + LoRaWAN skeleton (KoreroNet migration).
- [x] Clean rename `KN-1.1_*` → `EnviroNode_*`; **both cores build green**.
- [x] Design ground truth: `docs/PINOUT.md`, `docs/PAYLOAD.md`, `docs/CONFIG.md`,
      architecture.
- [x] `CLAUDE.md` for session continuity; private GitHub repo + push.

## Phase 1 — Peripheral bring-up ✅ (done, hand-written — no `.ioc`)
- [x] **I²C1** (PA9/PA10), **I²C2** (PA12/PA11), **SPI1** (PA5/6/7 + CS PA4),
      **ADC** ×4 (PB1/PB2/PB4/PB14), **EXTI3 + EXTI5** (rain PB3, wind PB5).
- [x] GPIO map locked in `docs/PINOUT.md` against UM2592 Table 17 + the mbed
      NUCLEO_WL55JC board file (which also corrects UM2592's "I2C1" mislabel on
      D14/D15 — those pins are I²C2).
- [x] HAL SPI module enabled and its driver sources added; build stays green.
- [ ] LPTIM (only needed for the Phase 5 low-power wake).

## Phase 2 — Sensor drivers ✅ (done)
- [x] `bme280` — chip probe, calibration load, forced-mode read, full Bosch
      compensation; auto address probe (0x76 → 0x77); one instance per bus.
- [x] `max31865` — PT1000, 4.02 kΩ Rref, 2/3/4-wire, 50 Hz reject, bias-on →
      one-shot → bias-off per read, CVD + sub-zero polynomial, fault register.
- [x] `analog_sensors` — 4 channels averaged ×8, divider/vane scaling.
- [x] `pulse_counter` — split rain/wind debounce, atomic snapshot, 3 s gust
      buckets, peek-vs-consume so the console can't steal an interval.
- [ ] Per-driver bench test on real hardware (needs the sensors wired).

## Phase 3 — Application ✅ (mostly done)
- [x] Sampling scheduler: first frame ~60 s after boot, then every configured
      interval (default 15 min); retries in 60 s when the radio refuses.
- [x] Payload pack → 32-byte FPort-1 frame (fmt 0x02, batt mA r22) per `docs/PAYLOAD.md`.
- [x] Hand-off to CM0+ mailbox (CM0+ honours `mb->port`, so FPort 1 is used).
- [ ] Verify on TTN with the JS decoder (needs hardware + gateway).
- [x] AudioMoth paths removed.
- [x] **Pi-power / recording-timetable / power-history code removed from
      `main.c`.** No Raspberry Pi, no audio, no timetable anywhere in this
      project; the brace syntax now belongs to the sensor-set config string
      (`docs/CONFIG.md`). This frees **PB10 (D6)** and **PC1 (D7)**.

## Phase 4 — Downlink & config ✅ (mostly done)
- [x] FPort-10 command table wired: `set_interval`, `uplink_now`, `reset_rain`,
      `set_cal`, `set_winddir_offset`, `set_enable`, `reboot`. Downlinks are
      drained and applied automatically from the main loop.
- [x] **Sensor-set configuration string** (`docs/CONFIG.md`) — one ASCII frame in
      braces selects which sensors run and the cycle interval, over a downlink on
      any FPort (first byte `{`) or over the console (`nucleo set {…}` / bare
      `{…}`). Replaces the inherited timetable. All-or-nothing validation; the
      mask + interval share the existing flash config page; `0x06 set_enable`
      writes the same 8-bit mask.
- [x] Config persisted in a dedicated flash page and re-applied on boot; the
      OTAA identity is mirrored to flash so it survives a full power loss.
- [ ] `get_config` (0x08) + the FPort-2 diagnostic uplink: reset cause, uptime,
      per-sensor error counts. `get_config` returns ENV_NOTIMPL until then, and a
      `{?}` arriving by downlink is answered on the console only.

## Phase 5 — Power & robustness
- [x] **STOP2 sleep + RTC wake — done and verified on hardware** (`envnode_power.c`):
      8 s IWDG-safe chunks, tick catch-up, `nucleo sleep on|off`. Only `R`
      blocks sleep; `WS` became ADC burst-sampled precisely so it doesn't.
- [x] Sensor rails gated: VSENS on D4/PB5 (LWS + vane), 15 ms settle, high-side
      switch only (docs/PINOUT.md).
- [x] Offline timestamped log in flash (153 records, `nucleo log dump` CSV);
      SD CSV logging **live** — FatFs, daily files, CONFIG.INI provisioning,
      CS on D5/PB8 (LOGBOOK §12A + r18).
- [ ] Measure actual average draw; tune the interval. **Top open bench item.**
- [ ] Low-battery behavior (longer interval / alarm uplink).
- [ ] Confirm IWDG coverage across the sample→uplink→sleep cycle.

## Phase 6 — Field
- [ ] TTN application + payload formatter; dashboard/integration.
- [ ] Calibration curves (soil moisture, leaf wetness, wind-dir mapping).
- [ ] Enclosure, sensor mounting, solar sizing; deploy + monitor.

---
### Open decisions to confirm with hardware
- Exact ADC channel↔pin mapping (datasheet) and whether wind-speed uses EXTI vs a
  TIM external counter.
- BME280 vs BME680 (gas channel) — currently **BME280**.
- Soil-moisture probe type + calibration curve (raw ADC vs permille on-air).
- Regional LoRaWAN params (AU915 FSB2 assumed — match the gateway/TTN).
