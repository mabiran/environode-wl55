#!/usr/bin/env python3
"""
EnviroNode-WL55 — remote dashboard (Streamlit, single file).

Graphically monitors and controls the LoRaWAN sensor node through The Things
Network: live sensor data + history from the TTN Storage integration, and every
remote command the firmware accepts (sensor selection, interval, calibration,
wind-vane offset, reset rain, uplink-now, reboot, raw config strings and raw
bytes) sent as Class-A downlinks.

Runs identically as a local app (`streamlit run app.py`) and on a Hugging Face
Space (this file as app.py + requirements.txt). No secret is ever hard-coded —
credentials come from st.secrets, environment variables, or the sidebar.

Ground truth (must match the node firmware — docs/PAYLOAD.md, docs/CONFIG.md):
  Uplink FPort 1, fmt 0x02, 32 bytes, little-endian.
  Config-string downlink: ASCII "{...}" on any FPort (we use FPort 1).
  Binary command downlink: FPort 10, byte0 = command id.

Self-test (no network, no Streamlit):  python app.py --selftest
"""

import os
import sys
import json
import base64
import struct
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Constants — the on-air contract with the firmware.
# ---------------------------------------------------------------------------
DEFAULT_REGION = "au1"
UPLINK_FPORT = 1
CONFIG_FPORT = 1          # a downlink starting with '{' is a config string on any port
COMMAND_FPORT = 10        # binary command table

# status byte bits (b7 = fault)
STATUS_BITS = [
    ("air1", 0x01), ("air2", 0x02), ("soil", 0x04), ("leaf", 0x08),
    ("soil_temp", 0x10), ("wind", 0x20), ("rain", 0x40), ("fault", 0x80),
]

# sensor-set keys -> selection-mask bit (docs/CONFIG.md, envnode_sensorset.h)
SENSOR_KEYS = [
    ("LW", 0x01, "Leaf wetness (A0)"),
    ("T1", 0x02, "Air #1 T/RH/P (I2C2, shield)"),
    ("T2", 0x04, "Air #2 T/RH/P (I2C1, board pins)"),
    ("SM", 0x08, "Soil moisture 10HS (A1)"),
    ("ST", 0x10, "Soil temperature PT1000 (CN7-17)"),
    ("WS", 0x20, "Wind speed (A4, burst)"),
    ("WD", 0x40, "Wind direction (A3)"),
    ("R",  0x80, "Rain gauge (D3) - blocks sleep"),
]

# binary command ids (docs/PAYLOAD.md)
CMD_SET_INTERVAL = 0x01
CMD_UPLINK_NOW = 0x02
CMD_RESET_RAIN = 0x03
CMD_SET_CAL = 0x04
CMD_SET_WINDDIR = 0x05
CMD_SET_ENABLE = 0x06
CMD_REBOOT = 0x07

# set_cal sensor ids (offset applied as x100, little-endian i16)
CAL_SENSORS = [
    (1, "Air #1 temperature (deg C)"),
    (2, "Air #1 humidity (%RH)"),
    (3, "Air #2 temperature (deg C)"),
    (4, "Air #2 humidity (%RH)"),
    (5, "Soil moisture (counts)"),
    (6, "Leaf wetness (counts)"),
    (7, "Soil temperature (deg C)"),
    (8, "Wind direction (deg)"),
]

INTERVAL_MIN, INTERVAL_MAX = 1, 999


