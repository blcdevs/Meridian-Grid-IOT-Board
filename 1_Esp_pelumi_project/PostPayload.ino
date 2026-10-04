String canonicalPayload(const char *timestampUtc, uint32_t recordSequence,
                        int32_t pvVoltage_mV, int32_t pvCurrent_mA, int32_t pvPower_mW, int32_t pvEnergy_mWh,
                        int32_t loadVoltage_mV, int32_t loadCurrent_mA, int32_t loadRealPower_mW, int32_t loadEnergy_mWh) {
  char canonical[700];
  snprintf(canonical, sizeof(canonical),
           "{\"schemaVersion\":\"1.0\",\"deviceId\":\"%s\",\"sequenceNo\":%lu,"
           "\"timestampUtc\":\"%s\",\"intervalSeconds\":60,"
           "\"pvVoltage_mV\":%ld,\"pvCurrent_mA\":%ld,\"pvPower_mW\":%ld,\"pvEnergy_mWh\":%ld,"
           "\"loadVoltage_mV\":%ld,\"loadCurrent_mA\":%ld,\"loadRealPower_mW\":%ld,\"loadEnergy_mWh\":%ld}",
           DEVICE_ID, static_cast<unsigned long>(recordSequence), timestampUtc,
           static_cast<long>(pvVoltage_mV), static_cast<long>(pvCurrent_mA), static_cast<long>(pvPower_mW), static_cast<long>(pvEnergy_mWh),
           static_cast<long>(loadVoltage_mV), static_cast<long>(loadCurrent_mA), static_cast<long>(loadRealPower_mW), static_cast<long>(loadEnergy_mWh));
  return String(canonical);
}

void buildAndSendPayload() {
  const DateTime now = rtc.now();
  char timestampUtc[25];
  snprintf(timestampUtc, sizeof(timestampUtc), "%04d-%02d-%02dT%02d:%02d:%02dZ", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  const int32_t pvVoltage_mV = lroundf(solarVolt * 1000.0f);
  const int32_t pvCurrent_mA = lroundf(solarAmp * 1000.0f);
  const int32_t pvPower_mW = lroundf(solarWatts * 1000.0f);
  const int32_t loadVoltage_mV = lroundf(voltageMAINS * 1000.0f);
  const int32_t loadCurrent_mA = lroundf(AcIrms * 1000.0f);
  const int32_t loadRealPower_mW = lroundf(RMSwatts * 1000.0f);
  const int32_t pvEnergy_mWh = lroundf(pvIntervalEnergy_mWh);
  const int32_t loadEnergy_mWh = lroundf(loadIntervalEnergy_mWh);

  // Save before transmit so a reboot or outage cannot repeat a sequence number.
  const uint32_t recordSequence = ++sequenceNo;
  prefs.putUInt("sequenceNo", sequenceNo);
  const String canonical = canonicalPayload(timestampUtc, recordSequence,
    pvVoltage_mV, pvCurrent_mA, pvPower_mW, pvEnergy_mWh,
    loadVoltage_mV, loadCurrent_mA, loadRealPower_mW, loadEnergy_mWh);
  const String payloadHash = calculateSHA256(canonical);
  if (payloadHash.length() != 64) { Serial.println("ERROR: SHA-256 hash could not be created."); return; }
  String payload = canonical;
  payload.remove(payload.length() - 1);
  payload += ",\"payloadHash\":\"" + payloadHash + "\"}";
#if ENABLE_PAYLOAD_SIGNATURE
  String signature;
  if (!signCanonicalPayload(canonical, signature)) { Serial.println("ERROR: device signature could not be created."); return; }
  payload.remove(payload.length() - 1);
  payload += ",\"signatureAlgorithm\":\"ECDSA_P256_SHA256\",\"signature\":\"" + signature + "\"}";
#endif
  Serial.println(payload);
  if (!queueRecord(payload)) { Serial.println("ERROR: record retained in energy bucket because storage failed."); return; }
  // These are one-minute interval energy values, not lifetime totals.
  pvIntervalEnergy_mWh = 0.0f;
  loadIntervalEnergy_mWh = 0.0f;
}
