---
title: EnviroNode WL55 Dashboard
emoji: 🌱
colorFrom: green
colorTo: blue
sdk: streamlit
sdk_version: 1.38.0
app_file: app.py
pinned: false
---

# EnviroNode-WL55 — remote dashboard

A Streamlit app to **monitor and remotely control** the EnviroNode-WL55 LoRaWAN
sensor node through The Things Network (TTN). Live data and history come from
the TTN Storage integration; every command the firmware accepts is sent as a
Class-A downlink.


## What it does
- **Status** — latest decoded reading (air ×2, soil moisture/temp, leaf, wind,
  rain, battery V + current), link quality, join info, gateway state.
- **Live data** — time-series charts and a downloadable table of recent uplinks.
- **Control** — pick which sensors run and how often, force an uplink, reset the
  rain counter, set the wind-vane north offset, apply calibration offsets, reboot.
- **Advanced / raw** — arbitrary config strings, raw byte downlinks, the
  `set_enable` mask, and the downlink queue.

All controls are **write-through**: the node persists changes in flash, and
(LoRaWAN Class A) a command is delivered in the receive window after the node's
**next uplink** — up to one interval of latency.

## Run it locally (Windows)
1. `install.bat` — creates a virtual environment, installs dependencies, runs the self-test.
2. Copy `.streamlit/secrets.toml.example` to `.streamlit/secrets.toml` and paste your TTN API key.
3. `run.bat` — opens http://localhost:8501.

Any OS: `pip install -r requirements.txt` then `streamlit run app.py`.

## Run it on Hugging Face
Create a Streamlit Space, upload `app.py` + `requirements.txt` + this `README.md`,
and set `TTN_API_KEY` (secret) plus `TTN_APP_ID` / `TTN_DEVICE_ID` / `TTN_REGION`
/ `TTN_GATEWAY_ID` (variables) in the Space settings. See `SECRETS.txt`.

## Verify the logic (no hardware, no network)
```
python app.py --selftest
```
Checks the uplink decoder and every downlink encoder against the firmware's
own byte vectors.

## Credentials
Never hard-coded. Provided via the sidebar, environment variables, or
Streamlit secrets — see **SECRETS.txt**. On-air formats: `../docs/PAYLOAD.md`
and `../docs/CONFIG.md`.
