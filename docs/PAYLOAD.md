# EnviroNode-WL55 — LoRaWAN Payload Specification (v1)

Bidirectional. **Uplink** carries a full sensor frame; **downlink** carries
configuration/commands. All multi-byte integers are **little-endian** unless
noted. Keep the uplink ≤ ~40 bytes so it fits the smaller AU915 data rates.

Two downlink transports coexist:
- **ASCII config string** — any FPort, first byte `{`. Selects the sensor set and
  the interval. Full reference: **[CONFIG.md](CONFIG.md)**.
- **Binary command table** — FPort 10, byte 0 = command id. Byte-efficient
  commands (calibration, reboot, one-shot uplink). Documented below.

---

## Uplink — sensor frame (FPort 1)

`fmt = 0x02`. Fixed 32-byte layout. Scaled integers avoid floats on-air and in
the decoder. *(fmt 0x01 was the 30-byte layout without battery current —
retired 2026-08-17; a decoder that accepts both keys on byte 0.)*

| Off | Field | Type | Encoding | Unit / range |
|----:|---|---|---|---|
| 0 | `fmt` | u8 | `0x02` | frame-format id |
| 1 | `status` | u8 | bitfield (below) | sensor-ok / error flags |
| 2 | `batt_mV` | u16 | raw | mV (0–65535) |
| 4 | `air1_temp` | i16 | ×100 | °C (−327.68…327.67) |
| 6 | `air1_rh` | u8 | ×2 | %RH (0–100, 0.5 steps) |
| 7 | `air1_press` | u16 | ×10 | hPa (0–6553.5) |
| 9 | `air2_temp` | i16 | ×100 | °C |
| 11 | `air2_rh` | u8 | ×2 | %RH |
| 12 | `air2_press` | u16 | ×10 | hPa |
| 14 | `soil_moist` | u16 | raw or ‰ | 0–4095 raw, or 0–1000 permille |
| 16 | `leaf_wet` | u16 | raw or ‰ | 0–4095 raw, or 0–1000 permille |
| 18 | `soil_temp` | i16 | ×100 | °C (PT1000) |
| 20 | `wind_speed` | u16 | ×100 | m/s (0–655.35) |
| 22 | `wind_dir` | u16 | ×10 | degrees (0–3599 = 0–359.9°) |
| 24 | `wind_gust` | u16 | ×100 | m/s (max over interval) |
| 26 | `rain_tips` | u16 | raw | bucket tips this interval |
| 28 | `rain_mm` | u16 | ×100 | mm this interval (derived) |
| 30 | `batt_mA` | i16 | raw mA | battery current, **discharge positive**, charging negative; `0x7FFF` = no INA219 fitted |

**`status` bitfield** (bit set = OK unless noted):
`b0` air1 ok · `b1` air2 ok · `b2` soil-moist ok · `b3` leaf ok · `b4` PT1000 ok ·
`b5` wind ok · `b6` rain ok · `b7` **fault** (any sensor error this frame — check
per-sensor logic / a follow-up diagnostic uplink).

> A sensor whose ok-bit is 0 should send a sentinel (e.g. `0x7FFF` for i16,
> `0xFFFF` for u16) so the decoder can render "no data" instead of a wrong 0.

**Deselected sensors.** The frame layout is fixed regardless of the configured
sensor set ([CONFIG.md](CONFIG.md)): a sensor that is switched off sends its
sentinel with its ok-bit clear and does **not** set `b7 fault` — nothing is
broken, it simply was not asked for. Only a *failed* read raises `b7`.
`batt_mV` and `batt_mA` are never selectable and are always present (the
current carries its own `0x7FFF` sentinel instead of a status bit — the status
byte is full).

### TTN JavaScript decoder (starter)
```js
function decodeUplink(input) {
  const b = input.bytes, dv = new DataView(new Uint8Array(b).buffer);
  if (input.fPort !== 1 || (b[0] !== 0x01 && b[0] !== 0x02))
    return { warnings: ["unknown frame"], data: {} };
  const i16 = o => dv.getInt16(o, true), u16 = o => dv.getUint16(o, true);
  const st = b[1];
  // Gate every block on its status OK-bit: a deselected or failed sensor
  // sends sentinels, and decoding those unconditionally renders plausible
  // garbage (655.35 m/s wind, 327.67 °C soil). null = "no data".
  const data = {
    status: st,
    fault:  !!(st & 0x80),
    batt_V: u16(2) / 1000,
    air1: (st & 0x01) ? { t: i16(4)/100,  rh: b[6]/2,  p: u16(7)/10 }  : null,
    air2: (st & 0x02) ? { t: i16(9)/100,  rh: b[11]/2, p: u16(12)/10 } : null,
    soil_moisture: (st & 0x04) ? u16(14) : null,
    leaf_wetness:  (st & 0x08) ? u16(16) : null,
    soil_temp:     (st & 0x10) ? i16(18)/100 : null,
    wind: (st & 0x20) ? { speed: u16(20)/100, dir: u16(22)/10, gust: u16(24)/100 } : null,
    rain: (st & 0x40) ? { tips: u16(26), mm: u16(28)/100 } : null,
  };
  if (b[0] >= 0x02 && b.length >= 32 && i16(30) !== 0x7FFF)
    data.batt_mA = i16(30);   // discharge positive, charging negative
  return { data };
}
```

### Auxiliary uplinks

**Live today (inherited KoreroNet power frames, sent on the radio core's
default app port — not FPort 1):**
- `nucleo send power` → 7 bytes: `0x01`, SoC_i %, SoC_v %, voltage in
  centi-volts (u16 LE), current in mA (i16 LE).
- `nucleo send power history` → 26 bytes: `0x02`, `24`, then 24 hourly SoC
  bytes. A decoder must branch on the first byte; neither frame is part of the
  fmt-0x02 schema above.

