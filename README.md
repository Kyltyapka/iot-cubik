# Pomodoro Cube

IoT Pomodoro timer controlled by a physical ESP32 cube. The cube detects its face with an MPU6050 sensor, publishes the selected mode over MQTT, Node-RED forwards the command to a Flask backend, and the dashboard shows the current timer, recent sessions, and statistics.

## Architecture

```text
ESP32 + MPU6050
  -> MQTT topic pomodoro/cube/state/321
  -> Mosquitto
  -> Node-RED flow
  -> Flask REST API
  -> SQLite + web dashboard + Telegram notifications
```

## Main features

- ESP32 face detection for `work`, `long_work`, `break`, `pause`, `resume`, and `off`.
- BLE provisioning for Wi-Fi and MQTT settings without reflashing firmware.
- Safer ESP32 fallback: temporary Wi-Fi/MQTT failures keep the saved config and retry later before entering BLE setup.
- Local Mosquitto broker with username/password authentication.
- Node-RED bridge from MQTT to Flask.
- Flask backend with REST API, SQLite session history, auto break transitions, and Telegram notifications.
- Dashboard with live timer, recent sessions, mode statistics, and system status.

## Repository layout

```text
backend/                 Flask backend, dashboard, SQLite runtime database
esp32_pomodoro/          Arduino ESP32 firmware
node-red/                Node-RED flow
tools/                   BLE provisioning helper
mosquitto.conf           Local Mosquitto config
start_project.ps1        Windows helper to start backend, Mosquitto, and Node-RED
RUN.md                   Detailed run guide
```

## Configuration

Create local backend config:

```powershell
copy backend\.env.example backend\.env
```

Fill `backend\.env` with local values. Do not commit real `.env`, `passwd`, or `.db` files.

Create the Mosquitto password file:

```powershell
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" -c passwd pomodoro_new
```

## Run

For the full step-by-step setup, see `RUN.md`.

After dependencies are installed and local secrets are configured, you can start the local services with:

```powershell
.\start_project.ps1
```

Open:

```text
Dashboard: http://127.0.0.1:3000
Node-RED:  http://127.0.0.1:1880
```

## ESP32 BLE provisioning

Install the helper dependency:

```powershell
py -m pip install -r .\tools\requirements.txt
```

Provision with automatic Windows Wi-Fi SSID and PC IP detection:

```powershell
py .\tools\provision_esp32_ble.py `
  --auto-windows `
  --wifi-password "YourWiFiPassword" `
  --mqtt-username "pomodoro_new" `
  --mqtt-password "YourMqttPassword"
```

The ESP32 stores the configuration in NVS and reuses it after reboot.

## Security notes

- Keep `backend/.env`, `passwd`, and `pomodoro.db` local.
- Rotate Telegram/API/MQTT credentials if real values were ever pushed to a public repository.
- Prefer DHCP reservation or a static LAN IP for the computer that runs Mosquitto.
