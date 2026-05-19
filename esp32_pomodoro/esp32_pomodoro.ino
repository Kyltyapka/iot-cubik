#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =========================
// Stored network configuration
// =========================
const char* CONFIG_NAMESPACE = "netcfg";
const char* DEFAULT_MQTT_TOPIC = "pomodoro/cube/state/321";
const char* DEFAULT_MQTT_CLIENT_ID = "esp32_pomodoro_cube";
const uint16_t DEFAULT_MQTT_PORT = 1883;

const char* BLE_DEVICE_NAME = "ESP32-Pomodoro-Setup";
const char* BLE_CONFIG_SERVICE_UUID = "8cb3b7a0-6f8b-4e6e-8a19-8f823c76f101";
const char* BLE_CONFIG_RX_UUID = "8cb3b7a1-6f8b-4e6e-8a19-8f823c76f101";
const char* BLE_CONFIG_TX_UUID = "8cb3b7a2-6f8b-4e6e-8a19-8f823c76f101";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long MQTT_CONNECT_TIMEOUT_MS = 15000;
const uint8_t MAX_CONNECT_FAILURES_BEFORE_BLE = 3;

struct DeviceConfig {
  String wifiSsid;
  String wifiPassword;
  String mqttBroker;
  uint16_t mqttPort;
  String mqttTopic;
  String mqttClientId;
  String mqttUsername;
  String mqttPassword;
  bool useStaticIp;
  String deviceIp;
  String gateway;
  String subnet;
  String dns;
  bool valid;
};

// =========================
// Pins
// =========================
const int MPU_INT_PIN = 27;   
const int SDA_PIN = 21;
const int SCL_PIN = 22;

const int BUZZER_PIN = 25; //buzzer
const bool BUZZER_ACTIVE_LOW = false;

// =========================
// MPU6050
// =========================
const uint8_t MPU_ADDR = 0x68;
const unsigned long STABILIZATION_DELAY_MS = 350;
const unsigned long ACTIVE_WINDOW_MS = 7000;
const unsigned long SENSOR_POLL_MS = 120;

// =========================
// Globals
// =========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;
DeviceConfig deviceConfig;
BLECharacteristic* bleTxCharacteristic = nullptr;
volatile bool bleConfigSaved = false;

// =========================
// Helpers: MPU6050
// =========================
void writeMPU(uint8_t reg, uint8_t data) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(data);
  Wire.endTransmission();
}

uint8_t readMPU(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }
  return 0;
}

void readAccelRaw(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)6);

  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
}

void setupMPU6050() {
  // Wake up MPU6050
  writeMPU(0x6B, 0x00);
  delay(100);

  // Accelerometer range ±2g
  writeMPU(0x1C, 0x00);

  // Gyro range ±250 deg/s
  writeMPU(0x1B, 0x00);

  // DLPF
  writeMPU(0x1A, 0x03);

  // Motion threshold
  writeMPU(0x1F, 3);

  // Motion duration
  writeMPU(0x20, 1);

  // Interrupt config: latch interrupt
  writeMPU(0x37, 0x20);

  // Enable motion interrupt
  writeMPU(0x38, 0x40);

  // Clear interrupt status
  readMPU(0x3A);
}


// ================= Buzzer =================

void buzzerOff() {
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LOW ? HIGH : LOW);
}

void buzzerOn() {
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LOW ? LOW : HIGH);
}

void setupBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff();
}

void beep(int durationMs) {
  buzzerOn();
  delay(durationMs);
  buzzerOff();
  delay(60);
}

void beepWork() {
  beep(180);
}

void beepLongWork() {
  beep(150);
  beep(180);
}

void beepBreak() {
  beep(120);
  beep(120);
}

void beepPause() {
  beep(350);
}

void beepResume() {
  beep(100);
  beep(100);
}

void beepOff() {
  beep(150);
}

void beepByMode(String mode) {
  if (mode == "work") {
    beepWork();
  } else if (mode == "long_work") {
    beepLongWork();
  } else if (mode == "break") {
    beepBreak();
  } else if (mode == "pause") {
    beepPause();
  } else if (mode == "resume") {
    beepResume();
  } else if (mode == "off") {
    beepOff();
  }
}

// =========================
// Helpers: Device config
// =========================
bool isConfigValid(const DeviceConfig& config) {
  return config.wifiSsid.length() > 0
    && config.mqttBroker.length() > 0
    && config.mqttTopic.length() > 0
    && config.mqttClientId.length() > 0
    && config.mqttPort > 0;
}

