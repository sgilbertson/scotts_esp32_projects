#!/usr/bin/env python3
"""Offline, windowed spectral analysis; requires NumPy, not device firmware.

RMS estimates use Hann-weighted periodograms, corrected for window energy.
The high-pass response is a digital second-order Butterworth at 2500 Hz.
This is a spectral estimate, not a sample-by-sample causal filter simulation.
Levels are relative to PCM full scale, not calibrated dB SPL.
"""
import argparse
import csv
import json
import wave
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav', type=Path)
    parser.add_argument('--output', type=Path, default=Path('analysis.csv'))
    parser.add_argument('--interval', type=float, default=0.5)
    parser.add_argument('--lookback', type=float, default=5.0)
    parser.add_argument('--threshold', type=float, default=12.0, help='Level rise in dB')
    parser.add_argument('--consecutive', type=int, default=5)
    args = parser.parse_args()
    if args.interval <= 0 or args.lookback < args.interval or args.consecutive < 1:
        parser.error('Require positive interval, lookback >= interval, consecutive >= 1')
    with wave.open(str(args.wav)) as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            parser.error('Requires mono signed 16-bit PCM WAV')
        rate = wav.getframerate()
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(np.float64) / 32768
    size = round(rate * args.interval)
    if size < 2 or rate <= 14000:
        parser.error('Require at least two samples per interval and sample rate > 14 kHz')
    window = np.hanning(size)
    freq = np.fft.rfftfreq(size, 1 / rate)
    k = np.tan(np.pi * 2500 / rate)
    norm = 1 / (1 + np.sqrt(2) * k + k * k)
    b = np.array([1, -2, 1]) * norm
    a = np.array([1, 2 * (k*k - 1) * norm, (1 - np.sqrt(2)*k + k*k) * norm])
    z = np.exp(-2j * np.pi * freq / rate)
    hp = abs((b[0] + b[1]*z + b[2]*z*z) / (a[0] + a[1]*z + a[2]*z*z))**2
    masks = {'hp2500_dbfs': hp, 'band3000_7000_dbfs': (freq >= 3000) & (freq < 7000)}
    for lo, hi in [(0, 1000), (1000, 2500), (2500, 3000), (3000, 4000),
                   (4000, 5000), (5000, 6000), (6000, 7000), (7000, 10000), (10000, rate/2+1)]:
        masks[f'band{lo}_{int(hi)}_dbfs'] = (freq >= lo) & (freq < hi)
    lag = round(args.lookback / args.interval)
    rows, runs = [], []
    streak = 0
    # Full windows only; keep time alignment explicit for replay comparisons.
    for start in range(0, len(audio) - size + 1, size):
        segment = audio[start:start+size]
        power = abs(np.fft.rfft(segment * window))**2 / (size * np.sum(window**2))
        power[1:-1 if size % 2 == 0 else None] *= 2
        row = {'start_s': start/rate, 'end_s': (start+size)/rate,
               'raw_dbfs': float(10*np.log10(max(np.mean(segment**2), 1e-16)))}
        for key, mask in masks.items():
            row[key] = float(10*np.log10(max(np.sum(power*mask), 1e-16)))
        delta = row['hp2500_dbfs'] - rows[-lag]['hp2500_dbfs'] if len(rows) >= lag else None
        row['rise_db'] = delta
        streak = streak + 1 if delta is not None and delta >= args.threshold else 0
        row['detected'] = streak >= args.consecutive
        if streak == args.consecutive:
            runs.append(row['end_s'])
        rows.append(row)
    with args.output.open('w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summary = {'sample_rate': rate, 'duration_s': len(audio)/rate,
               'interval_s': size/rate, 'lookback_intervals': lag,
               'threshold_db': args.threshold, 'consecutive': args.consecutive,
               'detection_times_s': runs, 'csv': str(args.output)}
    print(json.dumps(summary, indent=2))
    print('Buzzer-neighbourhood windows:')
    for row in rows:
        if 209 <= row['start_s'] <= 216:
            print({k: round(row[k], 2) if row[k] is not None else None
                   for k in ['start_s', 'raw_dbfs', 'hp2500_dbfs', 'band3000_7000_dbfs', 'rise_db', 'detected']})


if __name__ == '__main__':
    main()