# ---------------------------------------------------------------------------
# Uplink decoder — mirrors the TTN JS formatter byte-for-byte (docs/PAYLOAD.md).
# ---------------------------------------------------------------------------
def decode_uplink(b):
    """Decode a raw 30/32-byte FPort-1 frame into the same nested dict TTN's
    JavaScript formatter produces. Returns None for an unrecognised frame."""
    if b is None or len(b) < 30 or b[0] not in (0x01, 0x02):
        return None

    def i16(o):
        return struct.unpack_from("<h", b, o)[0]

    def u16(o):
        return struct.unpack_from("<H", b, o)[0]

    st = b[1]
    d = {
        "status": st,
        "fault": bool(st & 0x80),
        "batt_V": u16(2) / 1000.0,
        "air1": {"t": i16(4) / 100.0, "rh": b[6] / 2.0, "p": u16(7) / 10.0} if st & 0x01 else None,
        "air2": {"t": i16(9) / 100.0, "rh": b[11] / 2.0, "p": u16(12) / 10.0} if st & 0x02 else None,
        "soil_moisture": u16(14) if st & 0x04 else None,
        "leaf_wetness": u16(16) if st & 0x08 else None,
        "soil_temp": i16(18) / 100.0 if st & 0x10 else None,
        "wind": {"speed": u16(20) / 100.0, "dir": u16(22) / 10.0, "gust": u16(24) / 100.0} if st & 0x20 else None,
        "rain": {"tips": u16(26), "mm": u16(28) / 100.0} if st & 0x40 else None,
    }
    if b[0] >= 0x02 and len(b) >= 32 and i16(30) != 0x7FFF:
        d["batt_mA"] = i16(30)
    return d


def flatten_decoded(d):
    """Nested decoded dict -> flat column dict for tables/charts. Accepts both
    our decoder's output and TTN's decoded_payload (same shape)."""
    if not d:
        return {}
    out = {"status": d.get("status"), "fault": d.get("fault"), "batt_V": d.get("batt_V"),
           "batt_mA": d.get("batt_mA")}
    a1 = d.get("air1") or {}
    a2 = d.get("air2") or {}
    w = d.get("wind") or {}
    r = d.get("rain") or {}
    out.update({
        "air1_t": a1.get("t"), "air1_rh": a1.get("rh"), "air1_p": a1.get("p"),
        "air2_t": a2.get("t"), "air2_rh": a2.get("rh"), "air2_p": a2.get("p"),
        "soil_moisture": d.get("soil_moisture"), "leaf_wetness": d.get("leaf_wetness"),
        "soil_temp": d.get("soil_temp"),
        "wind_speed": w.get("speed"), "wind_dir": w.get("dir"), "wind_gust": w.get("gust"),
        "rain_tips": r.get("tips"), "rain_mm": r.get("mm"),
    })
    return out


# ---------------------------------------------------------------------------
# Downlink encoders — produce the exact bytes the firmware parses.
# ---------------------------------------------------------------------------
def enc_config_string(s):
    """A sensor-set/interval config string, e.g. '{T1,T2,15}'. FPort 1."""
    s = s.strip()
    if not (s.startswith("{") and s.endswith("}")):
        raise ValueError("config string must be wrapped in { }")
    return s.encode("ascii"), CONFIG_FPORT


def enc_set_interval(minutes):
    m = int(minutes) & 0xFFFF
    return bytes([CMD_SET_INTERVAL, m & 0xFF, (m >> 8) & 0xFF]), COMMAND_FPORT


def enc_uplink_now():
    return bytes([CMD_UPLINK_NOW]), COMMAND_FPORT


def enc_reset_rain():
    return bytes([CMD_RESET_RAIN]), COMMAND_FPORT


def enc_set_cal(sensor_id, offset_x100):
    o = int(offset_x100) & 0xFFFF
    return bytes([CMD_SET_CAL, int(sensor_id) & 0xFF, o & 0xFF, (o >> 8) & 0xFF]), COMMAND_FPORT


def enc_set_winddir_offset(deg_x10):
    v = int(deg_x10) & 0xFFFF
    return bytes([CMD_SET_WINDDIR, v & 0xFF, (v >> 8) & 0xFF]), COMMAND_FPORT


def enc_set_enable(mask):
    return bytes([CMD_SET_ENABLE, int(mask) & 0xFF]), COMMAND_FPORT


def enc_reboot():
    return bytes([CMD_REBOOT]), COMMAND_FPORT


def build_config_string(selected_keys, interval=None, edits=None):
    """Compose a config string. selected_keys: list of key tokens for a full
    replace; edits: list like ['+R','-LW'] for incremental; interval: int."""
    if edits:
        body = ",".join(edits)
    else:
        body = ",".join(selected_keys)
    if interval is not None:
        body = (body + "," if body else "") + str(int(interval))
    return "{" + body + "}"


