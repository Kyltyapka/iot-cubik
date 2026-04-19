#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>

// =========================
// Wi-Fi
// =========================
const char* WIFI_SSID = "TP-Link_274";
const char* WIFI_PASSWORD = "0989239311";

// =========================
// MQTT
// =========================
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "pomodoro/cube/state/321";
const char* MQTT_CLIENT_ID = "esp32_pomodoro_cube_456";

// =========================
// Pins
// =========================
const int MPU_INT_PIN = 27;   // INT pin from MPU6050
const int SDA_PIN = 21;
const int SCL_PIN = 22;

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

// =========================
// Helpers: Wi-Fi / MQTT
// =========================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 2 sec");
      delay(2000);
    }
  }
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

  return mqttClient.publish(MQTT_TOPIC, payload.c_str(), true);
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  connectWiFi();
  return WiFi.status() == WL_CONNECTED;
}

bool ensureMQTTConnected() {
  if (mqttClient.connected()) {
    return true;
  }

  connectMQTT();
  return mqttClient.connected();
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

  Serial.print("INT pin before sleep: ");
  Serial.println(digitalRead(MPU_INT_PIN));

  esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN, 1);

  Serial.println("Entering deep sleep...");
  delay(200);
  esp_deep_sleep_start();
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

  setupMPU6050();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Wakeup caused by MPU6050 interrupt");
  } else {
    Serial.println("Normal boot / power on");
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
      }
    } else {
      Serial.println("Same face as last mode, skipping publish");
      // навіть якщо режим той самий, даємо коротке вікно на можливе повторне перевертання
      if (ensureWiFiConnected() && ensureMQTTConnected()) {
        handleActiveWindow();

        delay(150);
        mqttClient.disconnect();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
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