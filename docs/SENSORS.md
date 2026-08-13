# EnviroNode-WL55 — Sensor Bring-up Guide

How each measurement is wired, driven, and calibrated. Cross-reference
[PINOUT.md](PINOUT.md) (interfaces/pins), [PAYLOAD.md](PAYLOAD.md) (on-air) and
[CONFIG.md](CONFIG.md) (which sensors are switched on).
Drivers live in `CM4/EnviroNode_CM4/Core/WL55JC1/{Inc,Src}/sensors/`.

Each section names its **config key** — the token that selects that sensor in the
brace configuration string, e.g. `{T1,T2,ST,60}`.

> **State: implemented.** All four drivers are complete, compiled into the CM4
> image, and driven by `envnode_sensors_sample()`. They have not yet been run
> against real sensors — bench-test each one with `nucleo sensors` on the ST-Link
> console before trusting a field deployment.
>
> Console: `nucleo sensors` prints one full frame (non-destructive — it does not
> consume the rain/wind interval), `nucleo uplink now` sends the 30-byte frame,
> `nucleo set {…}` (or a bare `{…}` line) selects the sensor set and interval,
> `nucleo interval <min>` changes just the uplink period, `info` shows the LoRaWAN
> identity, the canonical config string, and which drivers came up at boot.

---

## 1–2 · Air temp / humidity / pressure — 2× BME280 (`bme280.{h,c}`)
- **Config keys:** `T1` = air1 (I²C2 / shield) · `T2` = air2 (I²C1 / board pins).
- **Bus:** BME280 #1 ("air1") on **I²C2** (`hi2c2`, PA12/PA11 — the Grove
  shield's I²C sockets), #2 ("air2") on **I²C1** (`hi2c1`, PA9/PA10 — wired
  straight to the board pins). Two buses because both chips answer at
  `0x76`/`0x77`; `bme280_init_autoaddr()` probes both addresses on each bus.
- **Config:** verify chip id `0x60`; soft-reset (`0xB6`→`0xE0`); load the
  calibration blocks (`0x88..0xA1`, `0xE1..0xF0`); `ctrl_hum` osrs_h ×1, `ctrl_meas`
  osrs_t/p ×1, **forced mode** (sample-on-demand → low power), filter off.
- **Read:** kick forced measurement → poll `status.measuring` → burst-read 8 bytes
  → Bosch compensation (`t_fine` chain).
- **Calibration:** factory coefficients on-chip; no field cal needed. Sanity-check
  the two sensors agree within tolerance.

## 3 · Soil moisture — **Decagon 10HS** (`analog_sensors.{h,c}`)
- **Config key:** `SM`. **Pin:** ADC_IN4 — PB2, Arduino **A1** (the Grove **A0
  socket's second signal pin** — one socket carries A0+A1, so the LWS and the
  10HS share a socket; their power comes from the switched rail, not the socket).
- **Sensor:** Decagon Devices **10HS** — 10 cm capacitance/FDR probe running at
  **70 MHz**; measures the bulk dielectric of ~1 L of soil. Sold in irrigation
  channels as the Solem **"EC-10 HS"**; same hardware, Decagon manual applies
  [R13]. In the default sensor set alongside `T1,T2,LW`.
- **Power: 3 VDC @ 12 mA to 15 VDC @ 15 mA** [R13]. 3.3 V is inside the window
  but **only just above the 3.0 V floor** — and 12 mA is far beyond what a GPIO
  can source without sagging below that floor. **Power it from the switched
  high-side rail or 3V3 direct — never from a bare GPIO pin** (the LWS at ~4 mA
  already ruled that out; the 10HS at 12 mA is three times worse).
- **Output: 300–1250 mV, independent of excitation** [R13]. Unlike the LWS the
  10HS has an **onboard voltage regulator**, so its output is **not
  ratiometric**: a millivolt reading is meaningful on its own and Decagon's
  published calibration applies directly, whatever the excitation. At 12-bit /
  Vref 3.3 V that is ≈ **372 counts (dry air) to ≈ 1551 counts (saturation)**;
  the console prints counts **and** mV.
