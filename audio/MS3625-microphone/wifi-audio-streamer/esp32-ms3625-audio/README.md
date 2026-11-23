# ESP32 + MS3625 Wi-Fi audio streamer

A minimal PlatformIO (Arduino framework) project for an **original ESP32** and an **MS3625 I2S microphone**. Streams mono 16-bit PCM in UDP packets to an Ubuntu/Linux WAV recorder. No third-party Python packages required.

## Wiring

| MS3625 | ESP32 Dev Module |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SCK | GPIO26 |
| WS | GPIO25 |
| SD | GPIO33 |
| L/R | GND (left channel) |

Do not connect VDD to 5V. Use reasonably short signal leads.

## ESP32 setup

1. Open this folder in VS Code with PlatformIO.
2. Copy `include/config.h.example` to `include/config.h`.
3. Set `WIFI_SSID`, `WIFI_PASSWORD`, and `RECEIVER_IP` (your Ubuntu PC's LAN address).
4. Select the `esp32dev` environment. Build and Upload.
5. Open the serial monitor at 115200 baud for status and peak readings.

`platformio.ini` uses PlatformIO's stable `espressif32` platform and the Arduino framework. The firmware uses ESP-IDF's legacy I2S driver, which is still exposed in Arduino-ESP32 but deprecated in newer ESP-IDF versions. If your future framework no longer includes it, migrate to the standard-channel API.

## Ubuntu receiver

Open a terminal on the Ubuntu PC before powering or resetting the ESP32:

```bash
cd receiver
python3 receive.py --output test.wav --seconds 30
```

The receiver prints the sending ESP32's address, saves a 32kHz 16-bit mono WAV file, and counts missing UDP packets. Lost packets are padded with silence to keep recording duration aligned. `--seconds 0` records until Ctrl-C. Open the resulting WAV in Audacity.

If no data arrives, check the PC IP, Wi-Fi isolation and Ubuntu firewall. For ufw, if enabled, allow the sender's UDP packets on port 3333 (prefer restricting to your LAN).

## Notes / troubleshooting

- The microphone sends 24-bit words in 32-bit I2S slots; the firmware converts to signed 16-bit samples.
- Captures both stereo slots and chooses slot 0. If audio is silent despite correct wiring, change `MIC_SLOT_INDEX` to 1 in `src/main.cpp`; confirm by clapping near the mic while watching serial `peak=` output.
- For quiet signals, amplify *digitally on the PC* during exploratory analysis rather than immediately adding gain in firmware. If clipping occurs, reduce any gain you subsequently add.
- This first version transmits directly from a single loop. Long Wi-Fi stalls could cause I2S DMA overrun, which UDP sequence counters **cannot detect**. A more robust subsequent version should decouple capture and sending with a queue, DMA-overrun metrics, and/or sample counters.
- Packet header is 16 bytes, little-endian: magic `AUD0`, `uint32 sequence`, `uint16 frames`, `uint16 flags/reserved`, `uint32 sample_rate`; followed by little-endian int16 samples.
- UDP is unauthenticated and unencrypted; use it only on a trusted local LAN.
