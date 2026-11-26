#pragma once
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "detector_core.h"
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>

namespace esphome::buzz_detector {
class BuzzDetector : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_pins(int b, int w, int d) { bclk_ = b; ws_ = w; data_ = d; }
  void set_slot(unsigned slot) { slot_ = slot; }
  void set_report_interval(uint32_t ms) { report_ms_ = ms; }
  void set_control(unsigned i, number::Number *n) { controls_[i] = n; }
#define BUZZ_SENSOR(name) void set_##name(sensor::Sensor *s) { name##_ = s; } sensor::Sensor *name##_ = nullptr;
  BUZZ_SENSOR(level) BUZZ_SENSOR(baseline) BUZZ_SENSOR(rise) BUZZ_SENSOR(streak)
  BUZZ_SENSOR(read_errors) BUZZ_SENSOR(overflows) BUZZ_SENSOR(report_drops)
#undef BUZZ_SENSOR
  void set_detected(binary_sensor::BinarySensor *s) { detected_ = s; }
  void set_ready(binary_sensor::BinarySensor *s) { ready_ = s; }
 protected:
  static void task_entry(void *arg) { static_cast<BuzzDetector *>(arg)->capture(); }
  void capture();
  void submit(const buzz::Result &r);
  static bool IRAM_ATTR overflow_cb(i2s_chan_handle_t, i2s_event_data_t *, void *arg) {
    static_cast<BuzzDetector *>(arg)->overflows_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  number::Number *controls_[8]{};
  binary_sensor::BinarySensor *detected_ = nullptr, *ready_ = nullptr;
  int bclk_ = 4, ws_ = 5, data_ = 12;
  unsigned slot_ = 1;
  uint32_t report_ms_ = 2000, last_report_ = 0;
  buzz::Settings configured_;
  QueueHandle_t settings_queue_ = nullptr, result_queue_ = nullptr;
  std::atomic<uint32_t> errors_{0}, overflows_count_{0}, drops_{0};
  std::atomic<int> fatal_{0};
  uint32_t handled_drops_ = 0;
};
}