**Planned:**
- **FPort 2 — diagnostics:** reset cause, uptime, per-sensor error counts, RSSI/SNR
  of last downlink, firmware version. (Mirrors KoreroNet's `nucleo report` idea.)
- **FPort 3 — event/alarm:** threshold crossings (e.g. rain rate, low battery).

---

## Downlink A — sensor-set config string (any FPort, first byte `{`)

If the first byte of a downlink payload is `{` (0x7B), the payload is the ASCII
sensor-set configuration string, whatever FPort it arrived on. It selects which
sensors are measured and the cycle interval:

```
{LW,T1,T2,SM,ST,WS,WD,R,15}        canonical form
{ALL,15}   {NONE}   {+R}   {-LW,-WD}   {5}   {?}
```

Grammar, key table, replace-vs-edit semantics, all-or-nothing rejection rules,
worked examples and the hex/base64 to paste into TTN:
**[CONFIG.md](CONFIG.md)** — the authoritative reference. It also documents the
identical console form (`nucleo set {…}` or a bare `{…}` line).

Frames not starting with `{` fall through to the binary table below.

## Downlink B — binary command table (FPort 10)

Byte 0 = command id, followed by that command's args (little-endian). Unknown ids
are rejected (and, once the diag uplink exists, NAK'd). Applied on receipt;
config that must survive a reset goes to the **flash config page** (page 62).

| Cmd | Name | Args | Effect |
|----:|---|---|---|
| `0x01` | `set_interval` | u16 minutes | sampling + uplink period (1..999, clamped) |
| `0x02` | `uplink_now` | — | take a reading and uplink immediately |
| `0x03` | `reset_rain` | — | zero the rain-tip accumulator |
| `0x04` | `set_cal` | u8 sensor_id, i16 offset (×100) | additive calibration offset |
| `0x05` | `set_winddir_offset` | u16 deg×10 | vane north-alignment offset |
| `0x06` | `set_enable` | u8 mask *(u16 accepted, high byte ignored)* | sensor set — same 8-bit mask as the config string |
| `0x07` | `reboot` | — | software reset |
| `0x08` | `get_config` | — | echo current config in the next diag uplink |

**`sensor_id` for `set_cal`:** `1` air1_t · `2` air1_rh · `3` air2_t · `4` air2_rh ·
`5` soil_moist · `6` leaf · `7` soil_temp · `8` wind_dir.

**`0x06 set_enable` mask** — one bit per sensor, the same bit order as the config
string's keys ([CONFIG.md](CONFIG.md)). `0x06` is exactly a full-replace config
string with no interval token; both write the same stored field.

| Bit | 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80 |
|---|---|---|---|---|---|---|---|---|
| Key | `LW` | `T1` | `T2` | `SM` | `ST` | `WS` | `WD` | `R` |
| Status bit set when read OK | `SENS_OK_LEAF` | `SENS_OK_AIR1` | `SENS_OK_AIR2` | `SENS_OK_SOIL` | `SENS_OK_PT1000` | `SENS_OK_WIND` | `SENS_OK_WIND` | `SENS_OK_RAIN` |

`ALL` = `0xFF`, `NONE` = `0x00`. Note the mask's bit order is **not** the `status`
byte's order — `WS` and `WD` are separate mask bits but share one status bit.
A config page written by a pre-config-string build used the old 7-bit
`SENS_OK_*`-ordered mask; re-send `{ALL,15}` (or `0x06 0xFF`) once after upgrading.

### Implementation status
Implemented in `sensors/envnode_payload.c` (`envnode_downlink_apply`) and driven
automatically: `EnvNode_DrainDownlinks()` runs from the CM4 main loop, pulls each
frame out of the CM0+ ring, routes payloads starting with `{` to the config-string
parser and everything else on FPort 10 to the command table, echoing the result on
the console (`DL: cmd 0x01 (2 args) -> applied`, or the accepted canonical config
string). Frames that match neither are printed as hex rather than dropped.

`0x02 uplink_now` sets a flag that the scheduler services on its next pass — an
uplink is never started from inside the downlink drain, and it is honoured even
when the sensor set is `NONE` (which otherwise parks the periodic uplink, see
[CONFIG.md](CONFIG.md)). `0x01` and `0x06` echo the resulting canonical config
string on the console, the same way an ASCII config string does. `0x08 get_config` is the
one command still unimplemented; it needs the FPort-2 diagnostic uplink (so a
`{?}` arriving by downlink is answered on the console only, for now).

Config written by `0x01/0x04/0x05/0x06` or by an accepted config string is saved
to a dedicated **flash page** (not backup registers), so it survives a power cut.
A flash erase stalls the CM0+ core for ~20–40 ms; that is why config is only ever
written on an explicit command, and why an accepted config string that changes
nothing does not write at all.

### Reuse from KoreroNet
The CM0+ radio core already **stores downlinks in a ring** and CM4 drains them
(the `dl_*` mailbox fields), and it honours `mb->port`, so the sensor frame goes
out on FPort 1. OTAA join, key persistence, and the join-status mailbox flag all
carry over unchanged — with the identity now **also mirrored to flash** so the
AppKey survives a full power loss (`envnode_keystore.c`).

---

## Sizing / region notes
- The 32-byte uplink fits AU915 **DR3+** while the region's default 400 ms
  uplink dwell time applies (DR2 then carries only 11 B); with dwell disabled
  it fits every DR. If you must fit lower rates, split
  into two frames (air block / weather block) or drop the derived `rain_mm`.
- Uplink cadence is a power/airtime trade-off — default **every 15 min**
  (`set_interval`), with `uplink_now` for on-demand reads. Respect the regional
  duty-cycle / fair-use policy.
