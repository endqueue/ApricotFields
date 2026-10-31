"""Writes the streamed-voice test files in this folder (apaudio_tests.c,
"Streamed sounds"; APAUDIO_TODO.md item 15).

Made 2026-10-31 with Python's soundfile 0.14.0 over libsndfile 1.2.2 (its
FLAC, Vorbis and Opus encoders) and numpy, from a venv outside the
repository:

    python -m venv venv
    venv/Scripts/python -m pip install soundfile numpy
    venv/Scripts/python tests/data/apaudio/make_stream_data.py

The signal is computed here, so the samples going in are the same on every
machine; the encoders' bytes are libsndfile's, which is why the files are
kept rather than remade by the build.

- stream.flac, stream.ogg, stream.opus: 2 s of stereo at 48 kHz, long enough
  to wrap a 0.1 s ring twenty times. Left, a 440 Hz tone whose level
  swells; right, 660 Hz and 1,320 Hz; both with a little seeded noise, so
  no block compresses to nothing.
- stream_loop.flac, stream_loop.ogg: 0.7 s of mono at 44.1 kHz, a 523.25 Hz
  tone with a seeded noise floor, for looping and for a second rate and
  channel count. stream_loop.opus: the same at 48 kHz (Opus decodes at
  48 kHz whatever it was made from).
- stream_corrupt.flac, .ogg, .opus: stream.* with 4,096 bytes at 60 % of the
  file overwritten by a fixed pattern, past any prefetch the tests make: a
  stream meets it mid-way.
"""
import os

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))


def noise(count, seed):
    # A 32-bit linear congruential generator, so the noise is the same
    # everywhere, scaled to [-1, 1).
    state = np.uint64(seed)
    out = np.empty(count, dtype=np.float64)
    for i in range(count):
        state = (state * np.uint64(1664525) + np.uint64(1013904223)) & np.uint64(0xFFFFFFFF)
        out[i] = float(state) / 2147483648.0 - 1.0
    return out


def stream_signal():
    rate = 48000
    t = np.arange(2 * rate) / rate
    swell = 0.25 + 0.2 * np.sin(2 * np.pi * 0.75 * t)
    left = swell * np.sin(2 * np.pi * 440.0 * t) + 0.01 * noise(len(t), 1)
    right = 0.3 * np.sin(2 * np.pi * 660.0 * t) + 0.1 * np.sin(2 * np.pi * 1320.0 * t) + 0.01 * noise(len(t), 2)
    return np.stack([left, right], axis=1), rate


def loop_signal(rate):
    count = int(round(0.7 * rate))
    t = np.arange(count) / rate
    return 0.4 * np.sin(2 * np.pi * 523.25 * t) + 0.02 * noise(count, 3), rate


def corrupt(name, out_name):
    with open(os.path.join(HERE, name), 'rb') as f:
        data = bytearray(f.read())
    at = len(data) * 6 // 10
    for i in range(4096):
        data[at + i] = (0x5A + i * 7) & 0xFF
    with open(os.path.join(HERE, out_name), 'wb') as f:
        f.write(data)


def main():
    stereo, rate = stream_signal()
    sf.write(os.path.join(HERE, 'stream.flac'), stereo, rate, format='FLAC', subtype='PCM_16')
    sf.write(os.path.join(HERE, 'stream.ogg'), stereo, rate, format='OGG', subtype='VORBIS')
    sf.write(os.path.join(HERE, 'stream.opus'), stereo, rate, format='OGG', subtype='OPUS')
    mono, rate = loop_signal(44100)
    sf.write(os.path.join(HERE, 'stream_loop.flac'), mono, rate, format='FLAC', subtype='PCM_16')
    sf.write(os.path.join(HERE, 'stream_loop.ogg'), mono, rate, format='OGG', subtype='VORBIS')
    mono, rate = loop_signal(48000)
    sf.write(os.path.join(HERE, 'stream_loop.opus'), mono, rate, format='OGG', subtype='OPUS')
    for extension in ('flac', 'ogg', 'opus'):
        corrupt('stream.' + extension, 'stream_corrupt.' + extension)


if __name__ == '__main__':
    main()
