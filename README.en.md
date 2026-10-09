[Deutsch](README.md) · **English**

# HD Radio Version of AMWaveSynth (Wave Synthesizer for Longwave and Mediumwave)

Modified version of `am_modulator_5MSPS_integer.c` from https://github.com/radiolab81/AMWaveSynth, but with a digital HD Radio signal
instead of plain AM. The RF output is delivered via TCP port 12345 to `fl2k_tcp`, smiSDR/parlioSDR/similar SDR transmitters (or to a file).

## Which Modes Exist on Mediumwave?

NRSC-5-D (1012s, Layer 1 AM) defines exactly two MW service modes, and both are implemented:

| Mode | Meaning | Options |
|---|---|---|
| **MA1** | Hybrid: analog AM plus digital sidebands | `aab` (analog audio bandwidth 8 instead of 5 kHz), `pl` (higher secondary/tertiary power), `hpp` (higher PIDS power), `rdb` (reduced digital bandwidth) |
| **MA3** | All-digital (unmodulated carrier plus OFDM) | `hpp`, `rdb` |

![ma1](/images/MA1.png)*MA1 hybrid mode*

![ma3](/images/MA3.png)*MA3 all-digital mode*


## Signal Chain

```
Audio (UDP, s16le mono 44.1 kHz)
 ├─ digital: HDC encoder (fdk-aac, HDC patch) → L2 (PDU, RS, PSD) → L1 (scrambler, convolutional code,
 │           interleaver, diversity delay, mapping) → IFFT 4096 (16x oversampled)
 │           → pulse shaping → 744.1875 kHz baseband (I/Q)
 └─ analog (MA1 only): LPF 4.5/7.5 kHz → AGC → 5.5 s delay → polyphase interpolator
                       (135/8) → 1 + m(t) on the I axis of the same baseband
Baseband → cubic Lagrange/Farrow interpolation → 5 MSPS → NCO mixer → sum of all transmitters
         → sferics (optional) → limiter → int8/int16 → TCP/file
```

The following parts use integer arithmetic: L1/L2, IFFT (Q22, twiddles Q30), pulse shaping (Q15), analog FIR, Farrow interpolator
(int64 Horner), NCO with a 32-bit phase accumulator and a 14-bit sine LUT, mixer, scaling, and limiting.
Floating point is used only at startup (tables, filter design) and for the slow AGC every 10 ms. This matches
the approach of the original program.

## Building

```
./build.sh            # downloads and builds the HDC-capable fdk-aac, then the modulator (real audio)
./build.sh nohdc      # without codec: dummy audio frames, for signal tests only
```

Without `USE_FDK_HDC`, L1/L2/SIS/PSD run completely, but the audio channel carries random data.

## Usage

```
./hdradio_am [options] <port:frequency[:mode]> ...
./hdradio_am -b 8 -s 10 1234:603000:ma1,aab 1235:1017000:ma3,hpp
sudo fl2k_tcp -a 127.0.0.1 -p 12345 -s 10000000
```
Feed in audio (one UDP port per transmitter, 16-bit, mono, 44100 Hz, little endian):

```
ffmpeg -re -i music.mp3 -f s16le -ar 44100 -ac 1 udp://127.0.0.1:1234
```

![rx1](/images/rx1.jpg)
![rx2](/images/rx2.jpg)

A test transmission against the "offline" tool AMWaveSynthFFT is also possible:

```
./hdradio_am -b 12 1234:603000:ma1,aab 1235:1017000:ma3,hpp
python3 AMWaveSynthFFT.py
```

![fftpy](/images/AMWaveSynthFFT.png)

Important options: `-b` bit depth 8..16, `-s` 5 (native) or 10 (every sample doubled, as in the original),
`-o file -n sec` output to a file, `-i file` audio file for transmitters with port 0 (port 0 without `-i` = test tone),
`-m` analog modulation depth, `-G` fixed analog gain instead of AGC, `-D` analog delay (default 5.5 s),
`-L` carrier level relative to full scale, `-N/-S/-M/-C/-T/-A/-P` station name, slogan, message,
country, title, artist, program type.