- **Measurement time: 10 ms** — the driver's shared `ANALOG_RAIL_SETTLE_MS`
  (15 ms) covers it, same pulsed-excitation discipline as the LWS.
- **Wiring** (3-wire pigtail, Decagon convention — same as the older LWS):

  | Function | Wire | Goes to |
  |---|---|---|
  | Excitation | **white** | switched 3V3 rail (12 mA!) |
  | Analog output | **red** | **A1** (PB2) |
  | Ground | bare/clear | GND |

- **On-air:** raw 12-bit counts, u16 at offset 14 — the curve is applied
  off-node so it can be refined without touching deployed firmware.
- **Calibration:** Decagon's standard **mineral-soil** polynomial [R13]
  (θ in m³/m³, x = sensor output in mV):
  `θ = 2.97e-9·x³ − 7.37e-6·x² + 6.69e-3·x − 1.92`
  Accuracy ±0.03 m³/m³ with the standard equation (typical mineral soils,
  EC < 10 dS/m), ±0.02 with a soil-specific calibration; range 0 to
  saturation (~0.57 m³/m³). High-EC or high-organic soils want their own
  two-point check (air-dry vs saturated sample of the actual soil).
- **Deployment:** prongs fully buried in undisturbed soil, no air gaps
  (auger a pilot slot and press the prongs into the face); keep ≥ 8 cm from
  large metal; the 5 m cable is attached.
- **Operating temperature: 0 to 50 °C** (survival −40 to 50 °C) [R13] — the
  sensor is not rated to *measure* below freezing, so sub-zero soil readings
  from a frost-prone site should be discarded off-node, not trusted.

## 4 · Leaf wetness — **Decagon LWS** (`analog_sensors.{h,c}`)
- **Config key:** `LW`. **Pin:** ADC_IN5 — PB1, Arduino **A0** (Grove A0 socket).
- **Sensor:** Decagon Devices **LWS**, renamed **PHYTOS 31** when Decagon became
  METER Group; METER's PHYTOS 31 manual covers this sensor [R11].
- **It is dielectric (capacitive), not a resistive grid.** It measures the
  dielectric constant of a ~1 cm zone above its upper surface — water (ε≈80) and
  ice (ε≈5) against air (ε≈1). Water does **not** need to bridge two traces,
  which is why the sensor needs no painting and no per-unit calibration, and why
  units are interchangeable. It also detects frost (reading lower than liquid
  water). *(This section previously said "resistive grid" — wrong sensor class.)*
- **Excitation: 2.5–5.0 VDC**, hard limits [R11]. **3.3 V is in spec**, so the
  board's 3V3 rail drives it directly with no level shifting. There is no
  "typical" excitation voltage — METER prints *Typical: NA*.
- **Current:** ~2 mA at 2.5 V, 7–8 mA at 5 V. At 3.3 V ≈ 3.5–4 mA
  (**interpolated, not a manufacturer figure**).
- **Output:** single-ended DC analog voltage against the sensor ground.
  **Ratiometric-ish** — METER: *"the sensor does not have a voltage regulator …
  the output will be somewhat proportional to the excitation"*. So **the ratio is
  trustworthy, an absolute mV number is not**: any published threshold must be
  rescaled to the excitation actually used.
- **Range:** **10 %–50 % of excitation**, dry→wet. At 3 V the legacy Decagon spec
  gives 320–1000 mV; the headline figure across the whole excitation range is
  300–1250 mV.
- **Expected at 3.3 V, 12-bit, Vref 3.3 V:** dry ≈ **430–450 counts**
  (Decagon quotes **445 counts dry**, ~1400 counts fully wet). A clean dry sensor
  reading outside ~410–470 counts means the wiring, the sensor, or the
  "Vex = Vref" assumption is wrong.
- **On air:** raw counts, u16 at offset 16 — no thresholding on the node. The
  console prints counts **and** mV so a reading can be compared to the datasheet.
- **Wiring — check the colours before applying power.** The two generations swap
  colours that exist in both:

  | Function | **LWS** (older) | **PHYTOS 31** | Goes to |
  |---|---|---|---|
  | Excitation | **white** | brown | 3V3 |
  | Analog output | **red** | orange | **A0** (PB1) |
  | Ground | bare/clear | bare/clear | GND |

