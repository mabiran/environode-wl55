# EnviroNode-WL55 — Roadmap

Phased plan from the current scaffold to a field-ready node. Keep the build green
at every step (`flash.ps1 -Build -NoFlash`).

## Phase 0 — Scaffold ✅ (done)
- [x] New repo from the WL55 dual-core + LoRaWAN skeleton (KoreroNet migration).
- [x] Clean rename `KN-1.1_*` → `EnviroNode_*`; **both cores build green**.
- [x] Design ground truth: `docs/PINOUT.md`, `docs/PAYLOAD.md`, architecture.
- [x] `CLAUDE.md` for session continuity; private GitHub repo + push.

## Phase 1 — Peripheral bring-up (CubeMX)
- [ ] Create/refresh the `.ioc`: enable **I²C1, I²C2, SPI1, ADC** (4 channels),
      **2× EXTI** (rain, wind-speed) or a TIM counter, **LPTIM/RTC** wake.
- [ ] Lock the GPIO map in `docs/PINOUT.md` to the real board + shield.
- [ ] Regenerate HAL init; confirm the build stays green.

## Phase 2 — Sensor drivers
- [ ] `bme280` (I²C, used on both buses) — read T/RH/P, compensation.
- [ ] `max31865` (SPI) — PT1000, 4.02 kΩ Rref, 2/3/4-wire, fault flags.
- [ ] `analog` — ADC read + scaling for soil-moisture, leaf-wetness, battery, wind-dir.
- [ ] `pulse_counter` — debounced rain tips + wind-speed frequency (ISR-driven).
- [ ] Per-driver bench test over the UART console before integration.

## Phase 3 — Application (replace inherited CM4 logic)
- [ ] New sampling scheduler (RTC/LPTIM periodic wake; default 15-min interval).
- [ ] Payload pack → 30-byte FPort-1 frame per `docs/PAYLOAD.md`.
- [ ] Hand-off to CM0+ mailbox; verify uplink on TTN with the JS decoder.
- [ ] Strip the acoustic/Pi-power code paths; keep clock/UART/RTC/mailbox/OTAA/
      event-log/IWDG infrastructure.

## Phase 4 — Downlink & config
- [ ] Wire the FPort-10 command table (`set_interval`, `set_cal`, `reset_rain`, …).
- [ ] Persist config in RTC backup registers; re-apply on boot.
- [ ] Diagnostic uplink (FPort 2): reset cause, uptime, per-sensor error counts.

## Phase 5 — Power & robustness
- [ ] Gate sensor rails; measure average draw; tune the interval.
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
