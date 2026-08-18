# EnviroNode-WL55 — Pin & Peripheral Allocation

> **Status: LOCKED (firmware matches this table).** The Arduino/Grove header
> mapping below is taken from **UM2592 Table 17** (Arduino connector pinout,
> NUCLEO-WL55JC1) cross-checked against the mbed `NUCLEO_WL55JC` board file and
> the CubeMX MCU database (`STM32WL55JCIx.xml`) for the ADC channel numbers and
> alternate functions. Any change here must be mirrored in
> `Core/WL55JC1/Inc/pins_config.h`, `i2c.c`, `spi.c`, `adc.c`, `gpio.c`.

MCU: **STM32WL55JC** (dual core, UFBGA73). CM4 owns all application peripherals
below; CM0+ owns only the SubGHz radio (LoRaWAN). The two cores talk over the
shared SRAM2 mailbox (see [ARCHITECTURE.md](ARCHITECTURE.md)).

## Board reference — NUCLEO-WL55JC1 Arduino headers (UM2592 Table 17)

| Arduino | MCU pin | Arduino | MCU pin | Arduino | MCU pin |
|---|---|---|---|---|---|
| A0 | PB1 (ADC_IN5) | D0 | PB7 | D8  | PC2 |
| A1 | PB2 (ADC_IN4) | D1 | PB6 | D9  | PA9 |
| A2 | PA10 (ADC_IN6) | D2 | PB12 | D10 | PA4 (SPI1_NSS) |
| A3 | PB4 (ADC_IN3) | D3 | PB3 | D11 | PA7 (SPI1_MOSI) |
| A4 | PB14 (ADC_IN1) | D4 | PB5 | D12 | PA6 (SPI1_MISO) |
| A5 | PB13 (ADC_IN0) | D5 | PB8 | D13 | PA5 (SPI1_SCK) |
| | | D6 | PB10 | D14 | PA11 (**I2C2_SDA**) |
| | | D7 | PC1 | D15 | PA12 (**I2C2_SCL**) |

> **Doc trap:** UM2592 labels D14/D15 "I2C1_SDA/I2C1_SCL", but on the STM32WL55
> PA11/PA12 are **I2C2** (AF4) — the manual's label is wrong and ST has
> acknowledged it. The Grove Base Shield's four I²C sockets therefore land on
> **I²C2**, which is why the shield-side BME280 is bus #1 = I²C2.

## Sensor → interface map

| # | Measurement | Sensor / element | Interface | Pins | Notes |
|---|---|---|---|---|---|
| 1 | Air T/RH/P **(A)** | BME280 #1 | **I²C2** *(shield)* | PA12 SCL / PA11 SDA | Grove I²C socket, addr `0x76`/`0x77` |
| 2 | Air T/RH/P **(B)** | BME280 #2 | **I²C1** *(board pins)* | PA9 SCL / PA10 SDA | hand-wired; second bus dodges the address clash |
| 3 | Leaf wetness | **Decagon LWS** (3-wire analog) | **ADC_IN5** | PB1 (**A0**) | Grove A0 socket; ratiometric output, excite from 3V3 — see [SENSORS.md](SENSORS.md) |
| 4 | Soil moisture | **Decagon 10HS** (3-wire analog) | **ADC_IN4** | PB2 (**A1**) | shares the Grove A0 socket with the LWS (A0+A1); output 300–1250 mV regulated; **12 mA** — power from the switched rail, never a GPIO — see [SENSORS.md](SENSORS.md) |
| 5 | Wind **direction** | **Davis 7911** vane pot (20 kΩ) | **ADC_IN3** | PB4 (**A3**) | green = wiper; 0 Ω = N, 10 k = S; 1 MΩ pull-down for the dead band |
| 6 | Battery voltage | resistor divider | **ADC_IN0** | PB13 (**A5**) | fallback; primary is the INA219 bus voltage. Moved off A4 to free it for wind speed |
| 6b | Battery V + I | INA219 | **I²C2** | PA12/PA11 | addr `0x40` as fitted (init probes `0x45` then `0x40`), R_shunt 0.1 Ω |
| 7 | Soil temperature | **PT1000 RTD**, ~900 Ω divider off 3V3 | **ADC_IN6** | PA10 (**A2**) | ratiometric (`R = Rs·c/(4095−c)`); pin muxed from I²C1 SDA per read — **`T2` unusable while fitted**. MAX31865 variant dropped 2026-08-13 |
| 7b | Offline CSV log | **SD card** (SPI mode) | **SPI1** | PA5 SCK / PA6 MISO / PA7 MOSI, **CS PB8 (D5)** | mode 0, init ≤400 kHz then 2 MHz; ~10 k pull-up on CS; FAT32 ≤32 GB; 3V3 direct (12–35 mA writes) |
| 8 | Rain | tipping-bucket reed | **GPIO EXTI3** | PB3 (D3) | pull-up, falling edge, SW debounce |
| 9 | Wind **speed** | **Davis 7911** contact closure | **ADC_IN1** *(burst-sampled)* | PB14 (**A4**) | black = contact to GND; 47 kΩ pull-up at the connector; 1 Hz = 2.25 mph. Sampled ~1 kHz for 3 s so the node can still sleep |
| — | LoRaWAN | SubGHz (internal) | **CM0+ radio core** | — | AU915 FSB2 (match TTN) |
| — | Debug console | USART2 → ST-Link VCP | — | PA2 TX / PA3 RX | 115200 8N1, command server *(reused)* |
| — | Aux console | USART1 | — | PB6 TX (D1) / PB7 RX (D0) | mirrored command server *(reused)* |
| — | **Status LED** (power button) | external LED + ~470 Ω | GPIO out | **PB10 (D6)** | blink language — solid=boot, 1/2/3 flashes = ok/no-join/fault, dark=asleep (LOGBOOK Table 9a) |
| — | Legacy status LED | external LED | GPIO out | PC2 (D8) | brief low-duty pulses *(reused)* |
| — | RF front-end | board RF switch | GPIO out | PC3/PC4/PC5 | **do not touch** (FE_CTRL1..3) |
| — | Timekeeping / wake | RTC (LSE) | internal | PC14/PC15 | periodic sample/uplink wake |

