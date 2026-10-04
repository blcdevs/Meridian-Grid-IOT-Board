void i2c_display() {
  // Cycle display through 3 screens every 2.5 seconds
  if (millis() - lastDisplaySwitch >= 3000) {
    lastDisplaySwitch = millis();
    displayScreen = (displayScreen + 1) % 3; // Rotates between 0, 1, and 2
    lcd.clear();
  }

  if (displayScreen == 0) {
    // --- SCREEN 1: Project Name & Battery Voltage ---
    lcd.setCursor(0, 0);
    lcd.print("BlockChainEnergy");

    lcd.setCursor(0, 1);
    lcd.print("Bat V: ");
    lcd.print(batteryVolt, 1);
    lcd.print("V   ");
  }
  else if (displayScreen == 1) {
    // --- SCREEN 2: Solar Voltage, Current & Production Watts ---
    lcd.setCursor(0, 0);
    lcd.print("PV: ");
    lcd.print(solarVolt, 1);
    lcd.print("V  ");
    lcd.print(solarAmp, 1);
    lcd.print("A ");

    lcd.setCursor(0, 1);
    lcd.print("PV Power: ");
    lcd.print((int)solarWatts);
    lcd.print("W ");
  }
  else if (displayScreen == 2) {
    // --- SCREEN 3: Inverter AC Voltage, Current & Consumption Watts ---
    lcd.setCursor(0, 0);
    lcd.print("AC: ");
    lcd.print((int)voltageMAINS);
    lcd.print("V  ");
    lcd.print(AcIrms, 1);
    lcd.print("A ");

    lcd.setCursor(0, 1);
    lcd.print("AC Load: ");
    lcd.print((int)RMSwatts);
    lcd.print("W ");
  }
}
