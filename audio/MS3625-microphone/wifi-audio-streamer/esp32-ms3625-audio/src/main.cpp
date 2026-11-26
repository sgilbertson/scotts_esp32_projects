#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <esp_wifi.h>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <lwip/sockets.h>
#include "config.h"

// Optional config.h override; existing configurations need no changes.
#ifndef STATS_PORT
#define STATS_PORT 3334
#endif
static_assert(STATS_PORT > 0 && STATS_PORT <= 65535, "Invalid STATS_PORT");
static_assert(STATS_PORT != RECEIVER_PORT, "Statistics and audio need separate ports");

// Original ESP32 + MS3625: SCK=26, WS=25, SD=33, L/R=GND.
static constexpr int PIN_BCLK = 26, PIN_WS = 25, PIN_DATA = 33;
// Experimentally verified; do not infer the receive slot from L/R wiring.
static constexpr int MIC_SLOT_INDEX = 1;
static constexpr uint32_t SAMPLE_RATE = 32000;
static constexpr size_t FRAMES = 512; // 16 ms; always send complete blocks.
static constexpr size_t HEADER_BYTES = 16;
static constexpr size_t PACKET_BYTES = HEADER_BYTES + FRAMES * sizeof(int16_t);
static constexpr size_t QUEUE_BLOCKS = 32; // 512 ms / 32 KiB PCM.
static constexpr int DMA_BUFFERS = 8, DMA_FRAMES = 256; // 64 ms stereo input.
static constexpr int I2S_EVENT_DEPTH = 32;
static constexpr uint32_t READ_TIMEOUT_MS = 100;
// Bound each dequeued block's retry time; the 512 ms ring keeps capturing.
static constexpr uint32_t SEND_RETRY_MS = 5, SEND_DEADLINE_MS = 100;
static constexpr uint32_t REPORT_MS = 5000, RECONNECT_MS = 5000;
static constexpr uint32_t CAPTURE_STACK_BYTES = 4096, NETWORK_STACK_BYTES = 6144;

struct AudioBlock { uint8_t packet[PACKET_BYTES]; };
static AudioBlock blocks[QUEUE_BLOCKS];
static size_t head = 0, occupancy = 0;
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
struct Statistics {
  uint32_t captured = 0, sent = 0, queueDrops = 0;
  uint32_t readErrors = 0, readTimeouts = 0, partialReads = 0;
  uint32_t dmaErrors = 0, dmaOverflows = 0, eventQueueFull = 0;
  uint32_t udpFailures = 0, wifiAborted = 0;
  uint32_t connects = 0, disconnects = 0, reconnectAttempts = 0;
  size_t maxOccupancy = 0;
};
static Statistics stats;
static QueueHandle_t i2sEvents;
static IPAddress receiver;

static void put16(uint8_t *p, uint16_t n) {
  p[0] = static_cast<uint8_t>(n);
  p[1] = static_cast<uint8_t>(n >> 8);
}
static void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}

static void setupI2S() {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = DMA_BUFFERS;
  cfg.dma_buf_len = DMA_FRAMES;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_DATA;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, I2S_EVENT_DEPTH, &i2sEvents));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
}

static void drainI2SEvents() {
  // RX_DONE events are routine. Event loss is possible if this queue fills;
  // observed-full counts are a warning, not a count of lost audio or events.
  const bool full = uxQueueMessagesWaiting(i2sEvents) == I2S_EVENT_DEPTH;
  uint32_t errors = 0, overflows = 0;
  i2s_event_t event;
  while (xQueueReceive(i2sEvents, &event, 0) == pdTRUE) {
    if (event.type == I2S_EVENT_DMA_ERROR) ++errors;
    if (event.type == I2S_EVENT_RX_Q_OVF) ++overflows;
  }
  portENTER_CRITICAL(&stateMux);
  stats.dmaErrors += errors;
  stats.dmaOverflows += overflows;
  stats.eventQueueFull += full;
  portEXIT_CRITICAL(&stateMux);
}

