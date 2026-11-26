#!/usr/bin/env python3
"""Compile and exercise the exact firmware DSP on the host; optionally replay a WAV."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import wave

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wav', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    exe = str(Path(directory) / 'core_test')
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'core_test.cpp'), '-o', exe], check=True)
    subprocess.run([exe], check=True)
    if args.wav:
        with wave.open(str(args.wav)) as wav:
            assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 32000)
            pcm = wav.readframes(wav.getnframes())
        replay = subprocess.run([exe, '--replay'], input=pcm, capture_output=True, check=True)
        print(replay.stdout.decode(), end='')
        # This assertion is specific to the supplied spin-cycle/buzzer fixture.
        assert replay.stdout.decode().splitlines() == ['ON 213.5', 'OFF 215'], replay.stdout
        print('PASS: full WAV replay produces exactly one alert at 213.5 s')