The **Davis 7911**'s four wires land on the analog Grove socket that carries
**A3 + A4**: green (direction wiper) → A3, black (speed contact) → A4, yellow →
VCC, red → GND. One cable, one socket, no splitting. Rain keeps the Grove "D3"
socket (PB3) on its own.

### Switched sensor rail (VSENS) — the one transistor worth fitting

Every *powered* analog sensor hangs off a gated rail so it draws nothing between
measurements. Enable pin: **PB5 = Arduino D4**, settle 15 ms, then convert
(`analog_sensors.c`).

| On VSENS | Draw while excited |
|---|---|
| Decagon LWS excitation | ~4 mA |
| Decagon 10HS excitation | ~12 mA (its regulator needs ≥3.0 V — rail or 3V3, never a GPIO) |
| Davis 7911 direction pot (yellow) | 165 µA (3.3 V / 20 kΩ) |
| future powered analog probes | — |

Continuous ≈ **390 mAh/day** with all three. Pulsed 15 ms per cycle at 15 min ≈
**0.007 mAh/day** — about five orders of magnitude, and the LWS manual
*requires* pulsed excitation anyway.

**Switch the HIGH side, never the low side.** A single NPN in the ground return is
the obvious one-transistor circuit and it is wrong here: it lifts each sensor's
ground by Vce(sat) (0.1–0.2 V), and because these outputs are ground-referenced
that offset lands directly in the ADC reading — ~150 counts, when the LWS wet
threshold sits only ~3 % above its dry baseline. It would read permanently wet.

**Option A — NPN + PNP high-side switch** (`ENV_SENSPWR_ACTIVE_HIGH = 1`, default)

```
                3V3 ──────┬──────────────┐
                          │ 10k          │
                     ┌────┴────┐         │
        PB5 (D4) ──[4k7]──| NPN |        │ PNP (e.g. BC807 / MMBT3906)
                     │    | BC847|       │  emitter → 3V3
                    GND   └──┬──┘        │  base    → NPN collector
                             └───────────┤  collector → VSENS
                                         └──→ VSENS ──→ LWS excitation
                                                    └─→ 7911 yellow
        D4 HIGH = rail ON
```

**Option B — single P-MOSFET** (`ENV_SENSPWR_ACTIVE_HIGH = 0`)

```
        3V3 ──────┬── source
                  │   P-MOS (e.g. DMG2301L / AO3401 — |Vgs(th)| < 1.5 V)
   PB5 (D4) ──────┼── gate      (+ 100k gate→3V3 so it is OFF while the
                  │              MCU is in reset)
                  └── drain ──→ VSENS
        D4 LOW = rail ON
```

One part, no Vbe drop, no base current, µV of drop at 4 mA. Option A exists
because it uses the NPN most people already have; Option B is the better circuit.

