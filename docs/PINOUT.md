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
| 4 | Soil moisture | capacitive/resistive probe | **ADC_IN4** | PB2 (A1) | long sampling time; needs calibration curve |
| 5 | Wind **direction** | **Davis 7911** vane pot (20 kΩ) | **ADC_IN3** | PB4 (**A3**) | green = wiper; 0 Ω = N, 10 k = S; 1 MΩ pull-down for the dead band |
| 6 | Battery voltage | resistor divider | **ADC_IN0** | PB13 (**A5**) | fallback; primary is the INA219 bus voltage. Moved off A4 to free it for wind speed |
| 6b | Battery V + I | INA219 | **I²C2** | PA12/PA11 | addr `0x45`, R_shunt 0.1 Ω *(reused)* |
| 7 | Soil temperature | **PT1000 RTD** + MAX31865 | **SPI1** | PA5 SCK / PA6 MISO / PA7 MOSI, CS PA4 | D13/D12/D11/D10 — plain Arduino SPI |
| 8 | Rain | tipping-bucket reed | **GPIO EXTI3** | PB3 (D3) | pull-up, falling edge, SW debounce |
| 9 | Wind **speed** | **Davis 7911** contact closure | **GPIO EXTI14** | PB14 (**A4**) | black = contact to GND; pull-up, falling edge; 1 Hz = 2.25 mph |
| — | LoRaWAN | SubGHz (internal) | **CM0+ radio core** | — | AU915 FSB2 (match TTN) |
| — | Debug console | USART2 → ST-Link VCP | — | PA2 TX / PA3 RX | 115200 8N1, command server *(reused)* |
| — | Aux console | USART1 | — | PB6 TX (D1) / PB7 RX (D0) | mirrored command server *(reused)* |
| — | Status LED | external LED | GPIO out | PC2 (D8) | brief low-duty pulses *(reused)* |
| — | RF front-end | board RF switch | GPIO out | PC3/PC4/PC5 | **do not touch** (FE_CTRL1..3) |
| — | Timekeeping / wake | RTC (LSE) | internal | PC14/PC15 | periodic sample/uplink wake |

The **Davis 7911**'s four wires land on the analog Grove socket that carries
**A3 + A4**: green (direction wiper) → A3, black (speed contact) → A4, yellow →
VCC, red → GND. One cable, one socket, no splitting. Rain keeps the Grove "D3"
socket (PB3) on its own.

### Davis 7911 external parts (no transistor needed)

| Part | Where | Why |
|---|---|---|
| 10 kΩ | A4 → 3V3 | pull-up for the speed contact. The internal ~40 kΩ works, but the cable is 12 m |
| 1 MΩ | A3 → GND | pins the vane's dead band to ~0 V = north instead of letting the wiper float. 100 kΩ would skew mid-scale readings by ~17° |
| 100 nF *(optional)* | A4 → GND | RC debounce, ~1 ms with the 10 kΩ — well under the 5 ms software debounce |

## Free pins (room for the sensors added later)

`PB8` (D5) · `PB12` (D2) · `PB5` (D4) · `PA15` · `PA0` · `PA1` ·
`PA8` · `PB15` · `PC0` · `PC6` · `PB10` (D6) · `PC1` (D7).

`PB5` (D4) freed on 2026-07-30 when wind speed moved to PB14/A4; `PB13` (A5) is
now the battery divider.

`PB10`/`PC1` were the inherited Pi-wake / Pi-power outputs; that code is gone from
`main.c` and nothing drives them any more.

### Notes & cautions
- **PB3 doubles as JTDO/SWO.** SWD debug only needs PA13/PA14, so using PB3 as a
  pulse input is safe — but printf-over-SWO trace is not available.
- **I²C pull-ups:** both buses need 4.7 kΩ to 3V3. Most BME280 breakouts include
  them; the Grove shield does not add any.
- **PT1000 vs PT100:** the MAX31865 reference resistor must be ~4×Rnominal —
  **4.02 kΩ (0.1 %)** for PT1000 (430 Ω is PT100). Same value in firmware
  (`MAX31865_RREF`), and pick 2/3/4-wire to match the probe wiring.
- **Pulse debounce:** reed switches bounce for milliseconds — debounced in
  software (`PULSE_DEBOUNCE_MS`), optionally an RC + Schmitt input as well.
- **ADC clock:** the core runs HSI 16 MHz at voltage scale 2, so the ADC is
  clocked PCLK/4 = 4 MHz with a 160.5-cycle sampling time (~40 µs) — long enough
  for the high-impedance soil/leaf probes.
- **Power:** budget ADC excitation + 2× BME280 + MAX31865 + LoRa TX peaks; gate
  sensor rails where possible and sample in bursts (solar/battery node).

## Peripheral inventory (what the firmware instantiates)

`I2C1` (PA9/PA10), `I2C2` (PA12/PA11), `SPI1` (PA5/PA6/PA7 + PA4 CS),
`ADC` (IN5 leaf, IN4 soil, IN3 vane, IN1 battery — sequential single conversions,
rank 1 re-armed per channel), `RTC` (LSE, also the STOP2 wake-up source),
`EXTI3` + `EXTI5` (rain, wind), `USART1`, `USART2`, and the inherited `IWDG`
watchdog. The SubGHz radio + LoRaMAC live entirely on CM0+.

> **Changed 2026-07-30:** leaf wetness and soil moisture **swapped** channels —
> leaf is now `ADC_IN5` on **A0** (where the Decagon LWS is wired) and soil
> moisture is `ADC_IN4` on **A1**. Anything built before that date has them the
> other way round.
