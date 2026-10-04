/*
  Meridian Grid ESP32-S3 Energy Meter

  Hardware supplied by the IoT engineer:
  - ZMPT101B load-voltage circuit: GPIO 4
  - 5 A CT load-current circuit: GPIO 1
  - ADS1015 A2: PV voltage
  - ADS1015 A1: ACS712 30 A PV current
  - DS3231 RTC and 16x2 LCD: I2C SDA GPIO 8, SCL GPIO 9

  Required Arduino libraries:
  - WiFiManager by tzapu
  - AsyncMqttClient by Marvin Roger
  - RTClib by Adafruit
  - Adafruit ADS1X15 by Adafruit
  - ZMPT101B by Abdurraiq Bachmid
  - EmonLib by OpenEnergyMonitor
  - LiquidCrystal_I2C by Frank de Brabander

  The ESP32 creates one complete record every 60 seconds. It preserves the
  sequence number across resets, queues records while MQTT is offline, and
  publishes the agreed JSON payload to Meridian Grid's MQTT topic.
*/

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <ZMPT101B.h>
#include <EmonLib.h>
#include <AsyncTCP.h>
#if !defined(ASYNCTCP_VERSION_MAJOR) || ASYNCTCP_VERSION_MAJOR != 3 || ASYNCTCP_VERSION_MINOR != 3 || ASYNCTCP_VERSION_REVISION != 2
#error "Install the supplied ESP32Async AsyncTCP 3.3.2 ZIP; remove old/duplicate AsyncTCP libraries."
#endif
#include <AsyncMqttClient.h>
#include <atomic>
#include <esp_system.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <math.h>
#include <time.h>
#include "mbedtls/md.h"

// Signing is required for the live Meridian Grid device handoff. The matching
// public key is registered by the backend under the configured DEVICE_ID.
#define ENABLE_PAYLOAD_SIGNATURE 1
#if ENABLE_PAYLOAD_SIGNATURE
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/version.h"
#include "device-signing-secret.h"
#endif

// -------------------------- Identity and MQTT --------------------------
// Copy device-config.h.example to device-config.h and use the ID returned by
// Add device in the dashboard. This local header is deliberately gitignored.
#include "device-config.h"
String telemetryTopic = String("microgrid/v1/") + DEVICE_ID + "/telemetry";
String statusTopic = String("microgrid/v1/") + DEVICE_ID + "/status";

const uint32_t RECORD_INTERVAL_MS = 60000UL;
const uint32_t MQTT_RETRY_INTERVAL_MS = 10000UL;
const char *QUEUE_FILE = "/telemetry-queue.ndjson";
const uint32_t MQTT_ACK_TIMEOUT_MS = 30000UL;

// ------------------------------ Hardware --------------------------------
constexpr int LOAD_VOLTAGE_PIN = 4;  // GPIO 4 / ADC1_CH3
constexpr int LOAD_CURRENT_PIN = 1;  // GPIO 1 / ADC1_CH0
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;
constexpr uint8_t ADS1015_ADDRESS = 0x48;

// The calibration values below come from the supplied hardware design. Tune
// only with a trusted meter and document each final factor in the calibration log.
constexpr float ZMPT_SENSITIVITY = 850.0f;
constexpr float PV_VOLTAGE_DIVIDER_GAIN = 31.4f;  // 100 k / 3.3 k divider
constexpr float ACS712_ZERO_CURRENT_V = 2.5f;
constexpr float ACS712_SENSITIVITY_V_PER_A = 0.0660f; // ACS712 30 A
constexpr float CT_CALIBRATION = 20.05f;
constexpr float LOAD_POWER_FACTOR_ASSUMPTION = 1.0f;
constexpr uint8_t VOLTAGE_SAMPLE_COUNT = 6;
constexpr uint8_t PV_CURRENT_SAMPLE_COUNT = 3;

RTC_DS3231 rtc;
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_ADS1015 ads;
ZMPT101B voltageSensor(LOAD_VOLTAGE_PIN, 50.0);
EnergyMonitor emon1;
AsyncMqttClient mqttClient;

