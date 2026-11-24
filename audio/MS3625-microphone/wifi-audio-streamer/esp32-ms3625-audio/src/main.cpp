#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <cstring>
#include "config.h"

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

static void networkTask(void *) {
  WiFiUDP udp; // Socket and outgoing scratch belong exclusively to this task.
  static AudioBlock outgoing;
  bool wasConnected = false;
  uint32_t lastReconnect = millis(), lastReport = millis();
  for (;;) {
    const uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected) {
      udp.stop(); // Recreate socket after a link transition.
      wasConnected = connected;
    }
    if (!connected && now - lastReconnect >= RECONNECT_MS) {
      WiFi.reconnect();
      lastReconnect = now;
      portENTER_CRITICAL(&stateMux);
      ++stats.reconnectAttempts;
      portEXIT_CRITICAL(&stateMux);
    }
    bool haveBlock = false;
    if (connected) {
      portENTER_CRITICAL(&stateMux);
      if (occupancy) {
        outgoing = blocks[head];
        head = (head + 1) % QUEUE_BLOCKS;
        --occupancy;
        haveBlock = true;
      }
      portEXIT_CRITICAL(&stateMux);
    }
    if (haveBlock) {
      // No retries: failure consumes this sequence, visible as a receiver gap.
      bool ok = false;
      const bool linkAvailable = WiFi.status() == WL_CONNECTED;
      if (linkAvailable && udp.beginPacket(receiver, RECEIVER_PORT)) {
        const size_t written = udp.write(outgoing.packet, PACKET_BYTES);
        const int result = udp.endPacket();
        ok = written == PACKET_BYTES && result == 1;
      }
      if (!ok) udp.stop();
      portENTER_CRITICAL(&stateMux);
      if (ok) ++stats.sent;
      else if (!linkAvailable) ++stats.wifiAborted;
      else ++stats.udpFailures;
      portEXIT_CRITICAL(&stateMux);
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (now - lastReport >= REPORT_MS) {
      Statistics snapshot;
      size_t queued;
      portENTER_CRITICAL(&stateMux);
      snapshot = stats;
      queued = occupancy;
      portEXIT_CRITICAL(&stateMux);
      Serial.printf("capture=%lu udp_ok=%lu queue_drop=%lu queue=%u/%u max=%u "
                    "i2s_error=%lu timeout=%lu partial=%lu dma_error=%lu "
                    "rx_overflow=%lu event_full_seen=%lu udp_fail=%lu wifi_abort=%lu "
                    "wifi=%d connects=%lu disconnects=%lu reconnect_attempts=%lu\n",
          (unsigned long)snapshot.captured, (unsigned long)snapshot.sent,
          (unsigned long)snapshot.queueDrops, (unsigned)queued, (unsigned)QUEUE_BLOCKS,
          (unsigned)snapshot.maxOccupancy, (unsigned long)snapshot.readErrors,
          (unsigned long)snapshot.readTimeouts, (unsigned long)snapshot.partialReads,
          (unsigned long)snapshot.dmaErrors, (unsigned long)snapshot.dmaOverflows,
          (unsigned long)snapshot.eventQueueFull, (unsigned long)snapshot.udpFailures,
          (unsigned long)snapshot.wifiAborted, connected,
          (unsigned long)snapshot.connects, (unsigned long)snapshot.disconnects,
          (unsigned long)snapshot.reconnectAttempts);
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
