#include "buzz_detector.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::buzz_detector {
static const char *const TAG = "buzz_detector";
void BuzzDetector::setup() {
  // Allocate once. Only small interval results cross to the ESPHome/Zigbee loop.
  settings_queue_ = xQueueCreate(1, sizeof(buzz::Settings));
  result_queue_ = xQueueCreate(16, sizeof(buzz::Result));
  if (!settings_queue_ || !result_queue_) { mark_failed(); return; }
  detected_->publish_state(false);
  ready_->publish_state(false);
  if (xTaskCreate(task_entry, "buzz-capture", 6144, this, 5, nullptr) != pdPASS) mark_failed();
}
void BuzzDetector::dump_config() {
  ESP_LOGCONFIG(TAG, "32 kHz I2S: BCLK=%d WS=%d DIN=%d slot=%u", bclk_, ws_, data_, slot_);
}
void BuzzDetector::submit(const buzz::Result &r) {
  if (xQueueSend(result_queue_, &r, 0) != pdTRUE) drops_.fetch_add(1);
}
void BuzzDetector::capture() {
  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = 8;
  channel.dma_frame_num = 256;
  esp_err_t err = i2s_new_channel(&channel, nullptr, &rx);
  if (err == ESP_OK) {
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(buzz::SAMPLE_RATE);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk_);
    config.gpio_cfg.ws = static_cast<gpio_num_t>(ws_);
    config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    config.gpio_cfg.din = static_cast<gpio_num_t>(data_);
    err = i2s_channel_init_std_mode(rx, &config);
  }
  if (err == ESP_OK) {
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = overflow_cb;
    err = i2s_channel_register_event_callback(rx, &callbacks, this);
  }
  if (err == ESP_OK) err = i2s_channel_enable(rx);
  if (err != ESP_OK) {
    if (rx) i2s_del_channel(rx);
    fatal_.store(err);
    vTaskDelete(nullptr);
    return;
  }
  // All raw samples, filter/history state and accumulators belong to this task.
  // No Zigbee, logging, allocation or blocking queue writes in the capture loop.
  buzz::Detector detector;
  buzz::Settings settings;
  detector.configure(settings);
  int32_t samples[256 * 2];
  size_t filled = 0;
  uint32_t last_overflow = 0;
  for (;;) {
    if (xQueueReceive(settings_queue_, &settings, 0) == pdTRUE) {
      detector.configure(settings);
      submit(buzz::Result{}); // Invalidate old detection and warm up history again.
    }
    size_t bytes = 0;
    err = i2s_channel_read(rx, reinterpret_cast<uint8_t *>(samples) + filled,
                           sizeof(samples) - filled, &bytes, 100);
    const uint32_t ovf = overflows_count_.load();
    if (err != ESP_OK || bytes == 0 || ovf != last_overflow) {
      if (err != ESP_OK || bytes == 0) errors_.fetch_add(1);
      last_overflow = ovf;
      detector.reset(); filled = 0;
      submit(buzz::Result{});
      vTaskDelay(1);
      continue;
    }
    filled += bytes;
    if (filled != sizeof(samples)) { if (!bytes) vTaskDelay(1); continue; }
    filled = 0;
    for (unsigned i = 0; i < 256; ++i) {
      buzz::Result result;
      if (detector.sample(static_cast<int16_t>(samples[2*i + slot_] >> 16), result)) submit(result);
    }
  }
}
void BuzzDetector::loop() {
  if (fatal_.load()) {
    ESP_LOGE(TAG, "I2S setup failed: %d", fatal_.load());
    detected_->publish_state(false); ready_->publish_state(false);
    mark_failed(); return;
  }
  if (is_failed()) return;
  auto value = [this](unsigned i, float fallback) {
    return controls_[i]->has_state() && std::isfinite(controls_[i]->state) ? controls_[i]->state : fallback;
  };
  buzz::Settings settings;
  settings.interval_ms = std::clamp<unsigned>(value(0, 500), 100, 2000);
  settings.lookback_s = std::clamp(value(1, 5), .5f, 30.f);
  settings.threshold_db = std::clamp(value(2, 12), 1.f, 60.f);
  settings.consecutive = std::clamp<unsigned>(value(3, 5), 1, 100);
  settings.minimum_dbfs = std::clamp(value(4, -60), -120.f, 0.f);
  settings.highpass_hz = std::clamp(value(5, 2500), 0.f, 12000.f);
  settings.release_intervals = std::clamp<unsigned>(value(6, 1), 1, 20);
  settings.cooldown_s = std::clamp(value(7, 10), 0.f, 300.f);
  if (!(settings == configured_)) {
    xQueueOverwrite(settings_queue_, &settings);
    configured_ = settings;
  }
  // A stalled consumer may miss an ON/OFF edge. Fail closed and rebuild the
  // baseline instead of trusting a truncated run of interval results.
  const uint32_t dropped = drops_.load();
  if (dropped != handled_drops_) {
    handled_drops_ = dropped;
    xQueueReset(result_queue_);
    xQueueOverwrite(settings_queue_, &configured_);
    detected_->publish_state(false);
    ready_->publish_state(false);
  }
  buzz::Result result;
  const uint32_t now = millis();
  bool got = false;
  while (xQueueReceive(result_queue_, &result, 0) == pdTRUE) {
    // Entity publication and Zigbee reports happen only in the main loop.
    detected_->publish_state(result.detected);
    ready_->publish_state(result.ready);
    got = true;
  }
  if (got && now - last_report_ >= report_ms_) {
    level_->publish_state(result.level);
    baseline_->publish_state(result.baseline);
    rise_->publish_state(result.rise);
    streak_->publish_state(result.streak);
    read_errors_->publish_state(errors_.load());
    overflows_->publish_state(overflows_count_.load());
    report_drops_->publish_state(drops_.load());
    last_report_ = now;
  }
}
}