uint32_t sequenceNo = 0;
uint32_t lastRecordMillis = 0;
uint32_t lastMqttAttemptMillis = 0;
uint32_t lastEnergySampleMillis = 0;
uint32_t lastDisplaySwitch = 0;
uint8_t displayScreen = 0;
std::atomic<bool> mqttConnected{false};
std::atomic<uint32_t> mqttSession{0};
std::atomic<uint16_t> acknowledgedPacket{0};
std::atomic<int> mqttDisconnectReason{-1};
uint32_t observedMqttSession = 0;
uint32_t queueOffset = 0;
uint32_t pendingQueueEnd = 0;
uint32_t pendingPublishMillis = 0;
uint16_t pendingPacketId = 0;
bool filesystemReady = false;
bool rtcReady = false;

float voltageMAINS = 0.0f;
float batteryVolt = 0.0f;
float solarVolt = 0.0f;
float solarAmp = 0.0f;
float solarWatts = 0.0f;
float AcIrms = 0.0f;
// This is the name already used by the engineer's display code.
float RMSwatts = 0.0f;
float pvIntervalEnergy_mWh = 0.0f;
float loadIntervalEnergy_mWh = 0.0f;

String calculateSHA256(const String &input) {
  byte shaResult[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char *>(input.c_str()), input.length(), shaResult) != 0) return "";
  char hash[65];
  for (uint8_t i = 0; i < 32; i++) snprintf(hash + (i * 2), 3, "%02x", shaResult[i]);
  return String(hash);
}

bool validRtcTime() {
  DateTime now = rtc.now();
  return now.year() >= 2025 && now.year() <= 2100;
}

bool syncRtcFromNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 15000)) {
    Serial.println("NTP time was unavailable; keeping the DS3231 time.");
    return validRtcTime();
  }
  rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                      timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
  Serial.println("DS3231 synchronised from NTP in UTC.");
  return true;
}

void connectWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager *) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Wi-Fi setup AP");
    lcd.setCursor(0, 1); lcd.print(WiFi.softAPIP());
  });
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting Wi-Fi");
  if (!wm.autoConnect("BlockChainEnergy")) {
    lcd.setCursor(0, 1); lcd.print("Setup timed out");
    delay(2000);
    ESP.restart();
  }
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Wi-Fi connected");
  delay(1200);
}

bool queueRecord(const String &payload) {
  if (!filesystemReady) { Serial.println("ERROR: offline storage unavailable; record not stored."); return false; }
  File queue = LittleFS.open(QUEUE_FILE, FILE_APPEND);
  if (!queue) { Serial.println("ERROR: could not open telemetry queue."); return false; }
  const size_t written = queue.println(payload);
  queue.flush(); queue.close();
  if (written < payload.length() + 1) { Serial.println("ERROR: telemetry queue write incomplete."); return false; }
  Serial.println("Record saved; awaiting MQTT acknowledgement.");
  return true;
}

