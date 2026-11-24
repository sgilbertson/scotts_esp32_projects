#!/usr/bin/env python3
"""Receive ESP32 MS3625 UDP audio, save mono 16-bit PCM as a WAV file."""
import argparse
import socket
import struct
import time
import wave

HEADER = struct.Struct('<4sIHHI')
MAGIC = b'AUD0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=3333)
    parser.add_argument('--output', default='capture.wav')
    parser.add_argument('--seconds', type=float, default=30, help='Audio seconds to record (0 = until Ctrl-C)')
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    sock.bind(('0.0.0.0', args.port))
    sock.settimeout(2.0)
    print(f'Listening on UDP port {args.port}. Start/reset the ESP32.', flush=True)

    sample_rate = None
    expected = None
    first_sequence = None
    last_sequence = None
    packet_count = 0
    lost_packets = 0
    out_of_order = 0
    recorded = 0
    wav = None
    start = None
    last_frames = None
    try:
        while True:
            try:
                data, address = sock.recvfrom(4096)
            except socket.timeout:
                if start is not None:
                    print('No packets received for 2 seconds; still listening...', flush=True)
                continue
            if len(data) < HEADER.size:
                continue
            magic, seq, frames, flags, rate = HEADER.unpack_from(data)
            if magic != MAGIC or rate == 0 or not (1 <= frames <= 512):
                continue
            if len(data) != HEADER.size + frames * 2:
                print('Invalid payload length; ignoring packet')
                continue
            if wav is None:
                sample_rate = rate
                wav = wave.open(args.output, 'wb')
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(sample_rate)
                start = time.monotonic()
                first_sequence = seq
                print(f'First packet from {address}, seq={first_sequence}, {rate} Hz, '
                      f'writing {args.output}', flush=True)
            if rate != sample_rate:
                print('Unexpected sample-rate change; ignoring packet')
                continue

            if expected is not None:
                delta = (seq - expected) & 0xFFFFFFFF
                if delta == 0:
                    pass
                elif delta < 0x80000000:
                    lost_packets += delta
                    # Missing samples are replaced by silence to preserve timing.
                    # Use last normal frame count (512 unless the firmware changes).
                    missing_frames = last_frames or frames
                    gap_start = recorded / sample_rate
                    gap_end = (recorded + missing_frames * delta) / sample_rate
                    first_missing = expected
                    last_missing = (seq - 1) & 0xFFFFFFFF
                    wav.writeframesraw(b'\x00\x00' * missing_frames * delta)
                    recorded += missing_frames * delta
                    print(f'Missing {delta} packet(s), seq={first_missing}..{last_missing}, '
                          f'before seq={seq}; audio={gap_start:.3f}..{gap_end:.3f}s '
                          f'from first_seq={first_sequence}', flush=True)
                else:
                    out_of_order += 1
                    continue
            expected = (seq + 1) & 0xFFFFFFFF
            last_sequence = seq
            last_frames = frames
            wav.writeframesraw(data[HEADER.size:])
            recorded += frames
            packet_count += 1
            if args.seconds > 0 and recorded / sample_rate >= args.seconds:
                break
    except KeyboardInterrupt:
        print('\nInterrupted.')
    finally:
        if wav:
            wav.close()
        sock.close()
        print(f'Packets={packet_count}, missing={lost_packets}, '
              f'out-of-order={out_of_order}, audio={recorded / sample_rate if sample_rate else 0:.2f}s, '
              f'first_seq={first_sequence}, last_seq={last_sequence}')


if __name__ == '__main__':
    main()
