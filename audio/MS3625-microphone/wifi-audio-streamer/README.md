# README for WiFi audio streamer using MS3625 microphone and ESP32

## Introduction

The intent of this project is to gather sample audio data using the MS3625, which can be used to determine what processing would be required to identify specific sounds. Using the experimental results, algorithms can be incorporated into a different ESP32 project to detect specific sounds.

A simple example would be detecting the buzzer that indicates when my washing machine has finished running a load. A Zigbee ESPHome project would be used to detect that sound and trigger notifications through Home Assistant.

## Hardware

- MS3625 microphone -- [https://manuals.plus/ae/1005008755758954](https://manuals.plus/ae/1005008755758954)
- ESP32 (original model with 30 pins)
- USB cable
- Power supply


## Wiring to an original ESP32

For a typical ESP32-WROOM-32 development board, I'd use these connections:

| MS3625 pin | ESP32 connection | Function                 |
| ---------- | ---------------- | ------------------------ |
| VDD        | 3V3              | Power                    |
| GND        | GND              | Ground                   |
| SCK        | GPIO26           | I²S bit clock            |
| WS         | GPIO25           | I²S word select          |
| SD         | GPIO33           | Digital audio into ESP32 |
| L/R        | GND              | Select left channel      |

The GPIO assignments are configurable, so there's nothing particularly special about these three GPIO numbers.

Use **3.3 V power, not 5 V**. Connect L/R firmly to ground rather than leaving it floating.

The microphone is the I²S slave: the ESP32 generates SCK and WS, and the microphone returns audio samples on SD.

## A useful detail about the audio format

The MS3625 documentation specifies:

- 24-bit digital audio output
- Approximately 60 Hz–15 kHz frequency response
- Approximately 61 dBA signal-to-noise ratio
- Left/right channel selection through the L/R pin

We'll capture the microphone's 24-bit samples in 32-bit I²S slots. The ESP32 can then scale those samples to 16-bit PCM for network transmission. That is preferable to configuring the microphone for 16-bit I²S slots, because this family of microphones uses a 24-bit output format.

## Future possibility: two microphones

I might want to experiment with two microphones on a single ESP32. I²S can carry stereo audio, and the L/R input determines which channel a microphone uses.

With compatible microphone output timing, one microphone can be configured as left and the other as right, sharing SCK, WS, and SD.

This would allow me to experiment with sound direction, stereo recordings, and possibly estimating which microphone is closer to the sound source. One of the use-cases I have in mind is Hummingbird detection, and having one microphone closer to the bird than the other might provide a reliable way to distinguish its wingbeats from other sounds.

## Implementation strategy

Basic components:

- **ESP32:** Arduino framework, I²S DMA input, 32 kHz sampling, UDP transmission.
    - The development environment is PlatformIO (I used vscode)
- **Ubuntu PC:** Python UDP receiver that writes 16-bit WAV files.
- **Testing:** Record speech, hand claps, and a steady tone, then check the results in Audacity.

For reliable recording, The receiver should check sequence numbers and report dropped packets. Since the eventual goal is frequency analysis, knowing that the recordings have no missing samples is important.

We'll retain the option to record 24-bit data later if 16-bit recordings prove insufficient.

## Build steps

First install the "PlatformIO IDE" plugin for vscode (which requires a vscode restart).  Ensure your computer has the appropriate USB-to-UART drivers (typically CP210x or CH34x) installed so it recognizes your ESP32 when plugged in.

Copy include/config.h.example to include/config.h and enter your Wi-Fi credentials and Ubuntu computer's LAN IP address.

Open the project (i.e., the folder containing platform.ini) using the PlatformIO IDE (icon in the left sidebar looks like an alien's head). Build, upload, etc. using the tools in the PlatformIO sidebar.

## Usage

With the ESP32 running and connected to your WiFi, on the receiving PC do:

```bash
cd receiver
python3 receive.py --output test.wav --seconds 30
```

You should receive a 16-bit mono audio file named `test.wav` in the receiver directory, with a sample rate of 32,000 Hz, over UDP. The audio data rate is 64 kB/s, the packet size is 1040 bytes and the packet interval is 16 ms (62.5 per second). That's 65 kBps or about 0.5 Mbps, which shouldn't represent a heavy load on WiFi.