// Only loop() owns the queue. Callbacks report events without touching files.
void maintainTelemetryQueue() {
  const int reason = mqttDisconnectReason.exchange(-1);
  if (reason >= 0) Serial.printf("MQTT disconnected, reason %d.\n", reason);
  const uint32_t session = mqttSession.load();
  if (session != observedMqttSession) {
    observedMqttSession = session;
    pendingPacketId = 0;
    acknowledgedPacket.store(0);
    Serial.println("MQTT connected.");
    mqttClient.publish(statusTopic.c_str(), 1, true, "online");
  }
  const uint16_t ack = acknowledgedPacket.exchange(0);
  if (pendingPacketId && ack == pendingPacketId) {
    // A reset before persisting this cursor may replay, but cannot discard an unacked record.
    if (prefs.putUInt("queueOffset", pendingQueueEnd) == sizeof(uint32_t)) {
      queueOffset = pendingQueueEnd;
      Serial.printf("Telemetry acknowledged, packet ID %u.\n", pendingPacketId);
    } else Serial.println("ERROR: queue cursor could not be saved; record will replay.");
    pendingPacketId = 0;
  }
  if (!mqttConnected.load()) { pendingPacketId = 0; return; }
  if (pendingPacketId) {
    if (millis() - pendingPublishMillis >= MQTT_ACK_TIMEOUT_MS) {
      Serial.println("MQTT acknowledgement timed out; keeping queued record.");
      mqttClient.disconnect(true);
    }
    return;
  }
  if (!filesystemReady || !LittleFS.exists(QUEUE_FILE)) return;
  File source = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!source) return;
  const uint32_t size = source.size();
  if (queueOffset >= size) {
    source.close();
    // Reset cursor first: interruption can cause duplicates, never skipped records.
    if (prefs.putUInt("queueOffset", 0) != sizeof(uint32_t)) return;
    if (LittleFS.remove(QUEUE_FILE)) queueOffset = 0;
    else prefs.putUInt("queueOffset", queueOffset);
    return;
  }
  source.seek(queueOffset);
  String record = source.readStringUntil('\n');
  pendingQueueEnd = source.position();
  source.close();
  record.trim();
  if (record.isEmpty()) {
    if (prefs.putUInt("queueOffset", pendingQueueEnd) == sizeof(uint32_t)) queueOffset = pendingQueueEnd;
    return;
  }
  pendingPacketId = mqttClient.publish(telemetryTopic.c_str(), 1, false, record.c_str());
  if (pendingPacketId) {
    pendingPublishMillis = millis();
    Serial.printf("Telemetry queued for MQTT, packet ID %u.\n", pendingPacketId);
  }
}

void maintainMqtt() {
  if (mqttConnected || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttemptMillis < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttemptMillis = millis();
  Serial.printf("Connecting to MQTT broker %s:%u ...\n", MQTT_HOST, MQTT_PORT);
  mqttClient.connect();
}

#if ENABLE_PAYLOAD_SIGNATURE
// The Arduino ESP32 build does not enable mbedTLS PEM key parsing on every
// board package.  The supplied key stays in PEM form, but this small decoder
// extracts its P-256 private scalar and signs it through the always-present
// ECDSA API.  No measurement, display, or pin behaviour is changed here.
bool loadPrivateKeyScalar(unsigned char privateScalar[32]) {
  String encoded = String(DEVICE_PRIVATE_KEY_PEM);
  encoded.replace("-----BEGIN PRIVATE KEY-----", "");
  encoded.replace("-----END PRIVATE KEY-----", "");
  encoded.replace("\r", "");
  encoded.replace("\n", "");
  encoded.replace(" ", "");

  size_t derLength = 0;
  const int sizeResult = mbedtls_base64_decode(nullptr, 0, &derLength,
    reinterpret_cast<const unsigned char *>(encoded.c_str()), encoded.length());
  if (sizeResult != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || derLength < 34) return false;

  unsigned char *der = new unsigned char[derLength];
  if (der == nullptr) return false;
  const int decodeResult = mbedtls_base64_decode(der, derLength, &derLength,
    reinterpret_cast<const unsigned char *>(encoded.c_str()), encoded.length());
  if (decodeResult != 0) { delete[] der; return false; }

  // PKCS#8 carries the SEC1 private scalar as OCTET STRING, length 32.
  bool found = false;
  for (size_t i = 0; i + 33 < derLength; ++i) {
    if (der[i] == 0x04 && der[i + 1] == 0x20) {
      memcpy(privateScalar, der + i + 2, 32);
      found = true;
      break;
    }
  }
  delete[] der;
  return found;
}

bool signCanonicalPayload(const String &canonical, String &signatureBase64) {
  unsigned char digest[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char *>(canonical.c_str()), canonical.length(), digest) != 0) return false;
  unsigned char privateScalar[32] = {};
  if (!loadPrivateKeyScalar(privateScalar)) return false;

  mbedtls_ecdsa_context key; mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context random;
  mbedtls_ecdsa_init(&key); mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&random);
  const char *personalisation = "meridian-grid-payload";
  int result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<const unsigned char *>(personalisation), strlen(personalisation));
  if (result == 0) result = mbedtls_ecp_group_load(&key.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
  if (result == 0) result = mbedtls_mpi_read_binary(&key.MBEDTLS_PRIVATE(d), privateScalar, sizeof(privateScalar));
  if (result == 0) result = mbedtls_ecp_keypair_calc_public(&key, mbedtls_ctr_drbg_random, &random);
  unsigned char derSignature[128] = {}; size_t derLength = 0;
  if (result == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
    result = mbedtls_ecdsa_write_signature(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), derSignature,
      sizeof(derSignature), &derLength, mbedtls_ctr_drbg_random, &random);
#else
    result = mbedtls_ecdsa_write_signature(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), derSignature,
      &derLength, mbedtls_ctr_drbg_random, &random);