DeviceConfig loadDeviceConfig() {
  DeviceConfig config;

  prefs.begin(CONFIG_NAMESPACE, true);
  config.wifiSsid = prefs.getString("ssid", "");
  config.wifiPassword = prefs.getString("wifi_pass", "");
  config.mqttBroker = prefs.getString("mqtt_host", "");
  config.mqttPort = prefs.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
  config.mqttTopic = prefs.getString("mqtt_topic", DEFAULT_MQTT_TOPIC);
  config.mqttClientId = prefs.getString("client_id", DEFAULT_MQTT_CLIENT_ID);
  config.mqttUsername = prefs.getString("mqtt_user", "");
  config.mqttPassword = prefs.getString("mqtt_pass", "");
  config.useStaticIp = prefs.getBool("static_ip", false);
  config.deviceIp = prefs.getString("ip", "");
  config.gateway = prefs.getString("gateway", "");
  config.subnet = prefs.getString("subnet", "");
  config.dns = prefs.getString("dns", "");
  prefs.end();

  config.valid = isConfigValid(config);
  return config;
}

void saveDeviceConfig(const DeviceConfig& config) {
  prefs.begin(CONFIG_NAMESPACE, false);
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("wifi_pass", config.wifiPassword);
  prefs.putString("mqtt_host", config.mqttBroker);
  prefs.putUShort("mqtt_port", config.mqttPort);
  prefs.putString("mqtt_topic", config.mqttTopic);
  prefs.putString("client_id", config.mqttClientId);
  prefs.putString("mqtt_user", config.mqttUsername);
  prefs.putString("mqtt_pass", config.mqttPassword);
  prefs.putBool("static_ip", config.useStaticIp);
  prefs.putString("ip", config.deviceIp);
  prefs.putString("gateway", config.gateway);
  prefs.putString("subnet", config.subnet);
  prefs.putString("dns", config.dns);
  prefs.end();
}

