#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <cstring>
#include "config.h"

// MS3625: VDD -> 3V3, GND -> GND, SCK -> GPIO26,
// WS -> GPIO25, SD -> GPIO33, L/R -> GND.
static constexpr int PIN_BCLK = 26;
static constexpr int PIN_WS = 25;
static constexpr int PIN_DATA = 33;
static constexpr uint32_t SAMPLE_RATE = 32000;
static constexpr size_t FRAMES = 512;

// Usually left is first in each I2S stereo frame. If audio is silent,
// try changing between 0 and 1 after verifying wiring. Channel ordering can vary with drivers.
static constexpr int MIC_SLOT_INDEX = 1;

// 32-bit samples x 2 stereo slots, even though only one mic is connected.
static int32_t i2sSamples[FRAMES * 2];
static uint8_t packet[16 + FRAMES * sizeof(int16_t)];
static WiFiUDP udp;
static IPAddress receiver;
static uint32_t sequenceNo = 0;

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
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_DATA;

  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("MS3625 I2S UDP streamer starting");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWi-Fi connected, ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
  if (!receiver.fromString(RECEIVER_IP)) {
    Serial.println("Bad RECEIVER_IP in config.h");
    abort();
  }
  setupI2S();
  memcpy(packet, "AUD0", 4);
  put32(packet + 12, SAMPLE_RATE);
  Serial.printf("Streaming %lu Hz mono PCM to %s:%d\n",
                static_cast<unsigned long>(SAMPLE_RATE), RECEIVER_IP, RECEIVER_PORT);
}

#define DEBUG_COUNT_NONZERO_SAMPLES 0 // troubleshooting an issue where all samples in the received file were zero

void loop() {
  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(I2S_NUM_0, i2sSamples, sizeof(i2sSamples),
                                 &bytesRead, portMAX_DELAY);
  if (err != ESP_OK || bytesRead == 0) {
    Serial.printf("I2S read error: %d\n", static_cast<int>(err));
    return;
  }
  const size_t frames = bytesRead / (2 * sizeof(int32_t));
  if (frames == 0) return;

#if DEBUG_COUNT_NONZERO_SAMPLES

  // Diagnose both I2S channels before converting to 16-bit audio.
  static uint32_t lastDiag = 0;
  static uint32_t maxLeft = 0;
  static uint32_t maxRight = 0;
  static uint32_t nonzeroLeft = 0;
  static uint32_t nonzeroRight = 0;

  for (size_t i = 0; i < frames; ++i) {
    int32_t a = i2sSamples[2 * i];
    int32_t b = i2sSamples[2 * i + 1];

    uint32_t magA = a < 0 ? -(int64_t)a : a;
    uint32_t magB = b < 0 ? -(int64_t)b : b;

    if (magA > maxLeft) maxLeft = magA;
    if (magB > maxRight) maxRight = magB;
    if (a != 0) ++nonzeroLeft;
    if (b != 0) ++nonzeroRight;
  }

  if (millis() - lastDiag >= 2000) {
    Serial.printf(
      "I2S slot 0: peak=%lu nonzero=%lu | "
      "slot 1: peak=%lu nonzero=%lu\n",
      (unsigned long)maxLeft,
      (unsigned long)nonzeroLeft,
      (unsigned long)maxRight,
      (unsigned long)nonzeroRight
    );

    Serial.printf("Raw samples: %08lX %08lX %08lX %08lX\n",
      (unsigned long)(uint32_t)i2sSamples[0],
      (unsigned long)(uint32_t)i2sSamples[1],
      (unsigned long)(uint32_t)i2sSamples[2],
      (unsigned long)(uint32_t)i2sSamples[3]);

    maxLeft = maxRight = 0;
    nonzeroLeft = nonzeroRight = 0;
    lastDiag = millis();
  }

#endif // DEBUG_COUNT_NONZERO_SAMPLES

  put32(packet + 4, sequenceNo++);
  put16(packet + 8, static_cast<uint16_t>(frames));
  put16(packet + 10, 0);

  // I2S 24 significant bits are normally left-aligned in a 32-bit word.
  // Shift by 16 to produce signed 16-bit PCM with no undefined overflow.
  int16_t peak = 0;
  for (size_t i = 0; i < frames; ++i) {
    const int32_t raw = i2sSamples[2 * i + MIC_SLOT_INDEX];
    const int16_t sample = static_cast<int16_t>(raw >> 16);
    put16(packet + 16 + i * 2, static_cast<uint16_t>(sample));
    const int32_t mag = sample < 0 ? -static_cast<int32_t>(sample) : sample;
    if (mag > peak) peak = static_cast<int16_t>(mag > 32767 ? 32767 : mag);
  }
  if (WiFi.status() == WL_CONNECTED && udp.beginPacket(receiver, RECEIVER_PORT)) {
    udp.write(packet, 16 + frames * 2);
    udp.endPacket();
  }
  static uint32_t lastReport = 0;
  static int16_t maxPeak = 0;
  if (peak > maxPeak) maxPeak = peak;
  if (millis() - lastReport >= 2000) {
    Serial.printf("seq=%lu peak=%d wifi=%d\n",
                  static_cast<unsigned long>(sequenceNo), maxPeak,
                  WiFi.status() == WL_CONNECTED);
    lastReport = millis();
    maxPeak = 0;
  }
}
