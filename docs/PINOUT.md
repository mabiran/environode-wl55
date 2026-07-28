# EnviroNode-WL55 — Pin & Peripheral Allocation

> **Status: PROPOSED.** These assignments are the design's single source of truth
> for firmware, but the exact GPIOs **must be finalized in STM32CubeMX against the
> real board + sensor shield** before hardware bring-up. Where a pin is inherited
> from the KoreroNet WL55JC1 design it is marked *(reused)*; those are known-good
> on the NUCLEO-WL55JC. Anything marked *(TBD)* needs a datasheet/CubeMX check for
> the specific alternate-function / ADC-channel mapping.

MCU: **STM32WL55JC** (dual core). CM4 owns all application peripherals below;
CM0+ owns only the SubGHz radio (LoRaWAN). The two cores talk over the shared
SRAM2 mailbox (see [ARCHITECTURE.md](ARCHITECTURE.md)).

## Sensor → interface map

| # | Measurement | Sensor / element | Interface | Notes |
|---|---|---|---|---|
| 1 | Air temp / humidity / pressure **(A)** | BME280 #1 | **I²C1** | addr `0x76` or `0x77` |
| 2 | Air temp / humidity / pressure **(B)** | BME280 #2 | **I²C2** | addr `0x76`/`0x77`; second bus avoids the address clash |
| 3 | Soil moisture | capacitive/resistive probe | **ADC** (analog) | ratiometric; needs calibration curve |
| 4 | Leaf wetness | resistive grid | **ADC** (analog) | ratiometric |
| 5 | Battery voltage | resistor divider | **ADC** (analog) | scale by divider ratio |
| 6 | Wind **direction** | wind-vane potentiometer | **ADC** (analog) | 0–Vref ↦ 0–360° |
| 7 | Soil temperature | **PT1000 RTD** | **SPI → MAX31865** | dedicated RTD front-end; CS + optional DRDY |
| 8 | Rain | tipping-bucket reed switch | **GPIO EXTI** (pulse count) | debounced; each tip = fixed mm |
| 9 | Wind **speed** | anemometer reed/hall | **GPIO EXTI or TIM ext-clock** | count pulses / measure frequency |
| — | LoRaWAN | SubGHz (internal) | **CM0+ radio core** | AU915 FSB2 (match TTN region) |
| — | Debug console | LPUART/USART | **ST-Link VCP** | 115200 8N1, command server *(reused)* |
| — | Timekeeping / wake | RTC + LPTIM | internal | periodic low-power sample/uplink wake |

## Proposed GPIO assignment (finalize in CubeMX)

| Function | Pin(s) | Peripheral / AF | Source |
|---|---|---|---|
| I²C1 SCL / SDA | **PB8 / PB9** *(TBD)* | I2C1 | new |
| I²C2 SCL / SDA | **PA12 / PA11** *(reused)* | I2C2 | KoreroNet INA219 bus |
| SPI1 SCK / MISO / MOSI | **PA5 / PA6 / PA7** *(TBD)* | SPI1 | MAX31865 |
| MAX31865 CS | **PB0** *(TBD)* | GPIO out | RTD chip-select |
| MAX31865 DRDY | **PB5** *(TBD, optional)* | GPIO EXTI in | data-ready IRQ |
| ADC — soil moisture | **PB1 / ADC_INx** *(TBD)* | ADC | channel # per datasheet |
| ADC — leaf wetness | **PB2 / ADC_INx** *(TBD)* | ADC | |
| ADC — battery | **PB4 / ADC_INx** *(TBD)* | ADC | via divider |
| ADC — wind direction | **PB3 / ADC_INx** *(TBD)* | ADC | vane potentiometer |
| Rain bucket pulse | **PC0** *(TBD)* | GPIO EXTI | debounce in SW |
| Wind-speed pulse | **PC1** *(TBD)* | GPIO EXTI / TIM2 ext | free on this board |
| Debug UART TX / RX | **PA2 / PA3** *(reused)* | USART2 → VCP | console/command server |

### Notes & cautions
- **ADC channels:** the STM32WL55 ADC maps specific pins to specific `ADC_INx`
  channels — confirm each pin's channel number in the datasheet when you draw the
  `.ioc`. Prefer pins that don't collide with I²C2/SPI1 above.
- **Two I²C buses are required**, not optional: two BME280s share the same
  address space, so each needs its own bus (or an address strap + one bus — but
  the brief says "2 separate I²C connections", so two buses it is).
- **PT1000 vs PT100:** the MAX31865 reference resistor **must** be ~4×Rnominal —
  use a **4.02 kΩ** (0.1%) Rref for PT1000 (vs 430 Ω for PT100). Set the same in
  firmware (2-, 3-, or 4-wire mode per wiring).
- **Pulse debounce:** reed switches bounce for milliseconds — debounce rain/wind
  pulses in software (min inter-pulse time) or with an RC + Schmitt input.
- **Power:** budget the ADC excitation + BME280 + MAX31865 + LoRa TX peaks; gate
  sensor rails where possible and sample in bursts to keep the average draw low
  (this node is expected to be solar/battery like KoreroNet).

## Peripheral inventory (what the firmware will instantiate)

`I2C1`, `I2C2`, `SPI1`, `ADC` (4 channels, scan or sequential), `RTC`, `LPTIM`
(low-power periodic wake), 2× `EXTI` (rain, wind-speed) **or** a `TIM` in
external-counter mode for wind-speed, plus the inherited `USART` console and the
`IWDG` watchdog. The SubGHz radio + LoRaMAC live entirely on CM0+.