def mask_from_keys(keys):
    m = 0
    lut = {k: bit for k, bit, _ in SENSOR_KEYS}
    for k in keys:
        m |= lut.get(k, 0)
    return m


def hexstr(b):
    return b.hex().upper()


def b64(b):
    return base64.b64encode(b).decode("ascii")


# 1S Li-ion voltage -> state-of-charge %, identical to the node firmware
# (main.c soc_from_voltage, r24). 0% floors at 3.30 V, never at 0 V.
_SOC_CURVE = [(4.20, 100.0), (4.10, 90.0), (4.00, 78.0), (3.90, 62.0),
              (3.80, 45.0), (3.70, 28.0), (3.60, 12.0), (3.30, 0.0)]


def soc_from_voltage(v):
    if v is None:
        return None
    if v >= _SOC_CURVE[0][0]:
        return 100.0
    if v <= _SOC_CURVE[-1][0]:
        return 0.0
    for (v1, p1), (v2, p2) in zip(_SOC_CURVE, _SOC_CURVE[1:]):
        if v2 <= v <= v1:
            return round(p2 + (p1 - p2) * (v - v2) / (v1 - v2), 1)
    return None


# ===========================================================================
# SELF-TEST (pure Python; no Streamlit, no network) — python app.py --selftest
# ===========================================================================
def _run_selftest():
    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            ok = False
            print(f"  FAIL {name}: got {got!r} want {want!r}")
        else:
            print(f"  ok   {name}")

    print("decoder:")
    # Canonical packer vector from the firmware self-test (docs / main.c).
    frame = bytes.fromhex("0203740E66086E9427F3FDA00327FFFFFFFFFF7FFFFFFFFFFFFFFFFFFFFFEB00")
    d = decode_uplink(frame)
    check("fmt/len accepted", d is not None, True)
    check("batt_V", d["batt_V"], 3.7)
    check("air1", d["air1"], {"t": 21.5, "rh": 55.0, "p": 1013.2})
    check("air2", d["air2"], {"t": -5.25, "rh": 80.0, "p": 998.7})
    check("soil_moisture null", d["soil_moisture"], None)
    check("soil_temp null", d["soil_temp"], None)
    check("wind null", d["wind"], None)
    check("rain null", d["rain"], None)
    check("batt_mA", d["batt_mA"], 235)
    check("fault false (status 0x03)", d["fault"], False)
    check("bad frame -> None", decode_uplink(b"\x99\x00"), None)

    # A real all-sensors frame captured from TTN (status 0x3F, no rain/fault).
    real = base64.b64decode("Aj8oD/sHb/4nFghw+yfDAbwBogcAAMwCAAD/////Z/w=")
    dr = decode_uplink(real)
    check("real soil_temp ~19.54", dr["soil_temp"], 19.54)
    check("real batt_mA -921", dr["batt_mA"], -921)
    check("real air1_p 1023.8", dr["air1"]["p"], 1023.8)

    print("encoders:")
    check("set_interval 15 -> 010F00", hexstr(enc_set_interval(15)[0]), "010F00")
    check("set_interval 60 -> 013C00", hexstr(enc_set_interval(60)[0]), "013C00")
    check("uplink_now -> 02", hexstr(enc_uplink_now()[0]), "02")
    check("reset_rain -> 03", hexstr(enc_reset_rain()[0]), "03")
    check("set_cal 7 +1000 -> 0407E803", hexstr(enc_set_cal(7, 1000)[0]), "0407E803")
    check("set_cal 7 -1000 -> 040718FC", hexstr(enc_set_cal(7, -1000)[0]), "040718FC")
    check("set_winddir 60 -> 053C00", hexstr(enc_set_winddir_offset(60)[0]), "053C00")
    check("set_enable 0x7F -> 067F", hexstr(enc_set_enable(0x7F)[0]), "067F")
    check("reboot -> 07", hexstr(enc_reboot()[0]), "07")
    check("cfg {15} port", enc_config_string("{15}")[1], CONFIG_FPORT)
    check("cfg {15} bytes", hexstr(enc_config_string("{15}")[0]), "7B31357D")

    print("soc:")
    check("soc 4.20 -> 100", soc_from_voltage(4.20), 100.0)
    check("soc 3.30 -> 0", soc_from_voltage(3.30), 0.0)
    check("soc 3.90 -> 62", soc_from_voltage(3.90), 62.0)
    check("soc 2.50 floors 0", soc_from_voltage(2.50), 0.0)
    check("soc None", soc_from_voltage(None), None)

    print("builders:")
    check("build replace", build_config_string(["T1", "T2"], 15), "{T1,T2,15}")
    check("build edit", build_config_string(None, None, ["+R"]), "{+R}")
    check("build interval-only", build_config_string([], 5), "{5}")
    check("mask all-but-R", mask_from_keys(["LW", "T1", "T2", "SM", "ST", "WS", "WD"]), 0x7F)
    check("mask all", mask_from_keys([k for k, _, _ in SENSOR_KEYS]), 0xFF)

    print("regression:")
    try:
        import pandas as _pd
        _df = _pd.DataFrame([{"time": None, "batt_V": 4.0}, {"time": "2026-08-27T00:00:00Z", "batt_V": 4.1}])
        _df["time"] = _pd.to_datetime(_df["time"], errors="coerce", utc=True)
        _df = _df.dropna(subset=["time"]).sort_values("time")
        check("NaT row dropped", len(_df), 1)
        check("age int-safe", int((_pd.Timestamp.now(tz="UTC") - _df.iloc[-1]["time"]).total_seconds()) >= 0, True)
    except ImportError:
        print("  skip (pandas not installed)")

    print("PASS" if ok else "FAILURES ABOVE")
    return 0 if ok else 1


