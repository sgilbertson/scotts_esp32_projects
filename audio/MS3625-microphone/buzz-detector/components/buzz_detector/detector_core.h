#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// No ESPHome or RTOS dependencies: replay tests use this exact causal DSP.
namespace buzz {
constexpr unsigned SAMPLE_RATE = 32000;
struct Settings {
  unsigned interval_ms = 500;
  float lookback_s = 5, threshold_db = 12, minimum_dbfs = -60, highpass_hz = 2500;
  unsigned consecutive = 5, release_intervals = 1;
  float cooldown_s = 10;
  bool operator==(const Settings &o) const {
    return interval_ms == o.interval_ms && lookback_s == o.lookback_s &&
      threshold_db == o.threshold_db && minimum_dbfs == o.minimum_dbfs &&
      highpass_hz == o.highpass_hz && consecutive == o.consecutive &&
      release_intervals == o.release_intervals && cooldown_s == o.cooldown_s;
  }
};
struct Result {
  float level = -160, baseline = NAN, rise = NAN;
  unsigned streak = 0;
  bool ready = false, detected = false;
};
class Detector {
 public:
  void configure(const Settings &s) {
    settings_ = s;
    // History covers 30 seconds at the minimum 100 ms interval.
    lag_ = std::clamp<unsigned>(std::lround(s.lookback_s * 1000 / s.interval_ms), 1, 300);
    window_samples_ = SAMPLE_RATE * s.interval_ms / 1000;
    const float k = std::tan(3.14159265358979323846f * s.highpass_hz / SAMPLE_RATE);
    const float norm = 1 / (1 + std::sqrt(2.f) * k + k*k);
    b0_ = norm; b1_ = -2*norm; b2_ = norm;
    a1_ = 2*(k*k-1)*norm; a2_ = (1-std::sqrt(2.f)*k+k*k)*norm;
    reset();
  }
  void reset() {
    z1_ = z2_ = sum_ = 0;
    samples_ = position_ = history_count_ = streak_ = quiet_ = cooldown_ = 0;
    active_ = false;
  }
  bool sample(int16_t pcm, Result &out) {
    float y = pcm / 32768.f;
    if (settings_.highpass_hz > 0) {
      const float x = y;
      y = b0_*x + z1_;
      z1_ = b1_*x - a1_*y + z2_;
      z2_ = b2_*x - a2_*y;
    }
    sum_ += y*y;
    if (++samples_ < window_samples_) return false;
    const float level = 10*std::log10(std::max(sum_ / samples_, 1e-16f));
    samples_ = 0; sum_ = 0;
    out = Result{};
    out.level = level;
    out.ready = history_count_ >= lag_;
    if (out.ready) {
      out.baseline = history_[(position_ + history_.size() - lag_) % history_.size()];
      out.rise = level - out.baseline;
    }
    history_[position_] = level;
    position_ = (position_ + 1) % history_.size();
    if (history_count_ < history_.size()) ++history_count_;
    const bool candidate = out.ready && level >= settings_.minimum_dbfs &&
                           out.rise >= settings_.threshold_db;
    if (cooldown_) --cooldown_;
    if (candidate) {
      quiet_ = 0;
      if (streak_ < settings_.consecutive) ++streak_;
      if (!active_ && !cooldown_ && streak_ >= settings_.consecutive) active_ = true;
    } else {
      streak_ = 0;
      if (active_ && ++quiet_ >= settings_.release_intervals) {
        active_ = false; quiet_ = 0;
        cooldown_ = std::ceil(settings_.cooldown_s * 1000 / settings_.interval_ms);
      }
    }
    out.streak = streak_;
    out.detected = active_;
    return true;
  }
 private:
  Settings settings_;
  std::array<float, 301> history_{};
  unsigned lag_ = 10, window_samples_ = 16000, samples_ = 0;
  unsigned position_ = 0, history_count_ = 0, streak_ = 0, quiet_ = 0, cooldown_ = 0;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0, sum_ = 0;
  bool active_ = false;
};
}  // namespace buzz
