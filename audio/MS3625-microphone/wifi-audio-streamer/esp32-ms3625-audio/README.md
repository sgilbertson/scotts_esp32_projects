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
5. Open the serial monitor at 115200 baud for cumulative streaming diagnostics every five seconds.

`platformio.ini` uses PlatformIO's stable `espressif32` platform and the Arduino framework. The firmware uses ESP-IDF's legacy I2S driver, which is still exposed in Arduino-ESP32 but deprecated in newer ESP-IDF versions. If your future framework no longer includes it, migrate to the standard-channel API.

## Ubuntu receiver

Open a terminal on the Ubuntu PC before powering or resetting the ESP32:

```bash
cd receiver
python3 receive.py --output test.wav --seconds 30
```

The receiver prints the sending ESP32's address, saves a 32kHz 16-bit mono WAV file, and counts missing UDP packets. Lost packets are padded with silence to keep recording duration aligned. The first packet and final summary report `first_seq`; the summary also reports `last_seq`. Each gap reports its missing sequence range and its start/end time in the WAV, measured from the first accepted packet (including inserted silence), rather than wall-clock arrival time. `--seconds 0` records until Ctrl-C. Open the resulting WAV in Audacity.

If no data arrives, check the PC IP, Wi-Fi isolation and Ubuntu firewall. For ufw, if enabled, allow the sender's UDP packets on port 3333 (prefer restricting to your LAN).

## Notes / troubleshooting

- The microphone sends 24-bit words in 32-bit I2S slots; the firmware converts to signed 16-bit samples.
- Captures both stereo slots and selects **slot 1**, experimentally verified with L/R grounded. Keep this setting.
- For quiet signals, amplify *digitally on the PC* during exploratory analysis rather than immediately adding gain in firmware. If clipping occurs, reduce any gain you subsequently add.
- Capture runs at priority 4 on core 1; UDP runs at priority 2 on core 0, below the Wi-Fi stack. Arduino's idle loop runs at priority 1. Task stacks are 4096 and 6144 bytes respectively. I2S is installed from capture so its interrupt is allocated on core 1.
- Eight DMA buffers of 256 stereo frames provide 64 ms of input buffering. A preallocated ring holds 32 complete 512-frame PCM blocks (512 ms, 33,280 bytes including headers). Capture and sender each own a separate scratch block; ring transfers use a short critical section shared with statistics. No network calls, logging, heap allocation, or waiting for queue space occurs in the steady-state capture path.
- On a full ring, capture discards the **oldest queued block** and counts it. The sender leaves the ring alone while disconnected, so it retains the latest half-second of audio. One additional block can be in flight in the network task; socket stalls can delay that block beyond the ring's time window. Automatic Wi-Fi reconnection is enabled with explicit reconnect attempts every five seconds. Failed sends are not retried.
- Every completed capture block receives a sequence number **before enqueue**, including blocks later evicted or unsuccessfully sent. Packets always contain 512 samples; partial I2S reads are accumulated across calls. The unchanged receiver detects sequence gaps (including uint32 wraparound) and inserts the correct amount of silence for discarded complete blocks. No wire-format or receiver change is required.
- Packet header is 16 bytes, little-endian: magic `AUD0`, `uint32 sequence`, `uint16 frames`, `uint16 flags/reserved`, `uint32 sample_rate`; followed by little-endian int16 samples.
- UDP is unauthenticated and unencrypted; use it only on a trusted local LAN.

## Diagnostics and limits

Five-second reports are cumulative since boot (32-bit counters wrap). `capture` counts complete captured blocks; `udp_ok` counts packets accepted by the local UDP stack, **not delivery acknowledgements**. `queue_drop` counts oldest-block evictions; `queue` is a current occupancy snapshot and `max` its lifetime high-water mark. All shared counters and ring state are protected by the same ESP32 spinlock; no lock is held across socket, I2S, or Serial operations.

`i2s_error` counts non-timeout read errors, `timeout` counts timeout results or zero-byte successful reads, and `partial` counts nonempty short reads (which are accumulated, not discarded). `dma_error` counts driver DMA error events; `rx_overflow` counts driver RX queue overflow events indicating overwritten input. `event_full_seen` warns that the driver event queue was observed full; event notifications may have been lost. Overflow events do **not** provide an exact lost-frame count. Other unreported driver/hardware losses cannot be ruled out, and sequence gaps cannot represent their unknown duration. A clean UDP loss report alone does not prove uninterrupted capture.

`udp_fail` counts begin/write/end failures while the link initially appeared up; `wifi_abort` counts a dequeued block abandoned after the link went down. `wifi` is the polled link state; `connects` counts acquired-IP events (including initial connection), `disconnects` counts station disconnect events, and `reconnect_attempts` counts explicit retries. Transitions are counted by the asynchronous Wi-Fi callback, so a brief outage during a send is still counted. Reports themselves can be delayed by a blocked socket call without blocking capture.

The receiver cannot distinguish queue loss from network loss using this header. It cannot detect losses before its first packet or after its last packet, and it does not handle ESP32 reset as a new session: restart the receiver after resetting the board. Long outages create large silence gaps on recovery, and the existing receiver allocates silence for a gap in one operation; use short deliberate outages for testing. UDP can still be dropped or reordered on the network or host. I2S failures of unknown duration are reported but cannot be padded accurately.

## Hardware verification

1. Build with `pio run -e esp32dev`; upload separately when ready. Open the 115200-baud monitor. Start `python3 receiver/receive.py --output baseline.wav --seconds 60` and record speech plus a steady tone. Repeat several times, then run a longer recording. Check receiver missing/out-of-order counts and inspect the WAV for discontinuities.
2. Under normal conditions, expect approximately 62.5 captures and sends per second, low queue occupancy, and zero queue drops, read errors/timeouts, DMA errors/overflows, and UDP failures. Short reads alone are not audio loss. Compare counter differences over the recording; counters include startup time before the receiver began listening.
3. Briefly disable the access point for 2–5 seconds without resetting the ESP32. Capture should keep increasing, the queue should reach 32, and `queue_drop` should increase. Restore Wi-Fi: it should reconnect, sending should resume, and queue occupancy should fall. The receiver should report gaps and insert silence. Confirm no steadily growing latency after recovery. Repeat with the AP unavailable at boot.
4. Exercise a busy/weak Wi-Fi link and compare several equal-length recordings against the previous firmware. Correlate queue drops and UDP failures with receiver gaps. A nonzero `rx_overflow` means capture was unable to drain input in time; do not call that recording lossless even if UDP reports no missing packets.