- **⚠️ Excitation should be pulsed, not continuous.** METER intends the sensor for
  loggers that "provide short excitation pulses": ≥ **10 ms** settling, then
  sample. Continuous excitation from the Grove socket's always-on VCC costs
  ≈ 4 mA × 24 h ≈ **91 mAh/day** — acceptable on the bench, not on a solar node.
  Switching it needs a high-side load switch (**not** a GPIO: ~4 mA droops a
  push-pull output by 0.1–0.2 V ≈ 3–6 %, and the wet threshold sits only ~3 %
  above dry). **Not implemented — open item.**
- **Deployment:** electrodes facing **up**, ~45° from horizontal, pole-ward
  facing, clear of irrigation. Clean with water only; reapply UV protectant every
  ~45 days (yellowing is expected). **No IP rating is published** by Decagon or
  METER — do not quote one.
- **No mV→water-quantity transfer function exists.** It is a threshold/duration
  sensor; no accuracy or repeatability figure is published for either generation.

## 5 · Battery voltage — divider (`analog_sensors.{h,c}`)
- **Config key:** none — battery is **not selectable** and is measured on every
  cycle whatever the sensor set says (you cannot afford to lose it remotely).
- **Interface:** ADC via resistor divider. `Vbatt = Vadc × BATT_DIVIDER_RATIO`.
- **Set** `BATT_DIVIDER_RATIO` to your resistors; verify against a DMM.
- **On-air:** millivolts (u16), always sent.

## 6 · Wind direction — **Davis 7911** vane potentiometer (`analog_sensors.{h,c}`)
- **Config key:** `WD` (shares the `SENS_OK_WIND` status bit with `WS`).
  **Pin:** ADC_IN3 — PB4, Arduino **A3**.
- **Sensor:** the direction half of the **Davis 7911** (datasheet DS7911 Rev G,
  [R12]) — a wind vane on a **20 kΩ potentiometer**.
- **Datasheet output:** *"Variable resistance 0–20 K; 10 K = south, 180°"* — linear,
  0 Ω = 0° (north), 20 kΩ = 360°.
- **Interface:** the pot **is** the divider — yellow to 3V3, red to GND, green
  (wiper) to A3. `deg = counts/4095 × 360 + vane_offset`, wrapped 0–360.
- **Dead band:** the wiper leaves the track over a small arc at the 0°/360°
  crossover; unloaded, the ADC input floats and reads noise there. A **1 MΩ
  pull-down** on the wiper pins that arc to ~0 V ⇒ 0° = north, which is where the
  dead band physically is — so the failure mode degrades to the right answer.
  1 MΩ against a 20 kΩ pot costs ≈ 0.5 % ≈ 2° worst case (mid-scale), inside the
  sensor's own ±7°. A 100 kΩ pull-down would cost ~17° — too much.
- **Accuracy / resolution:** ±7°, 1°.
- **Calibration:** `set_winddir_offset` (downlink 0x05) aligns the vane's
  electrical zero to true/magnetic north.

## 7 · Soil temperature — **PT1000 divider on A2** (`analog_sensors.{h,c}`)
- **Config key:** `ST`. **Pin:** ADC_IN6 — PA10, Arduino **A2**.
- **Interface — no front-end chip at all** (the MAX31865 was dropped,
  LOGBOOK r18): a plain resistor divider, hardware-verified 2026-08-13 at
  19.5–20.4 °C on the bench (LOGBOOK r20).

  ```
  3V3 ──[ ~900 Ω series ]── A2 ──[ PT1000 probe ]── GND
  ```
- **Ratiometric by construction:** the divider's top rail is the ADC's own
  reference, so the rail voltage cancels exactly:
  `R_rtd = Rs · counts / (4095 − counts)`, then the same Callendar–Van Dusen
  conversion the MAX31865 used (`pt1000_ohms_to_celsius()`, sub-zero inverse
  polynomial included — frost matters for soil).
