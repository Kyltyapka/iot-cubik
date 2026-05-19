# How to run the Pomodoro Cube project

This project has four parts:

- ESP32 firmware in `esp32_pomodoro/esp32_pomodoro.ino`
- local MQTT broker with Mosquitto
- Node-RED flow in `node-red/flow.json`
- Flask dashboard/backend in `backend/app.py`

## 1. Install tools

Install these on Windows:

- Python 3.11+ with `py` or `python` available in PowerShell
- Mosquitto
- Node.js LTS
- Node-RED: `npm install -g --unsafe-perm node-red`
- Arduino IDE with the ESP32 board package

Arduino libraries:

- PubSubClient
- Adafruit MPU6050
- Adafruit Unified Sensor

`WiFi`, `Preferences`, and BLE headers come from the ESP32 Arduino core.

## 1.1 Configure local secrets

Create the local backend environment file:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
copy .\backend\.env.example .\backend\.env
```

Edit `backend\.env` and fill `API_KEY`, Telegram values if needed, and MQTT values.
Do not commit the real `.env` file.

Create the Mosquitto password file:

```powershell
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" -c C:\Users\LEGION\Desktop\bakalavr\code\passwd pomodoro_new
```

## 2. Start the backend

Open PowerShell:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code\backend
py -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python app.py
```

Dashboard:

```text
http://127.0.0.1:3000
```

Leave this terminal open.

## 2.1 Quick start after setup

After dependencies are installed and `backend\.env` plus `passwd` are configured, you can start the three local services in separate windows:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
.\start_project.ps1
```

## 3. Start Mosquitto

Open a second PowerShell:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
mosquitto -c .\mosquitto.conf -v
```

If `mosquitto` is not in `PATH`, use:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
& "C:\Program Files\mosquitto\mosquitto.exe" -c .\mosquitto.conf -v
```

The config uses `code/passwd`, so MQTT login is required.

## 4. Start Node-RED

Open a third PowerShell:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
$env:API_KEY = (Get-Content .\backend\.env | Where-Object { $_ -match '^API_KEY=' }).Split('=', 2)[1].Trim()
node-red -u .\node-red .\node-red\flow.json
```

Open:

```text
http://127.0.0.1:1880
```

In Node-RED, open the `Local Mosquitto` broker node and set its MQTT username/password to the same credentials used by Mosquitto. Then press Deploy.

## 5. Flash ESP32

Open `esp32_pomodoro/esp32_pomodoro.ino` in Arduino IDE.

Select:

- Board: ESP32 Dev Module or your exact ESP32 board
- Port: your ESP32 COM port
- Serial Monitor speed: 115200

Upload the firmware.

On the first boot, ESP32 starts BLE setup mode with this name:

```text
ESP32-Pomodoro-Setup
```

## 6. Send Wi-Fi/MQTT config over BLE

Install the BLE helper dependency:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
py -m pip install -r .\tools\requirements.txt
```

Find your PC LAN IP address:

```powershell
ipconfig
```

Use the IPv4 address of your Wi-Fi/Ethernet adapter, for example `192.168.1.10`.
Do not use `127.0.0.1` for ESP32, because ESP32 is a separate device on the network.

Provision ESP32:

```powershell
py .\tools\provision_esp32_ble.py `
  --ssid "YourWiFi" `
  --wifi-password "YourWiFiPassword" `
  --mqtt-broker "192.168.1.10" `
  --mqtt-username "pomodoro_new" `
  --mqtt-password "YourMqttPassword"
```

After this ESP32 saves the settings and reboots. Next launches will use the saved config automatically.

If Wi-Fi or MQTT is temporarily unavailable, ESP32 keeps the saved config and retries on the next wake/reset. BLE setup starts only after repeated connection failures or when no saved config exists.

On Windows you can let the helper detect the current Wi-Fi SSID and this PC's IPv4 address automatically:

```powershell
py .\tools\provision_esp32_ble.py `
  --auto-windows `
  --wifi-password "YourWiFiPassword" `
  --mqtt-username "pomodoro_new" `
  --mqtt-password "YourMqttPassword"
```

Use this again when you move the computer and ESP32 to another Wi-Fi network. ESP32 still needs the Wi-Fi password for the new network, so pass the new password if it changed.

## 7. Test without ESP32

You can test the backend directly:

```powershell
cd C:\Users\LEGION\Desktop\bakalavr\code
$apiKey = (Get-Content .\backend\.env | Where-Object { $_ -match '^API_KEY=' }).Split('=', 2)[1].Trim()
Invoke-RestMethod -Method Post `
  -Uri http://127.0.0.1:3000/api/mode `
  -Headers @{ "X-API-Key" = $apiKey } `
  -ContentType "application/json" `
  -Body '{"mode":"work"}'
```

You can test the whole MQTT -> Node-RED -> backend chain:

```powershell
mosquitto_pub -h 127.0.0.1 -p 1883 `
  -u pomodoro_new `
  -P "YourMqttPassword" `
  -t pomodoro/cube/state/321 `
  -m '{"mode":"work"}'
```

The dashboard should switch to Work mode.

## 8. Reset ESP32 config

When ESP32 is in BLE setup mode:

```powershell
py .\tools\provision_esp32_ble.py --reset
```

You can also force setup mode by sending wrong/empty Wi-Fi or by changing MQTT settings so connection fails.
