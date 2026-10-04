# Meridian Grid IoT board

ESP32-S3 meter sketch for PV generation and AC load telemetry. This repository contains the source code and **no working secrets**. The prior private engineer's copy was compiled with ESP32 core 3.3.0, AsyncMqttClient 0.9.0 and ESP32Async AsyncTCP 3.3.2. This public package has the same measurement, queue, and signing logic, but moves the meter ID and broker credentials into a local ignored header. It still needs a compile and a physical-board test after configuration.

## What you need

- ESP32-S3 Dev Module; ADS1015 at I2C address `0x48`; DS3231 clock; 16×2 I2C LCD. The original pilot board uses SDA GPIO 8 and SCL GPIO 9. ADS1015 A2 reads PV voltage through the existing divider; A1 reads the ACS712 current-sensor output. GPIO 4 reads the ZMPT101B AC-voltage circuit, and GPIO 1 reads the CT current circuit. **Verify the exact pinout and safe sensor isolation with the hardware engineer before applying PV or mains voltage.** The calibration constants came from the pilot board and must be checked against a reference meter for a different household meter.
- Arduino IDE or CLI with **esp32 by Espressif Systems 3.3.0**, board **ESP32S3 Dev Module**. Install WiFiManager, AsyncMqttClient 0.9.0, **ESP32Async AsyncTCP 3.3.2**, RTClib, Adafruit ADS1X15, ZMPT101B, EmonLib and LiquidCrystal_I2C. The sketch checks the AsyncTCP version at compile time; old 1.1.4 caused the observed TCP assertion/restart. Use the version named here, not the old library.
- A distinct MQTT broker account for each meter, provisioned by the broker administrator. The dashboard does **not** create MQTT accounts. The current sketch uses port 1883 without TLS; use a trusted network or upgrade the transport before field deployment.
- The device's Wi-Fi network credentials, entered locally through WiFiManager. Do not put Wi-Fi passwords in Git.

## Register one household meter

1. On the engineer's computer, run `python3 scripts/generate_meter_key.py`. This creates `1_Esp_pelumi_project/device-signing-secret.h` and `public-key.pem`. The private header stays on that computer and must never be pasted into the dashboard or committed. The script refuses to overwrite an existing identity.
2. Sign in as an admin. Open **Community → Add device**, paste the contents of `public-key.pem`, and select **Register meter**. The dashboard returns a generated meter ID and its telemetry/status MQTT topics. Give the ID and topics to the IoT engineer. The public key can be shared for registration; the private key cannot.
3. Copy `1_Esp_pelumi_project/device-config.h.example` to `device-config.h` in the same sketch folder. Replace `DEVICE_ID` with the generated ID. Enter the broker host, port and the new meter's MQTT username/password supplied by the broker administrator. Topics are derived from the ID in the code, so do not type them separately.
4. Open `1_Esp_pelumi_project/1_Esp_pelumi_project.ino` with all five `.ino` files and the two local `.h` files together. Compile for the selected board; then upload. Open Serial Monitor at **115200 baud**. Use WiFiManager to connect to the intended 2.4 GHz network.
5. Watch for `MQTT connected.` and `Telemetry acknowledged` every minute. In the Devices page, the meter should move from **awaiting first message** to online and show its latest signed readings. Compare PV and load voltage/current to a trusted meter. The LCD, Serial Monitor, dashboard and reference meter should agree within the recorded calibration tolerance. Do not claim measured accuracy from a signature alone.
6. After the backend's ledger submission interval (normally five minutes), inspect the record's Fabric transaction and containing block. The current network has two organisations; registering this meter does not create another Fabric organisation.

The backend rejects readings if the `DEVICE_ID` in the signed payload differs from the MQTT topic or from the public key registered in the dashboard. It also rejects missing/invalid signatures. A registered meter without a configured board shows no readings.

## Connection problems

- No Wi-Fi: check WiFiManager, 2.4 GHz coverage, power supply and Serial Monitor reset reason.
- No MQTT: check broker host/port, a **separate** MQTT account, network reachability, and broker permissions for `microgrid/v1/<device-id>/telemetry` and `/status`. Ask the broker administrator to provision these permissions; Add device does not do it.
- MQTT connected but no dashboard reading: confirm the exact generated device ID, corresponding public/private key pair, monotonically increasing sequence number, and backend Device event log. A broker acknowledgement proves receipt by the broker, not acceptance by PostgreSQL or Fabric.
- Reboots at connection: confirm AsyncTCP **3.3.2**, ESP32 core **3.3.0**, then collect at least ten minutes of Serial Monitor output including the first reset reason.
- Zero power: check current as well as voltage. The firmware suppresses small readings and uses an assumed AC power factor. Inspect wiring and calibration with the intended load connected.

The firmware preserves its sequence number and offline queue across resets. Do not erase flash merely to start a new dashboard test; use the admin's **Start fresh PV test** control, which leaves the ledger and earlier records intact.
