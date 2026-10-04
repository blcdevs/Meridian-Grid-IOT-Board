void read_data() {
  float pvVoltageSum = 0.0f, batteryVoltageSum = 0.0f, loadVoltageSum = 0.0f, ctCurrentSum = 0.0f;
  for (uint8_t i = 0; i < VOLTAGE_SAMPLE_COUNT; i++) {
    ctCurrentSum += emon1.calcIrms(1480);
    loadVoltageSum += voltageSensor.getRmsVoltage();
    batteryVoltageSum += ads.computeVolts(ads.readADC_SingleEnded(3)); // original battery input A3
    pvVoltageSum += ads.computeVolts(ads.readADC_SingleEnded(2)); // ADS1015 A2
  }
  AcIrms = ctCurrentSum / VOLTAGE_SAMPLE_COUNT;
  voltageMAINS = loadVoltageSum / VOLTAGE_SAMPLE_COUNT;
  batteryVolt = (batteryVoltageSum / VOLTAGE_SAMPLE_COUNT) * PV_VOLTAGE_DIVIDER_GAIN;
  solarVolt = (pvVoltageSum / VOLTAGE_SAMPLE_COUNT) * PV_VOLTAGE_DIVIDER_GAIN;
  if (voltageMAINS < 20.0f) voltageMAINS = 0.0f;
  if (AcIrms < 0.15f) AcIrms = 0.0f;

  float pvCurrentSensorVoltage = 0.0f;
  for (uint8_t i = 0; i < PV_CURRENT_SAMPLE_COUNT; i++) pvCurrentSensorVoltage += ads.computeVolts(ads.readADC_SingleEnded(1)); // ADS1015 A1
  pvCurrentSensorVoltage /= PV_CURRENT_SAMPLE_COUNT;
  solarAmp = (pvCurrentSensorVoltage - ACS712_ZERO_CURRENT_V) / ACS712_SENSITIVITY_V_PER_A;
  if (solarAmp < 0.1f) solarAmp = 0.0f;
  solarWatts = solarVolt * solarAmp;

  // The circuit provides separate RMS values. This is a calibrated real-power
  // estimate while the declared load power-factor assumption is 1.0.
  RMSwatts = voltageMAINS * AcIrms * LOAD_POWER_FACTOR_ASSUMPTION;
}

void updateEnergyIntegration() {
  const uint32_t now = millis();
  const uint32_t elapsedMs = now - lastEnergySampleMillis;
  lastEnergySampleMillis = now;
  if (elapsedMs == 0 || elapsedMs > 5000UL) return;
  // W x ms / 3600 = mWh. Buckets reset after each 60-second record.
  pvIntervalEnergy_mWh += solarWatts * elapsedMs / 3600.0f;
  loadIntervalEnergy_mWh += RMSwatts * elapsedMs / 3600.0f;
}