- **⚠️ The series resistor's value IS the calibration.** Sensitivity is only
  ~3.6 counts/°C, so each ohm of error in `ANALOG_RTD_SERIES_OHMS`
  (`analog_sensors.h`, 900.0 default) is ~0.26 °C. **Measure the fitted
  resistor with a DMM and set the constant to that value**; fine-trim with
  `set_cal` sensor_id 7. Expect ~±0.3 °C of quantisation jitter (1 count).
- **⚠️ A2 is I²C1 SDA** — BME280 #2's bus. While the divider is fitted, `T2`
  is electrically unusable (the divider holds SDA at ~1.8 V). The firmware
  muxes PA10 to analog only for the conversion and returns it to I²C1 after,
  so removing the divider restores `T2` with no firmware change.
- **Fault detection:** open probe → the series R pulls A2 to the rail
  (counts ≥ 4050 rejected); shorted probe → counts ≤ 50 rejected; anything
  outside −60…120 °C rejected. All raise the normal `ST` fault path.
- **Power:** with the top leg on permanent 3V3 the divider leaks
  ~1.7 mA continuously (~40 mAh/day) and dissipates ~3 mW in the probe
  (mild self-heating). For a field node, move the top leg to **VSENS** — the
  read already happens while the rail is up; it is a wire move only.
- **Expected values:** 0 °C = 1000 Ω ≈ 2153 counts; 20 °C = 1078 Ω ≈ 2231
  counts; each °C ≈ +3.6 counts.

### 7b · The MAX31865 alternative (not fitted)
A future node needing better than ~±0.5 °C can fit a MAX31865 on SPI
(`max31865.{h,c}` stays compiled): **Rref = 4.02 kΩ** for PT1000 (not 430 Ω),
`MAX31865_RTD_NOMINAL = 1000`, 50 Hz mains reject, one-shot with bias off
between reads. The A2 divider costs one resistor and no bus; the MAX31865
buys 15-bit resolution and lead-resistance compensation (3/4-wire).

## 8 · Rain — tipping bucket (`pulse_counter.{h,c}`)
- **Config key:** `R`. **Edge-counted → the node must stay awake while `R` is
  selected** (see [CONFIG.md](CONFIG.md); STOP2 sleep itself is Phase 5).
- **Interface:** reed switch on **PB3 (D3)** → **EXTI3**, internal pull-up,
  falling edge. Debounced in `pulse_rain_isr` (`RAIN_DEBOUNCE_MS` = 100 ms — a
  bucket cannot physically tip faster than that).
- **Calibration:** `RAIN_MM_PER_TIP` (0.2794 mm for a 0.011″ bucket — set to yours).

## 9 · Wind speed — **Davis 7911** anemometer (`pulse_counter.{h,c}`)
- **Config key:** `WS` (carries the gust field too). **Sampled, not edge-counted —
  so selecting `WS` no longer blocks STOP2 sleep.**
- **Sensor:** the speed half of the **Davis 7911** — datasheet calls it a *"solid
  state magnetic sensor"* whose output is a **contact closure to ground**, one per
  revolution. Measured cold: open circuit ↔ ~100 Ω closed.
- **Interface:** **PB14 (Arduino A4) = ADC_IN1**, with a **47 kΩ pull-up to 3V3 at
  the connector**. `analog_wind_burst()` samples the pin at ~1 kHz for
  `ANEMO_BURST_MS` (3 s) and counts high→low transitions.
- **Why sampled and not interrupt-counted.** A contact closure carries no voltage
  proportional to speed, so an ADC cannot read it directly — and with passive
  parts there is no way to convert frequency to voltage (an RC average is
  duty-cycle dependent, not frequency dependent, so it reads the same at 2 m/s
  and 20 m/s). Counting is the only option, and doing it by interrupt pins the
  core awake for the whole interval — an estimated ~50–60 mAh/day, roughly ten
  times the vane pot and pull-up combined. A burst runs inside the cycle's normal
  awake window instead.
- **The trade, stated plainly:** the burst sees **3 seconds of wind per cycle**,
  not the whole interval. Gusts between bursts are invisible, and `wind_speed`
  and `wind_gust` are **equal by construction** — the 3 s window *is* the WMO gust
  window. True interval averaging needs EXTI wake-from-STOP2, which additionally
  needs the pulse counter moved off `HAL_GetTick()` onto the RTC or an LPTIM.
