# EnviroNode-WL55 — Connection table (as built, r29, 2026-08-25)

Board: NUCLEO-WL55JC1 (STM32WL55JC) + Seeed Grove Base Shield V2 (selector at 3.3 V).
"Nucleo pin" = Arduino label · connector-pin (morpho equivalent in brackets). "MCU pin" = STM32WL55 port pin.

## 1 · Sensors and modules

| Device / wire | Nucleo pin | MCU pin | Value / address |
|---|---|---|---|
| **BME280 #1** SCL | D15 · CN5-10 (Grove I²C socket) | PA12 (I2C2_SCL) | I²C addr **0x76** |
| BME280 #1 SDA | D14 · CN5-9 (Grove I²C socket) | PA11 (I2C2_SDA) | |
| BME280 #1 VCC / GND | 3V3 · CN6-4 / GND | — | |
| **INA219** SCL | D15 · CN5-10 (Grove I²C socket) | PA12 (I2C2_SCL) | I²C addr **0x40** |
| INA219 SDA | D14 · CN5-9 (Grove I²C socket) | PA11 (I2C2_SDA) | |
| INA219 VCC / GND | 3V3 / GND | — | |
| INA219 VIN+ | battery pack **+** | — | shunt 0.1 Ω |
| INA219 VIN− | → power switch → E5V (see §3) | — | |
| **BME280 #2** SCL | D9 · CN5-2 (CN10-19) | PA9 (I2C1_SCL) | I²C addr 0x76 (auto-probes 0x76/0x77) |
| BME280 #2 SDA | A2 · CN8-3 (CN7-32) | PA10 (I2C1_SDA) | |
| BME280 #2 VCC / GND | 3V3 / GND | — | |
| **Decagon LWS** red (signal) | A0 · CN8-1 (CN7-28) | PB1 (ADC_IN5) | |
| LWS white (excitation) | VSENS rail (see §2) | — | |
| LWS bare | GND | — | |
| **Decagon 10HS** red (signal) | A1 · CN8-2 (CN7-30) | PB2 (ADC_IN4) | |
| 10HS white (excitation) | VSENS rail (see §2) | — | 12 mA |
| 10HS bare | GND | — | |
| **PT1000** wire 1 | morpho **CN7-17** | PA15 (ADC_IN11) | junction node |
| PT1000 wire 2 | GND · CN7-19 | — | |
| 905 Ω resistor, end 1 | morpho CN7-17 (same node as PT1000 wire 1) | PA15 | measured 905 Ω |
| 905 Ω resistor, end 2 | 3V3 · CN7-16 | — | |
| **Davis 7911** green (vane wiper) | A3 · CN8-4 (CN7-34) | PB4 (ADC_IN3) | |
| 7911 black (speed contact) | A4 · CN8-5 (CN7-36) | PB14 (ADC_IN1) | |
| 7911 yellow (pot supply) | VSENS rail (see §2) | — | |
| 7911 red | GND | — | |
| R1 47 kΩ | A4 → 3V3 | PB14 | speed-contact pull-up |
| R2 1 MΩ | A3 → GND | PB4 | vane dead-band pull-down |
| **Rain gauge** reed, side 1 | D3 · CN9-4 (CN10-31) | PB3 (EXTI3) | 0.2 mm/tip |
| Rain reed, side 2 | GND | — | |
| R3 8.2 kΩ | D3 → 3V3 | PB3 | pull-up |
| **SD-card breakout** SCK | D13 · CN5-6 (CN10-11) | PA5 (SPI1_SCK) | |
| SD MISO | D12 · CN5-5 (CN10-13) | PA6 (SPI1_MISO) | |
| SD MOSI | D11 · CN5-4 (CN10-15) | PA7 (SPI1_MOSI) | |
| SD CS | D5 · CN9-6 (CN10-27) | PB8 | 10 kΩ pull-up to 3V3 |
| SD VCC / GND | 3V3 / GND | — | 100 nF + 10 µF across VCC–GND |
| **Status LED** (power-button LED) anode | 470 Ω → D6 · CN9-7 (CN10-25) | PB10 | |
| Status LED cathode | GND | — | |

## 2 · Switched sensor rail (VSENS)

| Net / part | Nucleo pin | MCU pin | Notes |
|---|---|---|---|
| Rail enable | D4 · CN9-5 (CN10-29) | PB5 | drives Q1 |
| Q1 high-side switch (P-MOSFET) | source → 3V3, gate → D4 (+100 kΩ gate→3V3), drain → VSENS | — | VSENS feeds LWS white, 10HS white, 7911 yellow |

## 3 · Power

| Net / part | Connection | Notes |
|---|---|---|
| Battery pack | 1S 2P Li-ion (2 × 3.7 V / 6600 mAh) | |
| Battery + | INA219 VIN+ | |
| INA219 VIN− | **power switch (physical on/off button)** input | |
| Power switch output | **E5V** · morpho **CN7-6** | jumper JP4 (5V_SEL) on **E5V** |
| Battery − | GND | |
| Charger / solar controller | across battery + / − | |
| Alternative low-power input | STD_ALONE_5V · CN11-1 (GND CN11-2), JP4 on ALONE | leaves ST-LINK unpowered |
| ST-LINK USB | CN1 (micro-USB) | programming + console; VCP ↔ PA2 (USART2_TX) / PA3 (USART2_RX) via JP8 |

## 4 · Radio

| Net / part | Connection | MCU pin |
|---|---|---|
| **915 MHz antenna** (AU915) | SMA **CN12** | → on-board RF switch → RFI_P / RFI_N (RX), RFO_LP / RFO_HP (TX) |
| RF switch control (on-board) | — | PC3 (FE_CTRL3), PC4 (FE_CTRL1), PC5 (FE_CTRL2) |

## 5 · On-board buttons used by firmware

| Button | MCU pin | Function |
|---|---|---|
| B1 | PA0 (CN10-1) | sleep off (console live) |
| B2 | PA1 (CN10-36) | sleep on (normal schedule) |
| B3 (reset) | NRST | reset |

## 6 · Grove Base Shield V2 sockets (selector at 3.3 V)

| Socket | Carries | Used by |
|---|---|---|
| I²C ×2 | PA12 / PA11 | BME280 #1, INA219 |
| A0 | A0 + A1 | LWS (A0), 10HS (A1) |
| A3 | A3 + A4 | 7911 vane (A3), 7911 speed (A4) |
| D3 | D3 (+ D4) | rain gauge (D3) — D4 is the VSENS gate, keep clear |
