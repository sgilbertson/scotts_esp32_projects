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
- On a full ring, capture discards the **oldest queued block** and counts it. The sender leaves the ring alone while disconnected, so it retains the latest half-second of audio. One additional block can be in flight in the network task; socket stalls can delay that block beyond the ring's time window. Automatic Wi-Fi reconnection is enabled with explicit reconnect attempts every five seconds. Transient audio send failures are retried with a bounded deadline, as described below.
- Every completed capture block receives a sequence number **before enqueue**, including blocks later evicted or unsuccessfully sent. Packets always contain 512 samples; partial I2S reads are accumulated across calls. The unchanged receiver detects sequence gaps (including uint32 wraparound) and inserts the correct amount of silence for discarded complete blocks. No wire-format or receiver change is required.
- Packet header is 16 bytes, little-endian: magic `AUD0`, `uint32 sequence`, `uint16 frames`, `uint16 flags/reserved`, `uint32 sample_rate`; followed by little-endian int16 samples.
- UDP is unauthenticated and unencrypted; use it only on a trusted local LAN.

## Remote statistics (no USB connection needed)

The network transmission task sends the same statistics line printed on Serial
as a separate UDP broadcast every five seconds, on **port 3334** by default.
It uses the current Wi-Fi subnet's broadcast address, so any listening computer
on that LAN can receive it. Audio still goes only to `RECEIVER_IP:RECEIVER_PORT`.
No diagnostic formatting or networking runs in the microphone capture task.

On Ubuntu with OpenBSD netcat, listen in a second terminal:

```bash
nc -u -l -k 3334
```

Or display and save the output:

```bash
nc -u -l -k 3334 | tee esp32-stats.log
```

Start the listener before or during a recording. `-k` keeps the UDP listener
accepting different source endpoints after ESP32 reconnections or resets.
If your netcat variant uses different options, use OpenBSD netcat or
`socat -u UDP4-RECV:3334,reuseaddr -` instead.
If a firewall is active, allow inbound UDP 3334 from the ESP32/LAN.
AP client isolation, guest networks, or broadcast filtering can prevent receipt;
broadcasts normally stay within one subnet.

Optionally define `STATS_PORT` in `include/config.h` to change the port; it must
differ from the audio port. Existing configurations need no edits.

`report` is a zero-based report ID incremented every reporting interval, including
while disconnected. `uptime_ms` is milliseconds since boot (wraps after about
49.7 days). `bssid` identifies the connected access point radio (for example,
`AA:BB:CC:DD:EE:FF`), and `channel` is its primary Wi-Fi channel. These identify
which mesh node/radio is serving the ESP32; compare BSSIDs with your mesh app or
Wi-Fi scanner. `rssi_dbm`, `bssid`, and `channel` come from one driver snapshot.
When disconnected or that query fails, they are `0`, `unavailable`, and `0`
respectively. Five-second snapshots can miss brief changes between reports.
With this fixed
512-frame firmware, the latest completed capture sequence is `capture - 1`
(modulo 2^32, once capture is nonzero), which helps correlate receiver gaps.
`stats_ok` and `stats_fail` count local acceptance/failure of earlier diagnostic
sends; they exclude audio sends and the current report's own send result.

Diagnostics are best effort: there are no retries or offline report backlog.
During a Wi-Fi outage no broadcasts can be delivered; after recovery, cumulative
counters expose losses even if intermediate reports went missing. A missing
report alone does not prove capture stopped. The second socket is closed on link
transitions or diagnostic send failures and recreated on the next attempt.
Only one small datagram is attempted per reporting interval.

## Diagnostics and limits

Five-second reports are cumulative since boot (32-bit counters wrap). `capture` counts complete captured blocks; `udp_ok` counts packets accepted by the local UDP stack, **not delivery acknowledgements**. `queue_drop` counts oldest-block evictions; `queue` is a current occupancy snapshot and `max` its lifetime high-water mark. All shared counters and ring state are protected by the same ESP32 spinlock; no lock is held across socket, I2S, or Serial operations.

`i2s_error` counts non-timeout read errors, `timeout` counts timeout results or zero-byte successful reads, and `partial` counts nonempty short reads (which are accumulated, not discarded). `dma_error` counts driver DMA error events; `rx_overflow` counts driver RX queue overflow events indicating overwritten input. `event_full_seen` warns that the driver event queue was observed full; event notifications may have been lost. Overflow events do **not** provide an exact lost-frame count. Other unreported driver/hardware losses cannot be ruled out, and sequence gaps cannot represent their unknown duration. A clean UDP loss report alone does not prove uninterrupted capture.

`udp_fail` counts failed audio socket/send attempts, including attempts later recovered by a retry; `wifi_abort` counts a dequeued block abandoned after the link went down. `wifi` is the polled link state; `connects` counts acquired-IP events (including initial connection), `disconnects` counts station disconnect events, and `reconnect_attempts` counts explicit retries. Transitions are counted by the asynchronous Wi-Fi callback, so a brief outage during a send is still counted. Reports themselves can be delayed by a blocked socket call without blocking capture.