> Either way the firmware is identical — only the `ENV_SENSPWR_ACTIVE_HIGH` define
> in `pins_config.h` changes. **Do not** try to power the LWS from the GPIO pin
> directly: ~4 mA droops a push-pull output 0.1–0.2 V ≈ 3–6 %, which fabricates or
> masks a wet event all by itself.

### Davis 7911 external parts (no transistor needed)

| Part | Where | Why |
|---|---|---|
| **47 kΩ** | A4 → 3V3 | pull-up for the speed contact. A4 is an ADC input now, so the internal pull-up is unavailable — this resistor is **required**, not optional. 47 kΩ halves the closed-contact current versus 10 kΩ while staying stiff enough for the 12 m cable |
| 1 MΩ | A3 → GND | pins the vane's dead band to ~0 V = north instead of letting the wiper float. 100 kΩ would skew mid-scale readings by ~17° |

> **No debounce capacitor on A4.** An earlier revision suggested an optional
> 100 nF — with the 47 kΩ pull-up that is RC ≈ 4.7 ms, which at the sensor's
> 88 Hz ceiling (5.7 ms half-period) would keep the line from ever crossing the
> sampling hysteresis and silently cap recordable wind speed. The software
> hysteresis + 5 ms debounce handle bounce on their own.

## Free pins (room for the sensors added later)

`PB12` (D2) · `PA15` · `PA0` · `PA1` ·
`PA8` · `PB15` · `PC0` · `PC6` · `PC1` (D7). `PB10` (D6) became the status
LED on 2026-08-18 (blink language: LOGBOOK Table 9a).

`PA4` (D10) is **parked, not free** — gpio.c still drives it output-HIGH as the
dropped MAX31865's chip-select (ENV_RTD_CS); reclaim it by removing that init
before attaching anything to D10.

`PB5` (D4) is the switched-rail enable (VSENS). `PB8` (D5) became the SD card's
chip-select on 2026-08-13 — the pigtail's CS wire was soldered there (one
mirror-count off from the intended D2) and the firmware was repointed to match,
so `PB12` (D2) is free again.

`PB10`/`PC1` were the inherited Pi-wake / Pi-power outputs; that code is gone from
`main.c` and nothing drives them any more.

### Notes & cautions
- **PB3 doubles as JTDO/SWO.** SWD debug only needs PA13/PA14, so using PB3 as a
  pulse input is safe — but printf-over-SWO trace is not available.
- **I²C pull-ups:** both buses need 4.7 kΩ to 3V3. Most BME280 breakouts include
  them; the Grove shield does not add any.
- **PT1000 series resistor:** the A2 divider's temperature accuracy is set by
  how well `ANALOG_RTD_SERIES_OHMS` matches the fitted part — ~0.26 °C per ohm
  of error. Measure it with a DMM, don't trust the band code. (If a future
  node fits a MAX31865 instead: Rref = 4.02 kΩ for PT1000, not 430 Ω.)
- **Pulse debounce:** reed switches bounce for milliseconds — debounced in
  software (`PULSE_DEBOUNCE_MS`), optionally an RC + Schmitt input as well.
- **ADC clock:** the core runs HSI 16 MHz at voltage scale 2, so the ADC is
  clocked PCLK/4 = 4 MHz with a 160.5-cycle sampling time (~40 µs) — long enough
  for the high-impedance soil/leaf probes.
- **Power:** budget ADC excitation (10HS alone is 12 mA) + 2× BME280 + SD-card
  writes + LoRa TX peaks; gate sensor rails where possible and sample in bursts
  (solar/battery node).

## Peripheral inventory (what the firmware instantiates)

`I2C1` (PA9/PA10 — SDA muxed to ADC_IN6 per `ST` read), `I2C2` (PA12/PA11),
`SPI1` (PA5/PA6/PA7 + PB8 CS, SD card),
`ADC` (IN5 leaf, IN4 soil, IN6 soil-temp divider, IN3 vane, IN1 wind-speed
burst, IN0 battery — sequential single conversions,
rank 1 re-armed per channel), `RTC` (LSE, also the STOP2 wake-up source),
`EXTI3` (rain only — wind speed is ADC burst-sampled on IN1), `USART1`,
`USART2`, and the inherited `IWDG` watchdog. The SubGHz radio + LoRaMAC live
entirely on CM0+.

> **Changed 2026-07-30:** leaf wetness and soil moisture **swapped** channels —
> leaf is now `ADC_IN5` on **A0** (where the Decagon LWS is wired) and soil
> moisture is `ADC_IN4` on **A1**. Anything built before that date has them the
> other way round.