if __name__ == "__main__" and "--selftest" in sys.argv:
    sys.exit(_run_selftest())


# ===========================================================================
# TTN REST client (used only by the Streamlit UI below).
# ===========================================================================
import requests  # noqa: E402  (kept below the selftest so --selftest needs no deps beyond stdlib)


class TTN:
    def __init__(self, region, app_id, dev_id, api_key, gateway_id=""):
        self.base = f"https://{region}.cloud.thethings.network/api/v3"
        self.app = app_id
        self.dev = dev_id
        self.gw = gateway_id
        self.h = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}

    def _get(self, url, headers=None, timeout=15):
        try:
            r = requests.get(url, headers=headers or self.h, timeout=timeout)
            return r.status_code, r
        except requests.RequestException as e:
            return 0, e

    def storage_uplinks(self, limit=50):
        """Recent uplinks via the Storage integration. Returns (ok, list|error)."""
        url = (f"{self.base}/as/applications/{self.app}/devices/{self.dev}"
               f"/packages/storage/uplink_message?limit={int(limit)}&order=-received_at")
        h = dict(self.h)
        h["Accept"] = "text/event-stream"
        code, r = self._get(url, headers=h)
        if code == 0:
            return False, str(r)
        if code == 404:
            return False, "Storage integration not enabled (Integrations -> Storage Integration -> Activate)."
        if code != 200:
            return False, f"HTTP {code}: {getattr(r, 'text', '')[:300]}"
        rows = []
        for line in r.text.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            res = obj.get("result")
            if res:
                rows.append(res)
        return True, rows

    def gateway_stats(self):
        if not self.gw:
            return False, "no gateway id configured"
        url = f"{self.base}/gs/gateways/{self.gw}/connection/stats"
        code, r = self._get(url)
        if code == 0:
            return False, str(r)
        if code != 200:
            return False, f"HTTP {code}"
        return True, r.json()

    def join_info(self):
        url = (f"{self.base}/js/applications/{self.app}/devices/{self.dev}"
               f"?field_mask=last_dev_nonce,last_join_nonce,resets_join_nonces")
        code, r = self._get(url)
        if code == 0 or code != 200:
            return False, (str(r) if code == 0 else f"HTTP {code}")
        return True, r.json()

    def queue(self):
        url = f"{self.base}/as/applications/{self.app}/devices/{self.dev}/down"
        code, r = self._get(url)
        if code == 0 or code != 200:
            return False, (str(r) if code == 0 else f"HTTP {code}")
        return True, r.json().get("downlinks", [])

    def push_downlink(self, payload, fport, replace=False, confirmed=False):
        verb = "replace" if replace else "push"
        url = f"{self.base}/as/applications/{self.app}/devices/{self.dev}/down/{verb}"
        body = {"downlinks": [{
            "f_port": int(fport),
            "frm_payload": b64(payload),
            "priority": "NORMAL",
            "confirmed": bool(confirmed),
        }]}
        try:
            r = requests.post(url, headers=self.h, data=json.dumps(body), timeout=15)
        except requests.RequestException as e:
            return False, str(e)
        if r.status_code == 200:
            return True, "queued"
        return False, f"HTTP {r.status_code}: {r.text[:300]}"

    def clear_queue(self):
        # Replace the queue with an empty list.
        url = f"{self.base}/as/applications/{self.app}/devices/{self.dev}/down/replace"
        try:
            r = requests.post(url, headers=self.h, data=json.dumps({"downlinks": []}), timeout=15)
        except requests.RequestException as e:
            return False, str(e)
        return (r.status_code == 200), (f"HTTP {r.status_code}" if r.status_code != 200 else "cleared")


