# EnviroNode-WL55 — LoRaWAN Payload Specification (v1)

Bidirectional. **Uplink** carries a full sensor frame; **downlink** carries
configuration/commands. All multi-byte integers are **little-endian** unless
noted. Keep the uplink ≤ ~40 bytes so it fits the smaller AU915 data rates.

---

## Uplink — sensor frame (FPort 1)

`fmt = 0x01`. Fixed 30-byte layout. Scaled integers avoid floats on-air and in
the decoder.

| Off | Field | Type | Encoding | Unit / range |
|----:|---|---|---|---|
| 0 | `fmt` | u8 | `0x01` | frame-format id |
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

**`status` bitfield** (bit set = OK unless noted):
`b0` air1 ok · `b1` air2 ok · `b2` soil-moist ok · `b3` leaf ok · `b4` PT1000 ok ·
`b5` wind ok · `b6` rain ok · `b7` **fault** (any sensor error this frame — check
per-sensor logic / a follow-up diagnostic uplink).

> A sensor whose ok-bit is 0 should send a sentinel (e.g. `0x7FFF` for i16,
> `0xFFFF` for u16) so the decoder can render "no data" instead of a wrong 0.

### TTN JavaScript decoder (starter)
```js
function decodeUplink(input) {
  const b = input.bytes, dv = new DataView(new Uint8Array(b).buffer);
  if (input.fPort !== 1 || b[0] !== 0x01) return { warnings: ["unknown frame"], data: {} };
  const i16 = o => dv.getInt16(o, true), u16 = o => dv.getUint16(o, true);
  const st = b[1];
  return { data: {
    status: st,
    batt_V:      u16(2) / 1000,
    air1:  { t: i16(4)/100,  rh: b[6]/2,  p: u16(7)/10 },
    air2:  { t: i16(9)/100,  rh: b[11]/2, p: u16(12)/10 },
    soil_moisture: u16(14),
    leaf_wetness:  u16(16),
    soil_temp:     i16(18)/100,
    wind: { speed: u16(20)/100, dir: u16(22)/10, gust: u16(24)/100 },
    rain: { tips: u16(26), mm: u16(28)/100 },
  }};
}
```

### Auxiliary uplinks (optional, later)
- **FPort 2 — diagnostics:** reset cause, uptime, per-sensor error counts, RSSI/SNR
  of last downlink, firmware version. (Mirrors KoreroNet's `nucleo report` idea.)
- **FPort 3 — event/alarm:** threshold crossings (e.g. rain rate, low battery).

---

## Downlink — config & commands (FPort 10)

Byte 0 = command id, followed by that command's args. Unknown ids are ignored
(and, if a diag uplink is enabled, NAK'd). Apply on receipt; persist config that
should survive a reset in the **RTC backup registers** (as KoreroNet does).

| Cmd | Name | Args | Effect |
|----:|---|---|---|
| `0x01` | `set_interval` | u16 minutes | sampling + uplink period |
| `0x02` | `uplink_now` | — | take a reading and uplink immediately |
| `0x03` | `reset_rain` | — | zero the rain-tip accumulator |
| `0x04` | `set_cal` | u8 sensor_id, i16 offset (×100) | additive calibration offset |
| `0x05` | `set_winddir_offset` | u16 deg×10 | vane north-alignment offset |
| `0x06` | `set_enable` | u16 mask | enable/disable sensors (bit per sensor) |
| `0x07` | `reboot` | — | software reset |
| `0x08` | `get_config` | — | echo current config in the next diag uplink |

**`sensor_id` for `set_cal`:** `1` air1_t · `2` air1_rh · `3` air2_t · `4` air2_rh ·
`5` soil_moist · `6` leaf · `7` soil_temp · `8` wind_dir.

### Reuse from KoreroNet
The CM0+ radio core already **stores downlinks in a ring** and CM4 drains them
(`Korero_ServeDownlinks` / the `dl_*` mailbox fields). EnviroNode reuses that path:
CM0+ receives on FPort 10 → CM4 parses the command table above → applies + persists.
OTAA join, key persistence in backup registers, and the join-status mailbox flag
all carry over unchanged.

---

## Sizing / region notes
- 30-byte uplink fits AU915 **DR2+** (and US915). If you must fit DR0/DR1, split
  into two frames (air block / weather block) or drop the derived `rain_mm`.
- Uplink cadence is a power/airtime trade-off — default **every 15 min**
  (`set_interval`), with `uplink_now` for on-demand reads. Respect the regional
  duty-cycle / fair-use policy.