The receiver cannot distinguish queue loss from network loss using this header. It cannot detect losses before its first packet or after its last packet, and it does not handle ESP32 reset as a new session: restart the receiver after resetting the board. Long outages create large silence gaps on recovery, and the existing receiver allocates silence for a gap in one operation; use short deliberate outages for testing. UDP can still be dropped or reordered on the network or host. I2S failures of unknown duration are reported but cannot be padded accurately.

## Audio send retries and error diagnostics

Audio uses a nonblocking BSD UDP socket in the existing network task. This
captures `errno` immediately after a failing socket operation, avoiding the
Arduino UDP wrapper's per-failure logging. The broadcast statistics socket
continues using `WiFiUDP`. Microphone capture and the receiver protocol are
unchanged.

For `ENOMEM`, `ENOBUFS`, `EAGAIN`/`EWOULDBLOCK`, or `EINTR`, the sender retains the
same outgoing block and sequence and retries at least 5 ms apart, for up to
100 ms from dequeue. No retry is initiated at or after the deadline. A successful
local UDP send is never retried merely because delivery is unknown. Transient
errors keep the socket open. Other errors discard the block and close the socket;
the next block recreates it. A Wi-Fi interruption abandons the in-flight block
and closes the socket on the observed link transition. Queue overflow still
evicts the oldest queued audio independently; sustained congestion can therefore
still cause loss. Scheduling and socket-call duration can delay processing of
the deadline, but capture never waits for retries. Reports continue between
attempts. Retry timing constants live with the other buffer/audio constants.

Additional cumulative fields:

- `udp_retry`: extra send attempts after a block's first attempt.
- `udp_recovered`: blocks successfully sent after at least one failed attempt.
- `udp_drop`: blocks abandoned due to retry deadline or a nontransient error.
  `wifi_abort` separately counts in-flight blocks abandoned on link loss, and
  `queue_drop` separately counts ring evictions.
- `udp_errno`: most recent audio failure's numeric errno; initially zero and
  retained after recovery. `udp_enomem`, `udp_enobufs`, `udp_eagain`, and
  `udp_other` count failed attempts by error category (`EINTR` is in `udp_other`).
  These include socket creation/configuration failures, not just `sendto` errors.
- `heap_free`, `heap_min`, `heap_largest`: current free heap, minimum free heap
  since boot, and largest currently allocatable block, in bytes. These describe
  the ESP32 heap, not specifically Wi-Fi transmit-buffer availability.

`udp_fail` now counts **attempt failures, not discarded blocks**. A rising
`udp_fail` alongside rising `udp_recovered` and stable `udp_drop`/`queue_drop`
means retries are rescuing audio. Receiver gaps should instead be compared with
`udp_drop`, `wifi_abort`, and `queue_drop`, allowing for reporting intervals and
possible additional network losses. Error 12 on this toolchain is `ENOMEM`;
heap metrics help distinguish general heap pressure from fixed driver-buffer
limits, but cannot prove the underlying reason on their own.

## Hardware verification

1. Build with `pio run -e esp32dev`; upload separately when ready. Open the 115200-baud monitor. Start `python3 receiver/receive.py --output baseline.wav --seconds 60` and record speech plus a steady tone. Repeat several times, then run a longer recording. Check receiver missing/out-of-order counts and inspect the WAV for discontinuities.
2. Under normal conditions, expect approximately 62.5 captures and sends per second, low queue occupancy, and zero queue drops, read errors/timeouts, DMA errors/overflows, and UDP failures. Short reads alone are not audio loss. Compare counter differences over the recording; counters include startup time before the receiver began listening.
3. Briefly disable the access point for 2–5 seconds without resetting the ESP32. Capture should keep increasing, the queue should reach 32, and `queue_drop` should increase. Restore Wi-Fi: it should reconnect, sending should resume, and queue occupancy should fall. The receiver should report gaps and insert silence. Confirm no steadily growing latency after recovery. Repeat with the AP unavailable at boot.
4. Exercise a busy/weak Wi-Fi link and compare several equal-length recordings against the previous firmware. Correlate queue drops and UDP failures with receiver gaps. A nonzero `rx_overflow` means capture was unable to drain input in time; do not call that recording lossless even if UDP reports no missing packets.

5. With the retry firmware, repeat the long recording while saving broadcast
   statistics. Check whether `udp_recovered` rises during disturbances and
   whether receiver missing packets decrease. Watch `udp_drop`/`queue_drop` for
   sustained overload, and `udp_enomem`/`heap_min`/`heap_largest` for clues about
   buffer pressure. Briefly interrupt Wi-Fi and verify automatic recovery.

## Pinning to a mesh node

Optionally add this to `include/config.h` to connect only to the nearby node's
2.4 GHz radio:

```cpp
#define WIFI_BSSID_BYTES {0x30, 0x23, 0x03, 0x30, 0x65, 0x24}
```

Replace the six bytes with your desired BSSID. The existing SSID and password
still apply. Channel discovery stays automatic, so a channel change does not
require recompiling. Initial connection and reconnection retain this BSSID
restriction. There is **no fallback** to other nodes if the chosen node is
unavailable; capture continues and the bounded queue discards old audio.
Remove/comment the definition to restore automatic AP selection.

After uploading, verify `bssid=30:23:03:30:65:24` in the broadcast statistics;
the supplied scanner showed channel 8, but the mesh may change it. Compare
RSSI, queue drops, retry-deadline drops, and receiver gaps at the same location.