# ===========================================================================
# Streamlit UI
# ===========================================================================
import streamlit as st  # noqa: E402
import pandas as pd     # noqa: E402


def secret(name, default=""):
    try:
        if name in st.secrets:
            return str(st.secrets[name])
    except Exception:
        pass
    return os.environ.get(name, default)


def parse_uplink_row(res):
    """One Storage 'result' -> flat row with metadata + decoded fields."""
    um = res.get("uplink_message", {}) or {}
    ts = res.get("received_at") or um.get("received_at")
    dec = um.get("decoded_payload")
    if not dec and um.get("f_port") in (None, UPLINK_FPORT):
        frm = um.get("frm_payload")
        if frm:
            try:
                dec = decode_uplink(base64.b64decode(frm))
            except Exception:
                dec = None
    row = {"time": ts, "fport": um.get("f_port"), "fcnt": um.get("f_cnt")}
    rxs = um.get("rx_metadata") or [{}]
    row["rssi"] = rxs[0].get("rssi")
    row["snr"] = rxs[0].get("snr")
    row["gateway"] = (rxs[0].get("gateway_ids") or {}).get("gateway_id")
    dr = ((um.get("settings") or {}).get("data_rate") or {}).get("lora") or {}
    row["sf"] = dr.get("spreading_factor")
    row.update(flatten_decoded(dec))
    return row


def fmt_time(iso):
    if not iso:
        return "-"
    try:
        return datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone().strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        return iso


st.set_page_config(page_title="EnviroNode-WL55", layout="wide")

# Minimal, modern chrome: hide Streamlit's menu/footer/header, constrain width,
# quiet the type scale. No emoji anywhere.
st.markdown(
    """
    <style>
      #MainMenu, footer, header {visibility: hidden;}
      .block-container {max-width: 1080px; padding-top: 2.2rem; padding-bottom: 3rem;}
      h1, h2, h3 {font-weight: 600; letter-spacing: -0.01em;}
      [data-testid="stMetricValue"] {font-weight: 600;}
      .stTabs [data-baseweb="tab"] {font-weight: 500;}
      hr {margin: 0.9rem 0;}
    </style>
    """,
    unsafe_allow_html=True,
)


def sget(x, k, r=None):
    """Safe get from a pandas Series -> None when missing/NaN."""
    v = x.get(k)
    return None if v is None or (isinstance(v, float) and pd.isna(v)) else (round(v, r) if r is not None and isinstance(v, (int, float)) else v)


# ---- sidebar: connection ----
st.sidebar.subheader("EnviroNode-WL55")
st.sidebar.caption("Remote control via The Things Network")
region = st.sidebar.text_input("Region", secret("TTN_REGION", DEFAULT_REGION))
app_id = st.sidebar.text_input("Application", secret("TTN_APP_ID", "geoenvironode"))
dev_id = st.sidebar.text_input("Device", secret("TTN_DEVICE_ID", "envnode-01"))
gw_id = st.sidebar.text_input("Gateway (optional)", secret("TTN_GATEWAY_ID", ""))
api_key = st.sidebar.text_input("API key", secret("TTN_API_KEY", ""), type="password",
                                help="Full TTN key: NNSXS.<id>.<secret> — not just the key ID.")
