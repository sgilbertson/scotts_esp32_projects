#include "buzz_detector.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_debug_helpers.h>
#include <esp_rom_sys.h>
#include <esp_attr.h>
#include <cstring>

namespace esphome::buzz_detector {
static const char *const TAG = "buzz_detector";
namespace {
// Retain the restarting task across a software reset even if USB output is lost.
// A one-shot magic value and reset-reason check prevent stale power-up reports.
struct RestartBreadcrumb { uint32_t magic; char task[configMAX_TASK_NAME_LEN]; };
RTC_NOINIT_ATTR RestartBreadcrumb restart_breadcrumb;
constexpr uint32_t RESTART_MAGIC = 0x42555A5A;
void trace_software_restart() {
  const char *task = pcTaskGetName(nullptr);
  std::strncpy(restart_breadcrumb.task, task ? task : "unknown",
               sizeof(restart_breadcrumb.task) - 1);
  restart_breadcrumb.task[sizeof(restart_breadcrumb.task) - 1] = 0;
  restart_breadcrumb.magic = RESTART_MAGIC;
  // Shutdown callbacks run before the scheduler is suspended. Use the ROM
  // console and IDF unwinder rather than the buffered ESPHome task logger.
  esp_rom_printf("BUZZ: esp_restart requested from task %s; caller backtrace follows\n",
                 restart_breadcrumb.task);
  esp_backtrace_print(16);
}
}  // namespace
void BuzzDetector::setup() {
  setup_ms_ = millis();
  if (esp_reset_reason() == ESP_RST_SW && restart_breadcrumb.magic == RESTART_MAGIC) {
    restart_breadcrumb.task[sizeof(restart_breadcrumb.task) - 1] = 0;
    ESP_LOGW(TAG, "Previous software restart requested from task: %s", restart_breadcrumb.task);
  }
  restart_breadcrumb.magic = 0;
  const esp_err_t trace_status = esp_register_shutdown_handler(trace_software_restart);
  if (trace_status != ESP_OK) ESP_LOGW(TAG, "Cannot register restart trace: %d", trace_status);
  ESP_LOGI(TAG, "Boot reset_reason=%d (3=software, 4=panic, 6=task WDT), capture=%s",
           static_cast<int>(esp_reset_reason()), capture_enabled_ ? "enabled" : "disabled");
  // Allocate once. Only small interval results cross to the ESPHome/Zigbee loop.
  settings_queue_ = xQueueCreate(1, sizeof(buzz::Settings));
  result_queue_ = xQueueCreate(16, sizeof(buzz::Result));
  if (!settings_queue_ || !result_queue_) { mark_failed(); return; }
  detected_->publish_state(false);
  ready_->publish_state(false);
  // Start later from loop(): setup, Zigbee startup and config logging finish first.
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
    const int64_t processing_start = esp_timer_get_time();
    uint32_t raw_peak[2]{}, pcm_peak[2]{}, nonzero[2]{};
    for (unsigned i = 0; i < 256; ++i) {
      // Inspect both slots without changing which one feeds the detector.
      for (unsigned slot = 0; slot < 2; ++slot) {
        const int32_t raw = samples[2*i + slot];
        const uint32_t magnitude = raw < 0 ? -static_cast<int64_t>(raw) : raw;
        const int32_t pcm = static_cast<int16_t>(raw >> 16);
        raw_peak[slot] = std::max(raw_peak[slot], magnitude);
        pcm_peak[slot] = std::max(pcm_peak[slot], static_cast<uint32_t>(pcm < 0 ? -pcm : pcm));
        nonzero[slot] += raw != 0;
      }
      buzz::Result result;
      if (detector.sample(static_cast<int16_t>(samples[2*i + slot_] >> 16), result)) submit(result);
    }
    // One atomic aggregation per block, not per sample. Main loop exchanges
    // peaks/counts at reporting time; CAS avoids losing a concurrent new peak.
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto update_peak = [](std::atomic<uint32_t> &peak, uint32_t value) {
        uint32_t old = peak.load(std::memory_order_relaxed);
        while (old < value && !peak.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
      };
      update_peak(raw_peak_[slot], raw_peak[slot]);
      update_peak(pcm_peak_[slot], pcm_peak[slot]);
      nonzero_[slot].fetch_add(nonzero[slot], std::memory_order_relaxed);
    }
    const uint32_t elapsed = esp_timer_get_time() - processing_start;
    if (elapsed > max_dsp_us_.load()) max_dsp_us_.store(elapsed);
    blocks_.fetch_add(1);
    // The H2 has one core. Even a DMA backlog must not monopolize it: allow
    // Zigbee, the ESPHome loop and idle to run once per 8 ms audio block.
    vTaskDelay(1);
  }
}
void BuzzDetector::loop() {
  if (fatal_.load()) {
    ESP_LOGE(TAG, "I2S setup failed: %d", fatal_.load());
    detected_->publish_state(false); ready_->publish_state(false);
    mark_failed(); return;
  }
  if (is_failed()) return;
  const uint32_t uptime = millis();
  if (capture_enabled_ && !task_started_ && uptime - setup_ms_ >= start_delay_) {
    ESP_LOGI(TAG, "Starting capture: priority=2 stack=8192, yielding every 256 frames");
    if (xTaskCreate(task_entry, "buzz-capture", 8192, this, 2, &capture_task_) != pdPASS) {
      ESP_LOGE(TAG, "Cannot create capture task");
      mark_failed(); return;
    }
    task_started_ = true;
  }
  if (uptime - heartbeat_ms_ >= 5000) {
    ESP_LOGI(TAG, "alive uptime_ms=%lu blocks=%lu read_errors=%lu overflows=%lu result_drops=%lu "
             "dsp_us_max=%lu stack_free=%u heap_free=%u",
             static_cast<unsigned long>(uptime), static_cast<unsigned long>(blocks_.load()),
             static_cast<unsigned long>(errors_.load()), static_cast<unsigned long>(overflows_count_.load()),
             static_cast<unsigned long>(drops_.load()), static_cast<unsigned long>(max_dsp_us_.load()),
             capture_task_ ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(capture_task_)) : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    ESP_LOGI(TAG, "audio slot0_raw_peak=0x%08lX slot0_pcm_peak=%lu slot0_nonzero=%lu "
             "slot1_raw_peak=0x%08lX slot1_pcm_peak=%lu slot1_nonzero=%lu "
             "selected=%u level_dbfs=%.1f baseline_dbfs=%.1f rise_db=%.1f ready=%u",
             static_cast<unsigned long>(raw_peak_[0].exchange(0)),
             static_cast<unsigned long>(pcm_peak_[0].exchange(0)),
             static_cast<unsigned long>(nonzero_[0].exchange(0)),
             static_cast<unsigned long>(raw_peak_[1].exchange(0)),
             static_cast<unsigned long>(pcm_peak_[1].exchange(0)),
             static_cast<unsigned long>(nonzero_[1].exchange(0)), slot_,
             latest_result_.level, latest_result_.baseline, latest_result_.rise,
             latest_result_.ready);
    heartbeat_ms_ = uptime;
  }
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
    latest_result_ = result;
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
