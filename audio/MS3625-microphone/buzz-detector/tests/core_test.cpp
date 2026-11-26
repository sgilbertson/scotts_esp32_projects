#include "../components/buzz_detector/detector_core.h"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

static buzz::Result tone(buzz::Detector &d, float hz, float amplitude, unsigned count=16000) {
  buzz::Result result;
  for (unsigned i=0; i<count; ++i) {
    const int16_t pcm = std::lround(32767*amplitude*std::sin(2*3.141592653589793*hz*i/32000));
    d.sample(pcm, result);
  }
  return result;
}
int main(int argc, char **argv) {
  buzz::Detector detector;
  buzz::Settings settings;
  detector.configure(settings);
  if (argc>1 && std::string(argv[1])=="--replay") {
    uint64_t count=0;
    uint8_t bytes[2];
    bool previous=false;
    while (std::cin.read(reinterpret_cast<char *>(bytes),2)) {
      buzz::Result result;
      ++count;
      if (detector.sample(static_cast<int16_t>(bytes[0] | (uint16_t(bytes[1])<<8)),result)) {
        if (result.detected!=previous) std::cout << (result.detected?"ON ":"OFF ") << count/32000.0 << '\n';
        previous=result.detected;
      }
    }
    return 0;
  }
  for (unsigned i=0;i<10;++i) assert(!tone(detector,5000,.0001).ready);
  auto background=tone(detector,5000,.0001);
  assert(background.ready && !background.detected);
  // One loud window must not trigger. It must also break consecutive evidence.
  assert(!tone(detector,5000,.1).detected);
  assert(!tone(detector,5000,.0001).detected);
  for (unsigned i=0;i<4;++i) assert(!tone(detector,5000,.1).detected);
  assert(tone(detector,5000,.1).detected);
  assert(!tone(detector,5000,.0001).detected);
  // Reconfiguration and capture discontinuities discard the historical baseline.
  detector.reset();
  assert(!tone(detector,5000,.1).ready);
  settings.minimum_dbfs=-20; detector.configure(settings);
  for(unsigned i=0;i<11;++i) tone(detector,5000,.0001);
  for(unsigned i=0;i<7;++i) assert(!tone(detector,5000,.01).detected);
  // The causal filter strongly rejects machine-like low-frequency energy.
  detector.configure(buzz::Settings{});
  tone(detector,100,.1);
  float low=tone(detector,100,.1).level;
  tone(detector,5000,.1);
  float high=tone(detector,5000,.1).level;
  assert(high-low>40);
  // Cooldown prevents immediate repeat alerts; quiet release still occurs.
  settings=buzz::Settings{}; settings.consecutive=1; settings.cooldown_s=10;
  detector.configure(settings);
  for(unsigned i=0;i<11;++i) tone(detector,5000,.0001);
  assert(tone(detector,5000,.1).detected);
  assert(!tone(detector,5000,.0001).detected);
  assert(!tone(detector,5000,.1).detected);
  puts("PASS: baseline warmup, persistence, transient rejection, release, minimum level, reset, frequency response, cooldown");
}
