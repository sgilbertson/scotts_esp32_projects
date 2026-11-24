#!/bin/bash

# Monitor statistics from the ESP32, printing to the console and writing to a log file.
# Works only iIf you built the ESP32 project with this line in config.h:
# #define STATS_PORT 3334

trap 'printf "\nResults are in esp32-stats.log\n"; exit 130' INT

echo "Monitoring ESP32 audio streamer stats on port 3334..."
nc -u -l -k 3334 | tee esp32-stats.log