history_n = st.sidebar.slider("History depth", 10, 200, 50, 10)
st.sidebar.caption("Class A: commands apply after the node's next uplink "
                   "(up to one interval).")

if not api_key:
    st.title("EnviroNode-WL55")
    st.info("Enter your full TTN **API key** (`NNSXS.…`) in the sidebar to connect. "
            "Nothing is stored.")
    st.stop()

ttn = TTN(region, app_id, dev_id, api_key, gw_id)

# fetch once per run
ok_up, uplinks = ttn.storage_uplinks(history_n)
rows = [parse_uplink_row(r) for r in uplinks] if ok_up else []
df = pd.DataFrame(rows)
if not df.empty:
    df["time"] = pd.to_datetime(df["time"], errors="coerce", utc=True)
    df = df.dropna(subset=["time"]).sort_values("time")

tab_status, tab_data, tab_control, tab_advanced, tab_help = st.tabs(
    ["Status", "Data", "Control", "Advanced", "Help"])

# ===========================================================================
# Status
# ===========================================================================
with tab_status:
    top = st.columns([3, 1])
    top[0].title("EnviroNode-WL55")
    if top[1].button("Refresh", use_container_width=True):
        st.rerun()

    if not ok_up:
        st.error(uplinks)
    elif df.empty:
        st.warning("No stored uplinks yet.")
    else:
        last = df.iloc[-1]
        age = (pd.Timestamp.now(tz="UTC") - last["time"]).total_seconds()
        soc = soc_from_voltage(sget(last, "batt_V"))
        st.caption(
            f"Last uplink {fmt_time(str(last['time']))} · {int(age)}s ago · "
            f"FCnt {sget(last, 'fcnt')} · {sget(last, 'gateway') or '—'} · "
            f"RSSI {sget(last, 'rssi')} dBm · SNR {sget(last, 'snr')} dB · "
            f"SF{sget(last, 'sf')}"
        )
        if last.get("fault"):
            st.warning("Fault flag set — a selected sensor failed this frame "
                       "(a blank field below means not measured, not zero).")

        # headline metrics
        m = st.columns(4)
        m[0].metric("Battery", f"{soc:.0f} %" if soc is not None else "—",
                    f"{sget(last, 'batt_V', 2)} V" if sget(last, "batt_V") is not None else None)
        cur = sget(last, "batt_mA")
        m[1].metric("Current", f"{int(cur)} mA" if cur is not None else "—",
                    "charging" if (cur is not None and cur < 0) else ("discharging" if cur is not None else None),
                    delta_color="off")
        m[2].metric("Air", f"{sget(last, 'air1_t', 1)} °C" if sget(last, "air1_t") is not None else "—",
                    f"{sget(last, 'air1_rh', 0)} %RH" if sget(last, "air1_rh") is not None else None,
                    delta_color="off")
        m[3].metric("Soil temp", f"{sget(last, 'soil_temp', 1)} °C" if sget(last, "soil_temp") is not None else "—")

        # battery charge bar
        if soc is not None:
            st.progress(int(soc), text=f"State of charge {soc:.0f}% "
                        f"(1S Li-ion, 3.30 V = 0%, 4.20 V = 100%)")

        with st.expander("All channels"):
            def line(label, val, unit=""):
                st.write(f"**{label}**  {('—' if val is None else str(val) + unit)}")
            c = st.columns(2)
            with c[0]:
                line("Air #1", f"{sget(last,'air1_t',2)} °C / {sget(last,'air1_rh',0)} %RH / {sget(last,'air1_p',1)} hPa")
                line("Air #2", f"{sget(last,'air2_t',2)} °C / {sget(last,'air2_rh',0)} %RH / {sget(last,'air2_p',1)} hPa")
                line("Soil moisture", sget(last, "soil_moisture"), " cts")
                line("Leaf wetness", sget(last, "leaf_wetness"), " cts")
                line("Soil temperature", sget(last, "soil_temp", 2), " °C")
            with c[1]:
                line("Wind speed", sget(last, "wind_speed", 2), " m/s")
                line("Wind gust", sget(last, "wind_gust", 2), " m/s")
                line("Wind direction", sget(last, "wind_dir", 1), "°")
                line("Rain", f"{sget(last,'rain_tips')} tips / {sget(last,'rain_mm',2)} mm")
                st.write(f"**Status byte**  0x{int(last['status']):02X}"
                         if pd.notna(last.get("status")) else "**Status byte**  —")

        with st.expander("Network"):
            ok_j, ji = ttn.join_info()
            if ok_j:
                st.write(f"DevAddr {ji.get('ids', {}).get('dev_addr', '—')} · "
                         f"last DevNonce {ji.get('last_dev_nonce', '—')} · "
                         f"resets nonces {ji.get('resets_join_nonces', False)}")
            if gw_id:
                ok_g, gs = ttn.gateway_stats()
                if ok_g:
                    st.write(f"Gateway {gw_id} · last uplink {fmt_time(gs.get('last_uplink_received_at'))} · "
                             f"{gs.get('uplink_count')} up / {gs.get('downlink_count')} down")

