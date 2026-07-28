# EnviroNode-WL55 — Sensor Bring-up Guide

How each measurement is wired, driven, and calibrated. Cross-reference
[PINOUT.md](PINOUT.md) (interfaces/pins) and [PAYLOAD.md](PAYLOAD.md) (on-air).
Drivers live in `CM4/EnviroNode_CM4/Core/WL55JC1/{Inc,Src}/sensors/`.

> **State:** skeletons. The pulse counter is functionally complete; the BME280,
> MAX31865 and ADC drivers have concrete APIs + register maps but stubbed bodies
> (`ENV_NOTIMPL`) to be filled in Phase 2 after the `.ioc` exists. None are wired
> into `CMakeLists.txt` yet, so the build stays green until you add them.

---

## 1–2 · Air temp / humidity / pressure — 2× BME280 (`bme280.{h,c}`)
- **Bus:** BME280 #1 on **I²C1** (`hi2c1`), #2 on **I²C2** (`hi2c2`). Two buses
  because both chips answer at `0x76`/`0x77`.
- **Config:** verify chip id `0x60`; soft-reset (`0xB6`→`0xE0`); load the
  calibration blocks (`0x88..0xA1`, `0xE1..0xF0`); `ctrl_hum` osrs_h ×1, `ctrl_meas`
  osrs_t/p ×1, **forced mode** (sample-on-demand → low power), filter off.
- **Read:** kick forced measurement → poll `status.measuring` → burst-read 8 bytes
  → Bosch compensation (`t_fine` chain).
- **Calibration:** factory coefficients on-chip; no field cal needed. Sanity-check
  the two sensors agree within tolerance.

## 3 · Soil moisture — analog probe (`analog_sensors.{h,c}`)
- **Interface:** ADC channel. High output impedance → use a **long ADC sampling
  time**.
- **On-air:** raw 12-bit counts (curve applied off-node or via `set_cal`).
- **Calibration:** two-point (air-dry vs saturated / in-water) → map counts to
  %VWC per probe type.

## 4 · Leaf wetness — resistive grid (`analog_sensors.{h,c}`)
- **Interface:** ADC channel (ratiometric). Long sampling time.
- **Calibration:** dry vs wet reference; often reported as a raw index/permille.

## 5 · Battery voltage — divider (`analog_sensors.{h,c}`)
- **Interface:** ADC via resistor divider. `Vbatt = Vadc × BATT_DIVIDER_RATIO`.
- **Set** `BATT_DIVIDER_RATIO` to your resistors; verify against a DMM.
- **On-air:** millivolts (u16), always sent.

## 6 · Wind direction — vane potentiometer (`analog_sensors.{h,c}`)
- **Interface:** ADC. `deg = counts/4095 × 360 + vane_offset`, wrapped 0–360.
- **Calibration:** `set_winddir_offset` (FPort 10) aligns the vane's electrical
  zero to true/magnetic north.

## 7 · Soil temperature — PT1000 via MAX31865 (`max31865.{h,c}`)
- **Interface:** **SPI1** (`hspi1`) + a CS GPIO; optional DRDY on EXTI.
- **Critical:** **Rref = 4.02 kΩ** for PT1000 (`MAX31865_RREF`), not 430 Ω. Set
  `MAX31865_RTD_NOMINAL = 1000`. Choose 2/3/4-wire to match the probe.
- **Config:** Vbias on, auto-convert, **50 Hz** mains reject (NZ). Let Vbias
  settle ~10 ms before the first read.
- **Convert:** ratio → `Rrtd = ratio/32768 × Rref` → Callendar–Van Dusen
  (constants `A/B` in the driver; add the ITS-90 sub-zero extension for frost).
- **Faults:** on the RTD fault bit, read/clear `max31865_read_fault()`.

## 8 · Rain — tipping bucket (`pulse_counter.{h,c}`)
- **Interface:** reed switch → GPIO **EXTI**. Debounced in `pulse_rain_isr`
  (`PULSE_DEBOUNCE_MS`).
- **Calibration:** `RAIN_MM_PER_TIP` (0.2794 mm for a 0.011″ bucket — set to yours).

## 9 · Wind speed — anemometer (`pulse_counter.{h,c}`)
- **Interface:** reed/hall → GPIO **EXTI** (or a TIM in external-counter mode for
  high rates). Debounced in `pulse_wind_isr`, which also tracks the shortest gap
  for **gust**.
- **Calibration:** `ANEMO_MS_PER_HZ` (per your anemometer's Hz→speed spec).

---

## Measurement → driver → field → payload offset

| Measurement | Driver | `sensor_readings_t` field | Uplink off (FPort 1) |
|---|---|---|---|
| Air A temp/RH/press | `bme280` (I²C1) | `air1_temp_c` / `air1_rh_pct` / `air1_press_hpa` | 4 / 6 / 7 |
| Air B temp/RH/press | `bme280` (I²C2) | `air2_temp_c` / `air2_rh_pct` / `air2_press_hpa` | 9 / 11 / 12 |
| Soil moisture | `analog_sensors` | `soil_moist_raw` | 14 |
| Leaf wetness | `analog_sensors` | `leaf_wet_raw` | 16 |
| Soil temperature | `max31865` | `soil_temp_c` | 18 |
| Wind speed / dir / gust | `pulse_counter` / `analog_sensors` | `wind_speed_ms` / `wind_dir_deg` / `wind_gust_ms` | 20 / 22 / 24 |
| Rain tips / mm | `pulse_counter` | `rain_tips` / `rain_mm` | 26 / 28 |
| Battery | `analog_sensors` | `batt_v` | 2 (always) |

Each driver sets its **status OK-bit** (`SENS_OK_*`) on a good read; the packer
substitutes the sentinel (`0x7FFF`/`0xFFFF`/`0xFF`) when a bit is clear.