#endif
  }
  unsigned char encodedSignature[192] = {}; size_t encodedLength = 0;
  if (result == 0) result = mbedtls_base64_encode(encodedSignature, sizeof(encodedSignature), &encodedLength, derSignature, derLength);
  mbedtls_ecdsa_free(&key); mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy);
  if (result != 0) return false;
  signatureBase64 = String(reinterpret_cast<char *>(encodedSignature));
  return true;
}
#endif

void setup() {
  Serial.begin(115200); delay(500);
  Serial.printf("Meridian MQTT runtime fix; reset reason %d; AsyncTCP %s\n", static_cast<int>(esp_reset_reason()), ASYNCTCP_VERSION);
  Wire.begin(I2C_SDA, I2C_SCL);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  voltageSensor.setSensitivity(ZMPT_SENSITIVITY);
  emon1.current(LOAD_CURRENT_PIN, CT_CALIBRATION);
  // init() works with the LiquidCrystal_I2C version used on the engineer's PC
  // and retains the original 16x2 display configuration from the constructor.
  lcd.init(); lcd.backlight(); lcd.clear();
  filesystemReady = LittleFS.begin(false);
  if (!filesystemReady) Serial.println("ERROR: LittleFS mount failed; existing data was not formatted.");
  if (!rtc.begin()) { Serial.println("ERROR: DS3231 RTC was not detected."); while (true) delay(1000); }
  if (!ads.begin(ADS1015_ADDRESS, &Wire)) { Serial.println("ERROR: ADS1015 was not detected."); while (true) delay(1000); }
  ads.setGain(GAIN_ONE);
  prefs.begin("meter_data", false);
  sequenceNo = prefs.getUInt("sequenceNo", 0);
  queueOffset = LittleFS.exists(QUEUE_FILE) ? prefs.getUInt("queueOffset", 0) : 0;
  Serial.printf("Restored sequenceNo: %u\n", sequenceNo);
  connectWiFi();
  rtcReady = syncRtcFromNtp();
  if (!rtcReady) Serial.println("ERROR: RTC has no valid time. Fix DS3231 battery or connect to Wi-Fi for NTP sync.");
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCredentials(MQTT_USERNAME, MQTT_PASSWORD);
  mqttClient.setClientId(DEVICE_ID);
  mqttClient.setWill(statusTopic.c_str(), 1, true, "offline");
  mqttClient.onConnect([](bool) { mqttConnected.store(true); mqttSession.fetch_add(1); });
  mqttClient.onPublish([](uint16_t packetId) { acknowledgedPacket.store(packetId); });
  mqttClient.onDisconnect([](AsyncMqttClientDisconnectReason reason) { mqttConnected.store(false); mqttDisconnectReason.store(static_cast<int>(reason)); });
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("Meridian Grid"); lcd.setCursor(0, 1); lcd.print("Meter starting"); delay(1500);
  lastRecordMillis = millis();
  lastEnergySampleMillis = millis();
}

void loop() {
  read_data();
  updateEnergyIntegration();
  i2c_display();
  maintainMqtt();
  maintainTelemetryQueue();
  if (rtcReady && millis() - lastRecordMillis >= RECORD_INTERVAL_MS) {
    lastRecordMillis += RECORD_INTERVAL_MS;
    buildAndSendPayload();
  }
  delay(1);
}