# ===========================================================================
# Data
# ===========================================================================
with tab_data:
    if df.empty:
        st.warning("No data to chart yet.")
    else:
        idx = df.set_index("time")
        charts = [
            ("Temperature (°C)", ["air1_t", "air2_t", "soil_temp"]),
            ("Battery state of charge (%)", None),  # computed below
            ("Humidity (%RH)", ["air1_rh", "air2_rh"]),
            ("Battery (V)", ["batt_V"]),
            ("Soil / leaf (counts)", ["soil_moisture", "leaf_wetness"]),
            ("Battery current (mA)", ["batt_mA"]),
            ("Wind (m/s · °)", ["wind_speed", "wind_gust", "wind_dir"]),
            ("Link (RSSI dBm · SNR dB)", ["rssi", "snr"]),
        ]
        cols = st.columns(2)
        for i, (title, series) in enumerate(charts):
            with cols[i % 2]:
                st.caption(title)
                if series is None:
                    socs = idx["batt_V"].map(soc_from_voltage) if "batt_V" in idx else None
                    if socs is not None and socs.notna().any():
                        st.line_chart(socs.rename("soc_%"))
                else:
                    have = [s for s in series if s in idx.columns and idx[s].notna().any()]
                    if have:
                        st.line_chart(idx[have])
        st.divider()
        st.dataframe(df.iloc[::-1], use_container_width=True, height=280)
        st.download_button("Download CSV", df.to_csv(index=False).encode(),
                           file_name=f"{dev_id}_uplinks.csv", mime="text/csv")

# ===========================================================================
# Control
# ===========================================================================
def _send(payload, fport, label):
    ok, msg = ttn.push_downlink(payload, fport)
    (st.success if ok else st.error)(f"{label}: {msg}")


with tab_control:
    st.caption("Each control queues a downlink; the node applies it after its "
               "next uplink and persists it in flash.")
    left, right = st.columns(2)

    with left:
        st.subheader("Sensors & interval")
        grid = st.columns(2)
        chosen = []
        for i, (k, _bit, label) in enumerate(SENSOR_KEYS):
            if grid[i % 2].checkbox(f"{k} · {label}", value=(k != "R"), key=f"sel_{k}"):
                chosen.append(k)
        interval = st.number_input("Interval (min)", INTERVAL_MIN, INTERVAL_MAX, 15, 1)
        cfg = build_config_string(chosen if chosen else ["NONE"], interval)
        if not chosen:
            st.caption("No sensors selected — sends {NONE,…}, pausing measurements "
                       "(battery still reported).")
        st.code(cfg, language="text")
        if st.button("Send sensor set", type="primary", use_container_width=True):
            _send(*enc_config_string(cfg), cfg)

    with right:
        st.subheader("Actions")
        if st.button("Uplink now", use_container_width=True):
            _send(*enc_uplink_now(), "uplink now")
        if st.button("Reset rain counter", use_container_width=True):
            _send(*enc_reset_rain(), "reset rain")
        with st.expander("Calibration & alignment"):
            sid = st.selectbox("Sensor", CAL_SENSORS, format_func=lambda x: x[1])
            off = st.number_input("Offset (added, eng. units)", -300.0, 300.0, 0.0, 0.01)
            if st.button("Send calibration", use_container_width=True):
                _send(*enc_set_cal(sid[0], round(off * 100)), f"cal {sid[0]} {off:+}")
            deg = st.number_input("Wind-vane north offset (°)", 0.0, 359.9, 0.0, 0.1)
            if st.button("Send vane offset", use_container_width=True):
                _send(*enc_set_winddir_offset(round(deg * 10)), f"vane {deg}°")
        with st.expander("Reboot"):
            st.caption("Restarts the application core.")
            if st.button("Confirm reboot", use_container_width=True):
                _send(*enc_reboot(), "reboot")