- **Edge detection:** hysteresis at `ANEMO_HI_COUNTS` (2600 ≈ 2.1 V) and
  `ANEMO_LO_COUNTS` (1400 ≈ 1.1 V) rejects the ~1 ms of contact bounce that a
  single threshold would count several times; `WIND_DEBOUNCE_MS` (5 ms) catches
  the rest. ~11 samples per period at the sensor's 88 Hz (89 m/s) ceiling.
- **On the analog header on purpose:** A4 sits next to the direction wiper on A3,
  so the 7911's single 4-wire cable lands on **one Grove socket** (A3 + A4 + VCC +
  GND). The battery divider moved to **A5** to free it.
  *(History: PB5/D4 EXTI → PB14/A4 EXTI on 2026-07-30 → PB14/A4 ADC on 2026-08-04.)*
- **Calibration — from the datasheet, not a guess:**
  `V = P(2.25/T)` with V in **mph**, P = pulses, T = seconds ⇒ **1 Hz = 2.25 mph**.
  Cross-checks against the datasheet's *"1600 rev/hr = 1 mph"* (0.444 Hz per mph).
  Converted once, in `pulse_counter.h`:
  `ANEMO_MS_PER_HZ = 2.25 × 0.44704 = 1.00584` m/s per Hz.
  ⚠️ The previous generic default of **0.34 under-read this sensor by 3×**.
- **Range / accuracy:** 0.5–89 m/s; ±1 m/s or ±5 %, whichever is greater;
  resolution 0.1 m/s.
- **Gust:** pulses are bucketed into rolling `GUST_WINDOW_MS` (3 s) windows and
  the fullest bucket of the interval is reported — the WMO 3-second gust. This
  replaced "shortest gap seen", which one bounced edge could spike into a
  nonsense gust. (Davis's own console instead counts revolutions over a 2.25 s
  window, which is why 2.25 appears in the formula above.)
- **Cable:** 4-conductor 26 AWG, 12 m attached, RJ-11 plug. Recommended maximum
  42 m sensor-to-logger; beyond that the maximum *recordable* speed falls, though
  accuracy does not. A **10 kΩ external pull-up** to 3V3 is recommended over the
  internal ~40 kΩ because of that cable length.

---

## Key → measurement → driver → field → payload offset

| Key | Measurement | Driver | `sensor_readings_t` field | Uplink off (FPort 1) |
|---|---|---|---|---|
| `T1` | Air A temp/RH/press | `bme280` (**I²C2**, shield) | `air1_temp_c` / `air1_rh_pct` / `air1_press_hpa` | 4 / 6 / 7 |
| `T2` | Air B temp/RH/press | `bme280` (**I²C1**, board pins) | `air2_temp_c` / `air2_rh_pct` / `air2_press_hpa` | 9 / 11 / 12 |
| `SM` | Soil moisture (10HS) | `analog_sensors` | `soil_moist_raw` | 14 |
| `LW` | Leaf wetness | `analog_sensors` | `leaf_wet_raw` | 16 |
| `ST` | Soil temperature (PT1000, A2 divider) | `analog_sensors` | `soil_temp_c` | 18 |
| `WS` | Wind speed / gust | `pulse_counter` | `wind_speed_ms` / `wind_gust_ms` | 20 / 24 |
| `WD` | Wind direction | `analog_sensors` | `wind_dir_deg` | 22 |
| `R` | Rain tips / mm | `pulse_counter` | `rain_tips` / `rain_mm` | 26 / 28 |
| — | Battery | `analog_sensors` | `batt_v` | 2 (always, not selectable) |

Each driver sets its **status OK-bit** (`SENS_OK_*`) on a good read; the packer
substitutes the sentinel (`0x7FFF`/`0xFFFF`/`0xFF`) when a bit is clear. A sensor
left out of the config string is skipped entirely: OK-bit clear, sentinel on air,
**no** `SENS_FAULT` — see [CONFIG.md](CONFIG.md).