void clearDeviceConfig() {
  prefs.begin(CONFIG_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

uint8_t getConnectionFailureCount() {
  prefs.begin("pomodoro", true);
  uint8_t failures = prefs.getUChar("net_fail", 0);
  prefs.end();
  return failures;
}

void setConnectionFailureCount(uint8_t failures) {
  prefs.begin("pomodoro", false);
  prefs.putUChar("net_fail", failures);
  prefs.end();
}

void clearConnectionFailureCount() {
  setConnectionFailureCount(0);
}

bool readConfigValue(const String& payload, const String& key, String& value) {
  int start = 0;

  while (start < payload.length()) {
    int end = payload.indexOf('\n', start);
    if (end < 0) {
      end = payload.length();
    }

    String line = payload.substring(start, end);
    line.trim();

    if (line.length() > 0 && !line.startsWith("#")) {
      int separator = line.indexOf('=');
      if (separator > 0) {
        String lineKey = line.substring(0, separator);
        lineKey.trim();

        if (lineKey == key) {
          value = line.substring(separator + 1);
          value.trim();
          return true;
        }
      }
    }

    start = end + 1;
  }

  return false;
}

bool isTruthy(String value) {
  value.trim();
  value.toLowerCase();
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool updateConfigFromPayload(const String& payload, DeviceConfig& updatedConfig, String& error) {
  updatedConfig = deviceConfig;

  if (updatedConfig.mqttPort == 0) {
    updatedConfig.mqttPort = DEFAULT_MQTT_PORT;
  }
  if (updatedConfig.mqttTopic.length() == 0) {
    updatedConfig.mqttTopic = DEFAULT_MQTT_TOPIC;
  }
  if (updatedConfig.mqttClientId.length() == 0) {
    updatedConfig.mqttClientId = DEFAULT_MQTT_CLIENT_ID;
  }

  String value;

  if (readConfigValue(payload, "ssid", value)) {
    updatedConfig.wifiSsid = value;
  }
  if (readConfigValue(payload, "wifi_password", value)) {
    updatedConfig.wifiPassword = value;
  }
  if (readConfigValue(payload, "mqtt_broker", value)) {
    updatedConfig.mqttBroker = value;
  }
  if (readConfigValue(payload, "mqtt_port", value)) {
    long port = value.toInt();
    if (port < 1 || port > 65535) {
      error = "mqtt_port must be between 1 and 65535";
      return false;
    }
    updatedConfig.mqttPort = (uint16_t)port;
  }
  if (readConfigValue(payload, "mqtt_username", value)) {
    updatedConfig.mqttUsername = value;
  }
  if (readConfigValue(payload, "mqtt_password", value)) {
    updatedConfig.mqttPassword = value;
  }
  if (readConfigValue(payload, "mqtt_topic", value)) {
    updatedConfig.mqttTopic = value;
  }
  if (readConfigValue(payload, "mqtt_client_id", value)) {
    updatedConfig.mqttClientId = value;
  }
  if (readConfigValue(payload, "device_ip", value)) {
    updatedConfig.deviceIp = value;
  }
  if (readConfigValue(payload, "gateway", value)) {
    updatedConfig.gateway = value;
  }
  if (readConfigValue(payload, "subnet", value)) {
    updatedConfig.subnet = value;
  }
  if (readConfigValue(payload, "dns", value)) {
    updatedConfig.dns = value;
  }
  if (readConfigValue(payload, "clear_static_ip", value) && isTruthy(value)) {
    updatedConfig.useStaticIp = false;
    updatedConfig.deviceIp = "";
    updatedConfig.gateway = "";
    updatedConfig.subnet = "";
    updatedConfig.dns = "";
  } else {
    updatedConfig.useStaticIp = updatedConfig.deviceIp.length() > 0
      || updatedConfig.gateway.length() > 0
      || updatedConfig.subnet.length() > 0;
  }

  if (updatedConfig.wifiSsid.length() == 0) {
    error = "ssid is required";
    return false;
  }
  if (updatedConfig.mqttBroker.length() == 0) {
    error = "mqtt_broker is required";
    return false;
  }
  if (updatedConfig.mqttTopic.length() == 0) {
    error = "mqtt_topic is required";
    return false;
  }
  if (updatedConfig.mqttClientId.length() == 0) {
    error = "mqtt_client_id is required";
    return false;
  }
  if (updatedConfig.useStaticIp) {
    if (updatedConfig.deviceIp.length() == 0 || updatedConfig.gateway.length() == 0 || updatedConfig.subnet.length() == 0) {
      error = "device_ip, gateway and subnet are required for static IP";
      return false;
    }
  }

  updatedConfig.valid = isConfigValid(updatedConfig);
  if (!updatedConfig.valid) {
    error = "config is incomplete";
    return false;
  }

  return true;
}

void notifyBle(const String& message) {
  Serial.println(message);

  if (bleTxCharacteristic != nullptr) {
    bleTxCharacteristic->setValue(message.c_str());
    bleTxCharacteristic->notify();
  }
}

class ConfigWriteCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) {
    auto rawValue = characteristic->getValue();
    String payload = String(rawValue.c_str());
    payload.trim();

    if (payload.length() == 0) {
      notifyBle("ERROR empty payload");
      return;
    }

    String resetValue;
    if (payload == "reset" || (readConfigValue(payload, "reset", resetValue) && isTruthy(resetValue))) {
      clearDeviceConfig();
      deviceConfig = loadDeviceConfig();
      notifyBle("OK config cleared. Rebooting into setup mode.");
      bleConfigSaved = true;
      return;
    }

    DeviceConfig updatedConfig;
    String error;

    if (!updateConfigFromPayload(payload, updatedConfig, error)) {
      notifyBle("ERROR " + error);
      return;
    }

    saveDeviceConfig(updatedConfig);
    clearConnectionFailureCount();
    deviceConfig = loadDeviceConfig();
    notifyBle("OK config saved. Rebooting.");
    bleConfigSaved = true;
  }
};

void startBleConfigMode(const String& reason) {
  Serial.println("Starting BLE config mode");
  Serial.print("Reason: ");
  Serial.println(reason);
  mqttClient.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  beepPause();

  bleConfigSaved = false;
  BLEDevice::init(BLE_DEVICE_NAME);

  BLEServer* server = BLEDevice::createServer();
  BLEService* service = server->createService(BLE_CONFIG_SERVICE_UUID);

  bleTxCharacteristic = service->createCharacteristic(
    BLE_CONFIG_TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* rxCharacteristic = service->createCharacteristic(
    BLE_CONFIG_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxCharacteristic->setCallbacks(new ConfigWriteCallback());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_CONFIG_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  notifyBle("READY send key=value config over BLE");
  Serial.println("Example payload:");
  Serial.println("ssid=YourWiFi");
  Serial.println("wifi_password=YourWiFiPassword");
  Serial.println("mqtt_broker=192.168.1.10");
  Serial.println("mqtt_port=1883");
  Serial.println("mqtt_username=YourMqttUser");
  Serial.println("mqtt_password=YourMqttPassword");

  unsigned long lastReminder = millis();
  while (!bleConfigSaved) {
    delay(250);

    if (millis() - lastReminder > 10000) {
      lastReminder = millis();
      Serial.println("Waiting for BLE config...");
      beep(40);
    }
  }

  BLEDevice::stopAdvertising();
  delay(500);
  BLEDevice::deinit(true);
  bleTxCharacteristic = nullptr;

  beepResume();
  delay(800);
  ESP.restart();
}

bool applyStaticIpConfig() {
  if (!deviceConfig.useStaticIp) {
    return true;
  }

  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;

  if (!ip.fromString(deviceConfig.deviceIp.c_str())
      || !gateway.fromString(deviceConfig.gateway.c_str())
      || !subnet.fromString(deviceConfig.subnet.c_str())) {
    Serial.println("Invalid static IP config, falling back to DHCP");
    return false;
  }

  bool dnsConfigured = deviceConfig.dns.length() > 0 && dns.fromString(deviceConfig.dns.c_str());
  bool configured = dnsConfigured
    ? WiFi.config(ip, gateway, subnet, dns)
    : WiFi.config(ip, gateway, subnet);

  if (!configured) {
    Serial.println("Failed to apply static IP config");
    return false;
  }

  Serial.print("Using static IP: ");
  Serial.println(deviceConfig.deviceIp);
  return true;
}


// =========================
// Helpers: Wi-Fi / MQTT
// =========================
bool connectWiFi() {
  if (!deviceConfig.valid) {
    Serial.println("Network config is missing");
    return false;
  }

  WiFi.mode(WIFI_STA);
  applyStaticIpConfig();
  WiFi.begin(deviceConfig.wifiSsid.c_str(), deviceConfig.wifiPassword.c_str());

  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection failed");
    return false;
  }

  Serial.println("\nWi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool connectMQTT() {
  if (!deviceConfig.valid) {
    Serial.println("MQTT config is missing");
    return false;
  }

  mqttClient.setServer(deviceConfig.mqttBroker.c_str(), deviceConfig.mqttPort);

  unsigned long start = millis();
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    bool connected;

    if (deviceConfig.mqttUsername.length() > 0) {
      connected = mqttClient.connect(
        deviceConfig.mqttClientId.c_str(),
        deviceConfig.mqttUsername.c_str(),
        deviceConfig.mqttPassword.c_str()
      );
    } else {
      connected = mqttClient.connect(deviceConfig.mqttClientId.c_str());
    }

    if (connected) {
      Serial.println("connected");
      return true;
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 2 sec");
      delay(2000);
    }

    if (millis() - start >= MQTT_CONNECT_TIMEOUT_MS) {
      Serial.println("MQTT connection failed");
      return false;
    }
  }

  return true;
}

// =========================
// Helpers: Preferences
// =========================
String getLastMode() {
  prefs.begin("pomodoro", true);
  String lastMode = prefs.getString("last_mode", "");
  prefs.end();
  return lastMode;
}

void setLastMode(const String& mode) {
  prefs.begin("pomodoro", false);
  prefs.putString("last_mode", mode);
  prefs.end();
}

// =========================
// Cube face detection
// =========================
String detectFace(float ax, float ay, float az) {
  const float TH = 0.75;

  if (az > TH)  return "work";
  if (az < -TH) return "long_work";
  if (ax > TH)  return "resume";
  if (ax < -TH) return "break";
  if (ay > TH)  return "pause";
  if (ay < -TH) return "off";

  return "unknown";
}

// =========================
// MQTT payload
// =========================
bool publishFaceState(const String& face) {
  String payload;

  if (face == "work") {
    payload = "{\"mode\":\"work\"}";
  } else if (face == "long_work") {
    payload = "{\"mode\":\"long_work\"}";
  } else if (face == "resume") {
    payload = "{\"mode\":\"resume\"}";
  } else if (face == "break") {
    payload = "{\"mode\":\"break\"}";
  } else if (face == "pause") {
    payload = "{\"mode\":\"pause\"}";
  } else if (face == "off") {
    payload = "{\"mode\":\"off\"}";
  } else {
    return false;
  }

  Serial.print("Publishing: ");
  Serial.println(payload);

  return mqttClient.publish(deviceConfig.mqttTopic.c_str(), payload.c_str(), true);
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  return connectWiFi();
}

bool ensureMQTTConnected() {
  if (mqttClient.connected()) {
    return true;
  }

  return connectMQTT();
}

String readCurrentFace() {
  int16_t ax_raw, ay_raw, az_raw;
  readAccelRaw(ax_raw, ay_raw, az_raw);

  float ax_g = ax_raw / 16384.0;
  float ay_g = ay_raw / 16384.0;
  float az_g = az_raw / 16384.0;

  String face = detectFace(ax_g, ay_g, az_g);

  Serial.print("Read face: ");
  Serial.print(face);
  Serial.print(" | ax=");
  Serial.print(ax_g, 3);
  Serial.print(" ay=");
  Serial.print(ay_g, 3);
  Serial.print(" az=");
  Serial.println(az_g, 3);

  return face;
}

void handleActiveWindow() {
  unsigned long windowStart = millis();
  String lastMode = getLastMode();

  Serial.println("Active window started");

  while (millis() - windowStart < ACTIVE_WINDOW_MS) {
    mqttClient.loop();

    String face = readCurrentFace();

    if (face != "unknown" && face != lastMode) {
      Serial.print("New face detected in active window: ");
      Serial.println(face);

      // Коротка стабілізація після реального перевертання
      delay(250);
      face = readCurrentFace();

      if (face != "unknown" && face != lastMode) {
        if (!ensureWiFiConnected()) {
          Serial.println("Wi-Fi reconnect failed inside active window");
        } else if (!ensureMQTTConnected()) {
          Serial.println("MQTT reconnect failed inside active window");
        } else {
          if (publishFaceState(face)) {
            setLastMode(face);
            lastMode = face;

            // після нової дії починаємо активне вікно спочатку
            windowStart = millis();
            Serial.println("Mode published from active window");
          }
        }
      }
    }

    delay(SENSOR_POLL_MS);
  }

  Serial.println("Active window expired");
}


// =========================
// Deep sleep
// =========================
void goToDeepSleep() {
  // Clear MPU interrupt
  readMPU(0x3A);

  // Wait until INT pin becomes LOW
  int timeout = 0;
  while (digitalRead(MPU_INT_PIN) == HIGH && timeout < 50) {
    delay(10);
    timeout++;
  }
 
 buzzerOff();

  Serial.print("INT pin before sleep: ");
  Serial.println(digitalRead(MPU_INT_PIN));

  esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN, 1);

  Serial.println("Entering deep sleep...");
  delay(200);
  esp_deep_sleep_start();
}

void handleInitialConnectionFailure(const String& reason) {
  uint8_t failures = getConnectionFailureCount();
  if (failures < 255) {
    failures++;
  }

  setConnectionFailureCount(failures);

  Serial.print("Connection failure count: ");
  Serial.print(failures);
  Serial.print("/");
  Serial.println(MAX_CONNECT_FAILURES_BEFORE_BLE);

  if (failures >= MAX_CONNECT_FAILURES_BEFORE_BLE) {
    startBleConfigMode(reason + " after repeated failures");
  }

  Serial.println("Keeping saved config. Going to sleep; reset or move the cube to retry.");
  goToDeepSleep();
}

// =========================
// Setup
// =========================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32 Pomodoro Cube ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  pinMode(MPU_INT_PIN, INPUT);

  setupBuzzer();
  setupMPU6050();
  deviceConfig = loadDeviceConfig();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Wakeup caused by MPU6050 interrupt");
  } else {
    Serial.println("Normal boot / power on");
  }

  if (!deviceConfig.valid) {
    startBleConfigMode("No saved Wi-Fi/MQTT config");
  }

  // Стабілізація після пробудження
  delay(STABILIZATION_DELAY_MS);

  String face = readCurrentFace();

  if (face != "unknown") {
    String lastMode = getLastMode();

    Serial.print("Last mode: ");
    Serial.println(lastMode);

    bool shouldPublish = (face != lastMode);

    if (shouldPublish) {
      if (ensureWiFiConnected() && ensureMQTTConnected()) {
        clearConnectionFailureCount();
        mqttClient.loop();

        if (publishFaceState(face)) {
          setLastMode(face);

          // Не засинаємо одразу: тримаємо коротке активне вікно
          handleActiveWindow();
        }

        delay(150);
        mqttClient.disconnect();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      } else {
        Serial.println("Initial Wi-Fi/MQTT connection failed");
        handleInitialConnectionFailure("Initial Wi-Fi/MQTT connection failed");
      }
    } else {
      Serial.println("Same face as last mode, skipping publish");
      // навіть якщо режим той самий, даємо коротке вікно на можливе повторне перевертання
      if (ensureWiFiConnected() && ensureMQTTConnected()) {
        clearConnectionFailureCount();
        handleActiveWindow();

        delay(150);
        mqttClient.disconnect();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      } else {
        Serial.println("Initial Wi-Fi/MQTT connection failed");
        handleInitialConnectionFailure("Initial Wi-Fi/MQTT connection failed");
      }
    }
  }

  goToDeepSleep();
}

// =========================
// Loop
// =========================
void loop() {
  // not used
}