# ===========================================================================
# Advanced
# ===========================================================================
with tab_advanced:
    st.subheader("Raw config string")
    raw = st.text_input("Config string", value="{?}",
                        help="Any {…}. e.g. {ALL,15} {NONE} {+R} {-LW,-WD} {5}. "
                             "{?} asks the node to print its set (console only).")
    if st.button("Send config string"):
        try:
            payload, fport = enc_config_string(raw)
            _send(payload, fport, f"{raw} -> {hexstr(payload)}")
        except ValueError as e:
            st.error(str(e))

    st.divider()
    st.subheader("Raw binary downlink")
    rc = st.columns([1, 3])
    fport = rc[0].number_input("FPort", 1, 223, COMMAND_FPORT)
    hexin = rc[1].text_input("Payload (hex)", "02", help="e.g. 010F00 = set interval 15")
    if st.button("Send raw bytes"):
        try:
            _send(bytes.fromhex(hexin.replace(" ", "")), int(fport), f"FPort {int(fport)} {hexin}")
        except ValueError:
            st.error("payload must be valid hex")

    st.divider()
    st.subheader("Enable mask (FPort 10, 0x06)")
    mg = st.columns(4)
    mkeys = [k for i, (k, _b, _l) in enumerate(SENSOR_KEYS) if mg[i % 4].checkbox(k, value=(k != "R"), key=f"mask_{k}")]
    mask = mask_from_keys(mkeys)
    st.caption(f"mask 0x{mask:02X} · {hexstr(enc_set_enable(mask)[0])}")
    if st.button("Send mask"):
        _send(*enc_set_enable(mask), f"set_enable 0x{mask:02X}")

    st.divider()
    st.subheader("Downlink queue")
    okq, q = ttn.queue()
    if okq and q:
        st.dataframe(pd.DataFrame([{
            "f_port": d.get("f_port"),
            "hex": hexstr(base64.b64decode(d.get("frm_payload", ""))) if d.get("frm_payload") else "",
        } for d in q]), use_container_width=True)
    else:
        st.caption("queue empty" if okq else f"queue: {q}")
    if st.button("Clear queue"):
        ok, msg = ttn.clear_queue()
        (st.success if ok else st.error)(msg)
        st.rerun()

# ===========================================================================
# Help
# ===========================================================================
with tab_help:
    st.markdown("""
Monitors and remotely controls the EnviroNode-WL55 through The Things Network.
Uplink data comes from the TTN Storage integration; commands are Class-A
downlinks the node persists in flash.

**Sensor keys** — LW leaf · T1/T2 air · SM soil moisture · ST soil temp ·
WS wind speed · WD wind dir · R rain (R keeps the node awake between cycles).

**Battery %** is estimated from pack voltage on the node's own 1S Li-ion curve
(3.30 V = 0 %, 4.20 V = 100 %); it reads pessimistic under load.

**Latency** — a command is delivered in the receive window after the node's
next uplink, so it lands within one interval; TTN sends one queued item per
uplink. The node's current selection cannot be read back over the air
(`get_config` is unimplemented) — the status byte shows which sensors reported
OK, not which are selected.

**Credentials** — sidebar, environment variables, or Streamlit secrets. The
API key must be the full `NNSXS.<id>.<secret>` string, not just the key ID.
See `SECRETS.txt`.
""")
