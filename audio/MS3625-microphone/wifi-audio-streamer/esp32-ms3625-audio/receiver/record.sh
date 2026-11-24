#!/bin/bash

# Capture an audio recording from the ESP32 over WiFi and save it as a WAV audio file.
# Usage: record.sh <output_file> <max_seconds>

OFILE=${1:-record}
MAX_SECONDS=${2:-7200}

echo "Recording to file $OFILE.wav for a maximum of $MAX_SECONDS seconds..."
echo "(You can pPress control-C to end the recording early)"

python3 receive.py --output $OFILE.wav --seconds $MAX_SECONDS 2>&1 | tee $OFILE.log