Control via UDP as in the original: port 8888 `freq:gain` (fading), plus `title=…`, `artist=…`,
`message=…` (scrolling text); port 8889 `amp:ms` (sferics).

Startup takes about 1.5 to 3 s because of audio prebuffering until real audio is transmitted
(one L1 frame lasts 1.486 s). Initially, the signal consists of a carrier plus digital silence.

## Python Tk UI

The "UI" directory also contains a multilingual Python Tk user interface for controlling the entire
transmission process.

![ui](/UI/images/mainui.png)

## Verification

The open-source decoder **nrsc5** (with `--am`) served as the counterpart, preceded by a receive chain
(`tools/rf_to_nrsc5.py`: mixing, decimation, resampling to 46511.71875 Hz).

| Test | Result |
|---|---|
| Reference file from https://www.sigidwiki.com/wiki/HD_Radio_(AM) | MA1, AAB=8 kHz, PL low, HPP off, full digital bandwidth |
| MA1, MA1+aab, +pl, +hpp, +rdb (12 bit) | nrsc5 synchronizes, reports the flags correctly, decodes SIS (name, country, slogan, location, time), PSD (title/artist), audio service |
| MA1 with real HDC | Decoded audio correlates at 0.93 with the original (lossy codec), BER in the decoder approx. 0.003, independent of DAC depth (8 to 14 bit) |
| Analog path | Envelope demodulation correlates at 0.988 with the original, delay exactly 5.5 s |
| Spectrum (12 bit) | −58 dBc at ±16 kHz, −93 dBc at ±18 kHz, beyond that only the quantization floor (−102 dBc/300 Hz), no images at 46.5 kHz |
| Real time (UDP audio, TCP, throttled to 5 MB/s) | 2 transmitters (MA1 plus MA3) without underruns |
| CPU load | 3 transmitters: 20 s of signal in 3.5 s on one core (about 5.7x real time). On a Core2Duo, 2 transmitters (MA1 plus MA3) require 30 to 40% CPU (user report) |
| **Hardware receivers Sangean HDR-1, HDR-16, HDR-18** | **MA1 (603 kHz, AAB) and MA3 (1017 kHz, HPP) simultaneously, both channels decoded without errors, display shows HDAM / HD AM with station and PSD text, continuous operation over several hours** |

## Important Limitations

* **MA3 and nrsc5:** The open-source decoder nrsc5 does not synchronize to any MA3 signal, not even to one
  generated purely synthetically in Python. The cause therefore lies with the decoder, not the modulator: a real
  hardware receiver (Sangean HDR-1, HDR-16, HDR-18) decodes MA3 without errors (see table). For testing MA3,
  use a hardware receiver, not nrsc5.
* Not implemented: AAS/LOT data services (album art, SIG) and emergency alerts (EA). The P3 data channel
  is transmitted with filler bytes, only HD1 (program 0) is transmitted, and audio is mono.
* The 5.5 s analog delay comes from the gr-nrsc5 flowgraph. The standard specifies 4.5 frames
  (6.69 s) for blending in the receiver. It can be set with `-D 6.687`.
* The carrier levels (MA1 0.33, MA3 0.09 of full scale) are chosen so that the peaks of the
  OFDM signal are not clipped. With multiple transmitters, the level is divided by their number.
* The sferics amplitude is not directly comparable 1:1 with the original because of the different level reference. For sferics simulation,
  the sferics generators from the original AMWaveSynth project and the propagation simulator AMWaveSynthPropagationSimulator https://github.com/radiolab81/AMWaveSynthPropagationSimulator
  can be used!
* The location in the SIS (Erfurt), CET/EU daylight saving time, and the country `DE` are defaults in the code
  (`sis_init`).

## License

The protocol parts (L1, L2, SIS, PSD) are ported from **gr-nrsc5** (Clayton Smith, GPL-3.0). The
source code is therefore also licensed under GPL-3.0. The HDC encoder is the patched fdk-aac
(`argilo/fdk-aac`, branch `hdc-encoder`) and is subject to its own license.