static void captureTask(void *) {
  // Only this task owns these scratch buffers. No heap, Serial or Wi-Fi here.
  static int32_t samples[FRAMES * 2];
  static AudioBlock completed;
  uint32_t sequence = 0;
  size_t filled = 0;
  memcpy(completed.packet, "AUD0", 4);
  put16(completed.packet + 8, FRAMES);
  put16(completed.packet + 10, 0);
  put32(completed.packet + 12, SAMPLE_RATE);
  // Install on core 1 so the I2S interrupt follows capture, away from Wi-Fi.
  setupI2S();
  for (;;) {
    drainI2SEvents();
    size_t bytesRead = 0;
    const size_t requested = sizeof(samples) - filled;
    const esp_err_t err = i2s_read(I2S_NUM_0,
        reinterpret_cast<uint8_t *>(samples) + filled, requested,
        &bytesRead, pdMS_TO_TICKS(READ_TIMEOUT_MS));
    portENTER_CRITICAL(&stateMux);
    if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytesRead == 0)) ++stats.readTimeouts;
    else if (err != ESP_OK) ++stats.readErrors;
    if (bytesRead > 0 && bytesRead < requested) ++stats.partialReads;
    portEXIT_CRITICAL(&stateMux);
    // Retain partial reads, even across timeouts, so every sequenced block
    // represents exactly 512 frames. Driver-side loss has unknown duration.
    filled += bytesRead;
    if (filled != sizeof(samples)) {
      // A persistent driver fault must not spin at priority 4 and starve idle.
      if (bytesRead == 0) vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    filled = 0;
    put32(completed.packet + 4, sequence++);
    for (size_t i = 0; i < FRAMES; ++i) {
      const int16_t pcm = static_cast<int16_t>(samples[2 * i + MIC_SLOT_INDEX] >> 16);
      put16(completed.packet + HEADER_BYTES + 2 * i, static_cast<uint16_t>(pcm));
    }
    // Ring ownership transfers by copying under a short lock. Neither task
    // holds it during I/O. Evict oldest on overflow to bound queued latency.
    portENTER_CRITICAL(&stateMux);
    ++stats.captured;
    if (occupancy == QUEUE_BLOCKS) {
      head = (head + 1) % QUEUE_BLOCKS;
      --occupancy;
      ++stats.queueDrops;
    }
    blocks[(head + occupancy) % QUEUE_BLOCKS] = completed;
    ++occupancy;
    if (occupancy > stats.maxOccupancy) stats.maxOccupancy = occupancy;
    portEXIT_CRITICAL(&stateMux);
  }
}

// Use a nonblocking socket for audio so errno is captured immediately after
// sendto, before framework logging can change it. No per-attempt packet buffer.
static bool transientSendError(int error) {
  return error == ENOMEM || error == ENOBUFS || error == EAGAIN ||
         error == EWOULDBLOCK || error == EINTR;
}

static void closeAudioSocket(int &fd) {
  if (fd >= 0) close(fd);
  fd = -1;
}

static int sendAudio(int &fd, const AudioBlock &block) {
  if (fd < 0) {
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return errno;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
      const int error = errno;
      closeAudioSocket(fd);
      return error;
    }
  }
  sockaddr_in destination = {};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(RECEIVER_PORT);
  destination.sin_addr.s_addr = static_cast<uint32_t>(receiver);
  const ssize_t sent = sendto(fd, block.packet, PACKET_BYTES, 0,
      reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
  if (sent < 0) return errno;
  // UDP sends are atomic. Never retry a nonnegative unexpected short result.
  return sent == PACKET_BYTES ? 0 : EIO;
}

