#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

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
// MPU6050 I2C address
// =========================
const uint8_t MPU_ADDR = 0x68;

// =========================
// MQTT / WiFi
// =========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// =========================
// Helpers
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
  Wire.write(0x3B); // ACCEL_XOUT_H
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)6);

  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
}

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

String detectFace(float ax_g, float ay_g, float az_g) {
  const float TH = 0.75; // threshold in g

  if (az_g > TH)  return "work";
  if (az_g < -TH) return "break";
  if (ax_g > TH)  return "pause";
  if (ax_g < -TH) return "resume";
  if (ay_g > TH)  return "long_break";
  if (ay_g < -TH) return "idle";

  return "unknown";
}

bool publishFaceState(const String& face) {
  String payload;

  if (face == "work") {
    payload = "{\"action\":\"start\",\"mode\":\"work\",\"duration\":25}";
  } else if (face == "break") {
    payload = "{\"action\":\"start\",\"mode\":\"break\",\"duration\":5}";
  } else if (face == "long_break") {
    payload = "{\"action\":\"start\",\"mode\":\"long_break\",\"duration\":15}";
  } else if (face == "pause") {
    payload = "{\"action\":\"pause\"}";
  } else if (face == "resume") {
    payload = "{\"action\":\"resume\"}";
  } else if (face == "idle") {
    payload = "{\"action\":\"idle\"}";
  } else {
    return false;
  }

  Serial.print("Publishing: ");
  Serial.println(payload);

  return mqttClient.publish(MQTT_TOPIC, payload.c_str(), true);
}

// =========================
// MPU6050 setup
// =========================
void setupMPU6050() {
  // Wake up MPU6050
  writeMPU(0x6B, 0x00); // PWR_MGMT_1 = 0

  delay(100);

  // Accelerometer range ±2g
  writeMPU(0x1C, 0x00);

  // Gyro range ±250 deg/s
  writeMPU(0x1B, 0x00);

  // DLPF config
  writeMPU(0x1A, 0x03);

  // Motion threshold
  writeMPU(0x1F, 17);   // MOT_THR

  // Motion duration
  writeMPU(0x20, 1);    // MOT_DUR

  // Interrupt pin config:
  // bit5 LATCH_INT_EN = 1
  // bit4 INT_RD_CLEAR = 1
  writeMPU(0x37, 0x30);

  // Enable motion interrupt
  writeMPU(0x38, 0x40);

  // Optional: clear existing interrupt status
  readMPU(0x3A);
}

void clearMPUInterrupt() {
  readMPU(0x3A); // INT_STATUS
}

// =========================
// Deep sleep
// =========================
void goToDeepSleep() {
  Serial.println("Preparing for deep sleep...");

  clearMPUInterrupt();

  pinMode(MPU_INT_PIN, INPUT_PULLUP);

  // Wake up when INT pin goes HIGH
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN, 1);

  Serial.println("Entering deep sleep...");
  delay(200);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);

  pinMode(MPU_INT_PIN, INPUT_PULLUP);

  setupMPU6050();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Wakeup caused by MPU6050 interrupt");
  } else {
    Serial.println("Normal boot / power on");
  }

  // Wait a bit for cube to stabilize after movement
  delay(600);

  int16_t ax_raw, ay_raw, az_raw;
  readAccelRaw(ax_raw, ay_raw, az_raw);

  // Convert raw values to g for ±2g range
  float ax_g = ax_raw / 16384.0;
  float ay_g = ay_raw / 16384.0;
  float az_g = az_raw / 16384.0;

  String face = detectFace(ax_g, ay_g, az_g);

  Serial.print("Detected face: ");
  Serial.print(face);
  Serial.print(" | ax=");
  Serial.print(ax_g, 3);
  Serial.print(" ay=");
  Serial.print(ay_g, 3);
  Serial.print(" az=");
  Serial.println(az_g, 3);

  if (face != "unknown") {
    connectWiFi();
    connectMQTT();
    mqttClient.loop();

    publishFaceState(face);

    delay(300);
    mqttClient.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  goToDeepSleep();
}

void loop() {
  // Not used because device sleeps after setup()
}