# EnviroNode-WL55 — Sensor-set configuration string (SPEC v1)

The node is told **what to measure** and **how often** with a single ASCII string
in braces, delivered either as a LoRaWAN downlink or over the ST-Link console.
This is the whole runtime configuration surface for the measurement cycle; it
**replaces the inherited KoreroNet "timetable"**, which no longer exists.

Ground truth pairs with [PINOUT.md](PINOUT.md) (pins), [PAYLOAD.md](PAYLOAD.md)
(on-air bytes) and [SENSORS.md](SENSORS.md) (drivers). The string never changes
the uplink frame layout — see [Effect on the uplink](#effect-on-the-uplink).

---

## Grammar

```
config  := '{' token (',' token)* '}'
token   := key | '+' key | '-' key | setalias | interval | '?'
key     := LW | T1 | T2 | SM | ST | WS | WD | R
setalias:= ALL | NONE
interval:= 1..999            ; minutes between measure+uplink cycles
```

- **Case-insensitive.** All whitespace is ignored, anywhere.
- Order does not matter; duplicate keys are harmless.
- The payload/line is the literal ASCII of the string — no length prefix, no
  terminator, no escaping.

## Sensor keys

| Key | Bit | Measurement | Driver | Bus / pin | Uplink field (offset) | Status bit |
|---|---|---|---|---|---|---|
| `LW` | `1<<0` = 0x01 | Leaf wetness (**Decagon LWS**) | `analog_sensors` | ADC_IN5 — PB1 (**A0**) | `leaf_wet` (16) | `SENS_OK_LEAF` |
| `T1` | `1<<1` = 0x02 | Air temp / RH / pressure #1 | `bme280` | **I²C2** — PA12 SCL / PA11 SDA (Grove shield) | `air1_temp/rh/press` (4 / 6 / 7) | `SENS_OK_AIR1` |
| `T2` | `1<<2` = 0x04 | Air temp / RH / pressure #2 | `bme280` | **I²C1** — PA9 SCL / PA10 SDA (board pins) | `air2_temp/rh/press` (9 / 11 / 12) | `SENS_OK_AIR2` |
| `SM` | `1<<3` = 0x08 | Soil moisture | `analog_sensors` | ADC_IN4 — PB2 (A1) | `soil_moist` (14) | `SENS_OK_SOIL` |
| `ST` | `1<<4` = 0x10 | Soil temperature (PT1000) | `max31865` | SPI1 — PA5/PA6/PA7, CS PA4 | `soil_temp` (18) | `SENS_OK_PT1000` |
| `WS` | `1<<5` = 0x20 | Wind speed (+ gust) | `analog_sensors` (3 s ADC burst) | ADC_IN1 — PB14 (**A4**) | `wind_speed` (20), `wind_gust` (24) | `SENS_OK_WIND` |
| `WD` | `1<<6` = 0x40 | Wind direction | `analog_sensors` | ADC_IN3 — PB4 (A3) | `wind_dir` (22) | `SENS_OK_WIND` |
| `R`  | `1<<7` = 0x80 | Rainfall | `pulse_counter` | EXTI3 — PB3 (D3) | `rain_tips` (26), `rain_mm` (28) | `SENS_OK_RAIN` |

`ALL` = 0xFF, `NONE` = 0x00.

**Battery is not selectable.** `batt_mV` (offset 2, ADC_IN1 on PB14 plus the
INA219 on I²C2) is measured and sent on every cycle regardless of the set — it is
the one channel you cannot afford to lose remotely.

**`WS` and `WD` share one status bit** (`SENS_OK_WIND`, b5). Selecting only `WD`
sets b5 from the vane read and sends the sentinel for speed/gust; selecting only
`WS` does the converse.

## Interval

A bare integer, **1..999 minutes**, is the period between measure+uplink cycles.
Default 15. Outside that range the whole frame is rejected (see below).

> Every path shares this range: the brace string rejects anything outside it, and
> the binary downlink `0x01 set_interval` (u16) plus the console `nucleo interval
> <min>` **clamp** to the same 1..999 (`ENVCFG_INTERVAL_MIN_MAX`). The string is
> capped at three digits so `999` can never be a typo'd key.

## What a cycle does

1. The scheduler fires when the interval has elapsed (first frame ~60 s after boot).
   With the set at `NONE` the cycle is **parked** — see [`{NONE}`](#none-parks-the-cycle).
2. Every **selected** sensor is read. A **deselected** sensor is skipped entirely:
   its OK-bit stays clear and it raises **no** fault, because nothing is broken.
3. Battery is read unconditionally.
4. The 30-byte FPort-1 frame is packed (sentinels for skipped/failed channels) and
   handed to the CM0+ radio core.
5. Downlinks are drained and applied.
6. Rain/wind accumulators are **consumed** by the sample that feeds the uplink.
   (`nucleo sensors` *peeks* — it never steals the interval the next uplink owns.)

## Replace vs. edit

| Frame contains | Behaviour |
|---|---|
| any plain key, or `ALL` / `NONE` | **REPLACES** the sensor set with those keys; any `+`/`-` tokens in the same frame are then applied on top |
| only `+`/`-` tokens and/or an interval | **EDITS** the current set in place |
| an interval alongside keys | set is replaced/edited *and* the interval is changed |
| `?` | reports the current configuration (`?` never modifies anything) |

Examples: `{T1,T2,+R}` → set becomes exactly `{T1,T2,R}`. `{+R}` → whatever was
selected, plus rain. `{NONE,+R}` → rain only.

## Rejection rules — all or nothing

A frame is rejected **in full**, with **nothing applied and no flash written**, if it:

- contains an unknown token,
- has missing or unbalanced braces, or nothing between them (`{}`),
- carries an interval outside 1..999.

The report names the offending token, e.g.
`ERR: config rejected -- bad token 'XX' (nothing applied)`.

A partially-applied remote config is worse than a rejected one: a node that
half-took a downlink is a node whose real state nobody knows.

## Canonical rendering

Fixed key order, then the interval:

```
{LW,T1,T2,SM,ST,WS,WD,R,15}
```

An empty set renders `{NONE,15}`. This is what `?`, `info` and every accept/echo
print, so the string you read back can be pasted straight into another node.

## Effect on the uplink

**The 30-byte FPort-1 frame layout never changes** — offsets are fixed whatever
the sensor set. A deselected sensor sends its sentinel (`0x7FFF` for i16,
`0xFFFF` for u16, `0xFF` for u8) with its OK-bit clear, so the TTN decoder in
[PAYLOAD.md](PAYLOAD.md) renders "no data" rather than a plausible-looking zero.
`b7 fault` is **not** set for a deselected sensor.

## Power implication

Rain (`R`) is counted as **EXTI edges**, so the node must stay awake between
cycles to see tips. Everything else — **including wind speed, which is ADC
burst-sampled since 2026-08-04** — is measured on demand inside the awake window.

| Selection | May sleep between cycles? | Reason |
|---|---|---|
| contains `R` | **no** | rain tips are edge-counted on EXTI3 and must never be missed |
| any other selection (incl. `WS`) | yes | sampled on demand; wind is a 3 s ADC burst per cycle |

The firmware exposes this as a **predicate plus a human-readable reason string**
so the console (`info`, `?`) can explain *why* a node will not sleep.

> **STOP2 sleep is implemented** (`envnode_power.c`). Between cycles the CM4 core
> is stopped and woken by the RTC, in watchdog-safe chunks, with the HAL tick
> advanced on wake. Only **`R`** blocks it — wind speed became burst-sampled on
> 2026-08-04 and no longer pins the core awake;
> `nucleo sleep off` disables it for bench work. Details and the remaining power
> work: [LOGBOOK.md §6.4](LOGBOOK.md#64-sleep-and-power).
>
> **Factory default: `{T1,T2,1}`** — the two air sensors on a 1-minute cycle,
> which is the first-warm-test setting. It is deliberately short so a bench
> operator sees frames quickly; ⚠️ 1-minute uplinks exceed the TTN fair-use
> allowance, so raise the interval before leaving a node running.

## Worked examples

| String | Effect |
|---|---|
| `{ALL,15}` | every sensor, 15-minute cycle (the normal field setting) |
| `{NONE}` | stop measuring **and** stop the periodic uplink (see below) |
| `{+R}` | add rainfall to the current set; interval untouched |
| `{-LW,-WD}` | drop leaf wetness and wind direction; keep everything else |
| `{5}` | keep the current set, cycle every 5 minutes |
| `{?}` | reply with the canonical string, e.g. `{T1,T2,ST,60}` — changes nothing |
| `{T1,T2,ST,60}` | replace the set with air ×2 + soil temp; hourly. **May sleep** (no `R`/`WS`) |
| `{ALL,-T2,10}` | everything except air #2, every 10 min |
| `{NONE,+R,+WS,5}` | rain + wind speed only, every 5 min. Stays awake — **`R` is edge-counted** (`WS` is burst-sampled and would sleep on its own) |
| `{sm,st, 30}` | accepted — case and whitespace are ignored → `{SM,ST,30}` |
| `{T1,XX}` | **rejected**: unknown token `XX`. `T1` is *not* applied |
| `{T1,1000}` | **rejected**: interval out of range 1..999 |
| `{T1,T2` | **rejected**: unbalanced braces |
| `{}` | **rejected**: empty frame |

## How to send it

### LoRaWAN downlink (TTN)

**Any FPort.** If the first payload byte is `{` (0x7B) the payload is treated as a
config string; otherwise it falls through to the binary command table on FPort 10
([PAYLOAD.md](PAYLOAD.md)). Send the raw ASCII bytes — no `fmt` byte, no NUL.

| String | Hex payload | Base64 |
|---|---|---|
| `{ALL,15}` | `7B 41 4C 4C 2C 31 35 7D` | `e0FMTCwxNX0=` |
| `{+R}` | `7B 2B 52 7D` | `eytSfQ==` |
| `{?}` | `7B 3F 7D` | `ez99` |
| `{T1,T2,ST,60}` | `7B 54 31 2C 54 32 2C 53 54 2C 36 30 7D` | `e1QxLFQyLFNULDYwfQ==` |

Any ASCII→hex converter produces the rest. The longest canonical string is 28
bytes, which fits an AU915 RX2 (DR8) downlink comfortably — but check your
region's payload limit before inventing long strings.

`{?}` over the air is accepted, but the reply currently only reaches the console:
the FPort-2 diagnostic uplink that would carry it is still open work (see
[ROADMAP.md](ROADMAP.md) Phase 4).

### Console (USART2 → ST-Link VCP, or USART1; 115200 8N1)

```
nucleo set {ALL,15}
{T1,T2,ST,60}          <- a bare brace line works too
{?}                    <- print the canonical string
info                   <- also prints the canonical string + sleep verdict
```

An accepted line echoes the canonical string, whether it was saved, and the sleep
verdict; a rejected one names the offending token and changes nothing:

```
> nucleo set {T1,T2,ST,60}
ACK: config {T1,T2,ST,60} saved
ACK: no edge-counted sensors selected: may sleep between cycles  (STOP2 sleep is Phase 5, not implemented)

> {T1,XX}
ERR: config rejected -- bad token 'XX' (nothing applied)
```

A config string that arrives by **downlink** prints the same two lines on the
console, prefixed by `DL: config string on FPort <n>`, so whoever is standing at
the node can see what the gateway changed. The current set is also printed at
boot (`CONFIG: {…}`) and by `info`.

## Persistence

The sensor mask and the interval live in the **existing flash config page**
(page 62 @ `0x0801F000`, `Core/WL55JC1/Src/envnode_config.c`) and are reloaded at
boot, so a node keeps its configuration through a power cut.

Flash is written **only when a frame is accepted AND something actually changed**.
A repeated identical config, a `?` query, or a rejected frame writes nothing.

> **Relation to `0x06 set_enable`:** the stored mask *is* the sensor-set mask, in
> the `SENSOR_*` bit order above (`ALL` = 0xFF). The binary `0x06` command writes
> the same field, so it is exactly a full-replace config string with no interval.
> A node carrying a config page written by an older build (7-bit mask in
> `SENS_OK_*` order, `ALL` = 0x7F) must be re-configured once with `{ALL,15}`.

## Gotchas

- **Never parse the normalized command buffer.** `normalize_cmd()` in `main.c`
  strips `,` `.` `!` `?` `:` `;` `'` `"` and all whitespace, and lowercases the
  rest — it turns `{T1,T2,15}` into `{t1t215}` and `{?}` into `{}`. The brace
  string **must** be parsed from the **raw** console line.
- **All-or-nothing.** A frame with one bad token applies nothing at all. Read the
  echo; do not assume a downlink landed.
- **Flash timing.** A config write erases a flash page, which stalls the CM0+
  radio core's instruction fetches for ~20–40 ms. That is why config is only ever
  written on an explicit accepted change — never on a timer, and never from inside
  a LoRaWAN RX window.
- <a id="none-parks-the-cycle"></a>**`{NONE}` parks the measurement cycle.** With
  nothing selected every field but the battery would be a sentinel, so the
  periodic uplink stops rather than paying airtime for an empty frame. The
  console says so **once** (`INFO: sensor set is {NONE} - periodic uplinks
  paused`), not every pass. `nucleo uplink now` and downlink `0x02 uplink_now`
  still send a (battery-only) frame on demand, and the moment a sensor is
  selected again one frame goes out immediately so you can see the change land.
- **`{NONE}` does not stop the ISRs.** Rain/wind EXTI lines stay armed and the
  counters keep accumulating; they simply are not reported.
- **Brace lines are the sensor set now.** The old `nucleo timetable {…}{…}`
  command is gone along with the rest of the Pi/recording logic.
- **One interval limit, three doors.** 1..999 minutes whether it arrives as a
  brace string, as binary `0x01`, or as `nucleo interval <min>` — same stored
  field, same clamp. The brace string *rejects* an out-of-range value; the other
  two *clamp* it, because a 16-bit argument has no way to say "reject".