static void networkTask(void *) {
  int audioSocket = -1; // Socket and outgoing scratch belong only to this task.
  WiFiUDP statsUdp; // Independent best-effort diagnostics, also network-task-only.
  static char report[1400]; // One short text datagram; no fragmentation required.
  uint32_t reportId = 0, statsSent = 0, statsFailures = 0;
  static AudioBlock outgoing;
  // These counters have a single owner: networkTask. No cross-task reads.
  uint32_t retries = 0, recovered = 0, sendDrops = 0;
  uint32_t noMemory = 0, noBuffers = 0, wouldBlock = 0, otherErrors = 0;
  int lastSendError = 0;
  bool haveBlock = false;
  uint32_t blockStarted = 0, lastAttempt = 0, attempts = 0;
  bool wasConnected = false;
  uint32_t lastReconnect = millis(), lastReport = millis();
  for (;;) {
    const uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected) {
      closeAudioSocket(audioSocket); // Recreate sockets after a link transition.
      statsUdp.stop();
      wasConnected = connected;
    }
    if (!connected && now - lastReconnect >= RECONNECT_MS) {
      WiFi.reconnect();
      lastReconnect = now;
      portENTER_CRITICAL(&stateMux);
      ++stats.reconnectAttempts;
      portEXIT_CRITICAL(&stateMux);
    }
    if (connected && !haveBlock) {
      portENTER_CRITICAL(&stateMux);
      if (occupancy) {
        outgoing = blocks[head];
        head = (head + 1) % QUEUE_BLOCKS;
        --occupancy;
        haveBlock = true;
        blockStarted = now;
        attempts = 0;
      }
      portEXIT_CRITICAL(&stateMux);
    }
    if (haveBlock) {
      if (WiFi.status() != WL_CONNECTED) {
        portENTER_CRITICAL(&stateMux);
        ++stats.wifiAborted;
        portEXIT_CRITICAL(&stateMux);
        haveBlock = false;
      } else if (attempts && now - blockStarted >= SEND_DEADLINE_MS) {
        // Deadline expiry discards this sequence; never re-enqueue or renumber.
        ++sendDrops;
        haveBlock = false;
      } else if (!attempts || now - lastAttempt >= SEND_RETRY_MS) {
        if (attempts) ++retries;
        ++attempts;
        lastAttempt = now;
        const int error = sendAudio(audioSocket, outgoing);
        if (error == 0) {
          portENTER_CRITICAL(&stateMux);
          ++stats.sent;
          portEXIT_CRITICAL(&stateMux);
          if (attempts > 1) ++recovered;
          haveBlock = false;
        } else {
          lastSendError = error;
          if (error == ENOMEM) ++noMemory;
          else if (error == ENOBUFS) ++noBuffers;
          else if (error == EAGAIN || error == EWOULDBLOCK) ++wouldBlock;
          else ++otherErrors;
          portENTER_CRITICAL(&stateMux);
          ++stats.udpFailures; // Failed attempts, including later recovered sends.
          portEXIT_CRITICAL(&stateMux);
          if (!transientSendError(error)) {
            closeAudioSocket(audioSocket);
            ++sendDrops;
            haveBlock = false;
          }
          // ENOMEM/ENOBUFS/EAGAIN keep the socket and same outgoing block.
        }
      }
    }
    // Yield while waiting for a retry; reports and reconnect logic keep running.
    vTaskDelay(pdMS_TO_TICKS(1));
    if (now - lastReport >= REPORT_MS) {
      Statistics snapshot;
      size_t queued;
      portENTER_CRITICAL(&stateMux);
      snapshot = stats;
      queued = occupancy;
      portEXIT_CRITICAL(&stateMux);
      // Snapshot under lock, then format and perform all I/O outside it.
      // Report IDs advance even offline, exposing missed diagnostic intervals.
      const uint32_t uptime = millis();
      const bool reportConnected = WiFi.status() == WL_CONNECTED;
      // One driver snapshot keeps BSSID, channel and RSSI tied to the same AP.
      // Query only from the network task, outside the capture/statistics lock.
      wifi_ap_record_t ap = {};
      const bool haveAp = reportConnected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
      const int rssi = haveAp ? ap.rssi : 0;
      const unsigned channel = haveAp ? ap.primary : 0;
      char bssid[18] = "unavailable";
      if (haveAp) {
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
      }
      const int length = snprintf(report, sizeof(report),
                    "report=%lu uptime_ms=%lu rssi_dbm=%d bssid=%s channel=%u "
                    "stats_ok=%lu stats_fail=%lu "
                    "capture=%lu udp_ok=%lu queue_drop=%lu queue=%u/%u max=%u "
                    "i2s_error=%lu timeout=%lu partial=%lu dma_error=%lu "
                    "rx_overflow=%lu event_full_seen=%lu udp_fail=%lu wifi_abort=%lu "
                    "wifi=%d connects=%lu disconnects=%lu reconnect_attempts=%lu "
                    "udp_retry=%lu udp_recovered=%lu udp_drop=%lu udp_errno=%d "
                    "udp_enomem=%lu udp_enobufs=%lu udp_eagain=%lu udp_other=%lu "
                    "heap_free=%lu heap_min=%lu heap_largest=%lu\n",
          (unsigned long)reportId++, (unsigned long)uptime, rssi, bssid, channel,
          (unsigned long)statsSent, (unsigned long)statsFailures,
          (unsigned long)snapshot.captured, (unsigned long)snapshot.sent,
          (unsigned long)snapshot.queueDrops, (unsigned)queued, (unsigned)QUEUE_BLOCKS,
          (unsigned)snapshot.maxOccupancy, (unsigned long)snapshot.readErrors,
          (unsigned long)snapshot.readTimeouts, (unsigned long)snapshot.partialReads,
          (unsigned long)snapshot.dmaErrors, (unsigned long)snapshot.dmaOverflows,
          (unsigned long)snapshot.eventQueueFull, (unsigned long)snapshot.udpFailures,
          (unsigned long)snapshot.wifiAborted, reportConnected,
          (unsigned long)snapshot.connects, (unsigned long)snapshot.disconnects,
          (unsigned long)snapshot.reconnectAttempts,
          (unsigned long)retries, (unsigned long)recovered, (unsigned long)sendDrops,
          lastSendError, (unsigned long)noMemory, (unsigned long)noBuffers,
          (unsigned long)wouldBlock, (unsigned long)otherErrors,
          (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
          (unsigned long)ESP.getMaxAllocHeap());
      if (length > 0 && static_cast<size_t>(length) < sizeof(report)) {
        Serial.write(reinterpret_cast<const uint8_t *>(report), length);
        if (reportConnected) {
          // Recompute the subnet broadcast address for DHCP/network changes.
          // Never retry diagnostics or add their failures to audio udp_fail.
          bool ok = false;
          if (statsUdp.beginPacket(WiFi.broadcastIP(), STATS_PORT)) {
            const size_t written = statsUdp.write(
                reinterpret_cast<const uint8_t *>(report), length);
            const int result = statsUdp.endPacket();
            ok = written == static_cast<size_t>(length) && result == 1;
          }
          if (ok) ++statsSent;
          else {
            ++statsFailures;
            statsUdp.stop();
          }
        }
      }
      lastReport = now;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  if (!receiver.fromString(RECEIVER_IP)) {
    Serial.println("Bad RECEIVER_IP in config.h");
    abort();
  }
  // Count actual asynchronous Wi-Fi events, including transitions that occur
  // while the network task is inside a socket call. Callback only touches stats.
  WiFi.onEvent([](WiFiEvent_t event) {
    portENTER_CRITICAL(&stateMux);
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) ++stats.connects;
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) ++stats.disconnects;
    portEXIT_CRITICAL(&stateMux);
  });
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // Never wait for association in capture.
  Serial.println("32 kHz mono, slot 1; capture core 1 / UDP core 0; queue 512 ms");
  // Arduino loop runs at priority 1. Capture blocks on DMA at priority 4;
  // network priority 2 on core 0 stays below the ESP32 Wi-Fi stack tasks.
  if (xTaskCreatePinnedToCore(captureTask, "audio-capture", CAPTURE_STACK_BYTES,
                             nullptr, 4, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(networkTask, "audio-network", NETWORK_STACK_BYTES,
                             nullptr, 2, nullptr, 0) != pdPASS) {
    Serial.println("Cannot create audio tasks");
    abort();
  }
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
