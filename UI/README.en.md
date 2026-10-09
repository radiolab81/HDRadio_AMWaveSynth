# HD Radio Modulator GUI (hdtxgui.py)

Python 3/Tk interface for `hdradio_am`, successor to `amtxgui.py`.

## Files (place all in one folder, together with the modulator program `hdradio_am`)

| File | Purpose |
|---|---|
| `hdtxgui.py` | the GUI (`python3 hdtxgui.py`) |
| `start_sender_hd.sh` | starts one ffmpeg instance per transmitter plus the modulator, and shuts everything down cleanly on stop |
| `hdradio_am` | the modulator (path can be changed under *Modulator → Settings*) |
| `start_fl2k.sh` | the same startup script for `fl2k_tcp` as before; it is launched via `sudo` in its own xterm. Comment it out for smiSDR/parlioSDR/other SDR TXs |
| `stations.db` | Internet radio database, lines of `Name,URL` (same format as before, example: `stations_example.db`) |
| `hdtx_settings.json` | created by the GUI (settings, language) |

Requirements: `python3-tk`, `ffmpeg`, `xterm`; `socat` only for the SDR bridge. The old `stop_sender.sh` is no longer needed.

## Creating Transmitters (Transmitter → Add, double-click = Edit)

* **Frequency plan / frequency** as before (Europe 9 kHz, USA 10 kHz, CH/IT special cases, manual entry). 100 to 2400 kHz is allowed.
* **Mode:** MA1 (hybrid, analog AM plus digital) or MA3 (all-digital).
* **Options:** AAB (analog audio 8 instead of 5 kHz), PL (higher secondary/tertiary power), HPP (higher PIDS power), RDB (reduced digital bandwidth). With MA3, AAB and PL cannot be selected because they have no effect there.
* **Select program:** choosing an entry from `stations.db` fills in the URL automatically.
* **URL / path:** can be pasted freely; anything ffmpeg can read (stream URL, local file, playlist). *File ...* opens a file dialog.
* There is **no audio bandwidth** setting anymore: ffmpeg always delivers the full band (s16le, mono, 44.1 kHz) with `volume=0.8` and the compressor from the old script. The modulator itself handles the limiting to 5/8 kHz for the analog path.
* The approximate occupied bandwidth is shown below the options. If the digital signal overlaps with another transmitter (MA1 approx. ±14.8 kHz, MA3 approx. ±9.6 kHz, MA3+RDB approx. ±5 kHz), a warning appears, which you can confirm. Maximum of 8 transmitters.

![dlg](/UI/images/stationsdlg.png)

![inetradio](/UI/images/internetradios.png)

## CSV Transmitter Landscapes (File → Load / Save)

New format, delimiter `;`: `Frequency;Mode;Options;Program name;URL / path`, example `Beispiel_Senderlandschaft_HD.csv`.
Old files from `amtxgui.py` (`Frequency;Bandwidth;Program name;URL`) are recognized and loaded as MA1 without options. The bandwidth is dropped.
The header row may be in any language.

![mainui](/UI/images/mainui.png)

## Start / Stop and Indicators

*START ALL transmitters* opens an xterm running `start_sender_hd.sh` (UDP audio ports starting at 1234, in table order) and, after 3 s, optionally starts your `start_fl2k.sh`. *STOP* closes the xterm, whereupon the script terminates ffmpeg, the modulator, and socat. Two indicator lights: modulator running (`hdradio_am`) and SDR connection (`fl2k_tcp` or `socat`).

## Live Control (while the modulator is running, UDP 8888/8889)

* Change title/artist (PSD) and message (SIS) (RDS-like)
* Level of the selected transmitter via slider (fading, 0 to 1.5) -> for automatic fading control of the modulator, see the AMWaveSynthPropagationSimulator project https://github.com/radiolab81/AMWaveSynthPropagationSimulator
* Sferics/lightning with strength and duration; this is only a single trigger of one lightning discharge (test case). For automatically moving thunderstorm fronts, see the AMWaveSynthPropagationSimulator project https://github.com/radiolab81/AMWaveSynthPropagationSimulator

## Modulator → Settings

DAC bit depth, sample rate (5 or 10 MSPS, 10 = every sample output twice as before), station name, slogan, message, country, program type, analog modulation depth and delay, carrier level, modulator path, SDR bridge (IP, port, remote fl2k), and the fl2k command.

![settings](/UI/images/modulatorsettings.png)

## Limitations

* **Station name, slogan, message, country, and program type apply to all transmitters collectively**, because the modulator accepts them only once. Individual names per transmitter would require a small change in the modulator.
* The **carrier level** (leave empty = default MA1 0.33, MA3 0.09) applies to all transmitters. High values overdrive MA3 in particular.
* The *Program name* column is for display and database selection only; it is not transmitted.
