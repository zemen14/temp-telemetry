# 🌡️ Temp Telemetry — ESP32-C3 Sensor Reporting to Python

**The other half of talking to the cloud: sending data up.** An
ESP32-C3 reads a real SHTC3 temperature/humidity sensor and POSTs
the temperature to a small Python server every 10 seconds. The
server timestamps and logs every report it receives.

```
[2026-10-13 16:10:05] ESP32 sensor temperature: 24.61°C
```

## How it works

- **Firmware** (`main/main.c`) — joins WiFi (event-driven,
  auto-reconnect), reads the SHTC3 over I2C with CRC-8 validation,
  and POSTs `sensor_temp` form-encoded to the server. A failed or
  corrupt read is skipped — nothing gets posted that wasn't
  actually measured.
- **Server** (`server/server.py`) — a dependency-free Python
  `http.server` app listening on port 1234; `/temperature` accepts
  the reports and logs them with timestamps.

## Setup

Firmware — in `main/main.c` set:

- `WIFI_SSID` / `WIFI_PASSWORD` — your network credentials
  (placeholders in this repo; never commit real ones)
- `POST_URL` — your server's address, e.g.
  `http://<server-ip>:1234/temperature`

Server:

```bash
python3 server/server.py
```

Firmware (ESP-IDF 6.1):

```bash
idf.py set-target esp32c3
idf.py build flash monitor
```

## Hardware

- ESP32-C3 board
- SHTC3 temperature/humidity sensor (I2C `0x70`, SDA GPIO 7,
  SCL GPIO 8)
