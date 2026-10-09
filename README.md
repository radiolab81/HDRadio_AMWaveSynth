
**Deutsch** · [English](README.en.md)

# HD Radio Version von AMWaveSynth (Wave Synthesizer für Lang- und Mittelwelle)

Modifizierte Version zu `am_modulator_5MSPS_integer.c` aus https://github.com/radiolab81/AMWaveSynth, aber mit digitalem HD-Radio-Signal
statt normalem AM. Ausgabe der HF per über TCP-Port 12345 an `fl2k_tcp`, smiSDR/parlioSDR/ähnliche SDR Sender (oder in eine Datei).

## Welche Modi gibt es auf der Mittelwelle?

NRSC-5-D (1012s, Layer 1 AM) kennt genau zwei MW-Servicemodi, beide sind implementiert:

| Modus | Bedeutung | Optionen |
|---|---|---|
| **MA1** | Hybrid: analoges AM plus digitale Seitenbänder | `aab` (Analog-Audio 8 statt 5 kHz), `pl` (Sekundär/Tertiär-Leistung hoch), `hpp` (PIDS-Leistung hoch), `rdb` (reduzierte Digitalbandbreite) |
| **MA3** | All-Digital (unmodulierter Träger plus OFDM) | `hpp`, `rdb` |

![ma1](/images/MA1.png)*MA1 hybrid mode*

![ma3](/images/MA3.png)*MA3 all digital mode*


## Signalkette

```
Audio (UDP, s16le mono 44,1 kHz)
 ├─ digital: HDC-Encoder (fdk-aac, HDC-Patch) → L2 (PDU, RS, PSD) → L1 (Scrambler, Faltungscode,
 │           Interleaver, Diversity-Delay, Mapping) → IFFT 4096 (16-fach überabgetastet)
 │           → Pulsformung → 744,1875 kHz Basisband (I/Q)
 └─ analog (nur MA1): LPF 4,5/7,5 kHz → AGC → 5,5 s Verzögerung → Polyphasen-Interpolator
                       (135/8) → 1 + m(t) auf der I-Achse des gleichen Basisbands
Basisband → kubische Lagrange/Farrow-Interpolation → 5 MSPS → NCO-Mischer → Summe aller Sender
         → Sferics (optional) → Begrenzer → int8/int16 → TCP/Datei
```

Ganzzahlig sind: L1/L2, IFFT (Q22, Twiddles Q30), Pulsformung (Q15), Analog-FIR, Farrow-Interpolator
(int64-Horner), NCO mit 32-Bit-Phasenakkumulator und 14-Bit-Sinus-LUT, Mischer, Skalierung und Begrenzung.
Float wird nur beim Start (Tabellen, Filterentwurf) und für die langsame AGC je 10 ms benutzt. Das entspricht
dem Vorgehen im Originalprogramm.

## Bauen

```
./build.sh            # lädt und baut das HDC-fähige fdk-aac, dann der Modulator (echtes Audio)
./build.sh nohdc      # ohne Codec: Dummy-Audioframes, nur für Signaltests
```

Ohne `USE_FDK_HDC` laufen L1/L2/SIS/PSD vollständig, aber im Audiokanal stehen Zufallsdaten.

## Benutzung

```
./hdradio_am [Optionen] <Port:Frequenz[:Modus]> ...
./hdradio_am -b 8 -s 10 1234:603000:ma1,aab 1235:1017000:ma3,hpp
sudo fl2k_tcp -a 127.0.0.1 -p 12345 -s 10000000
```
Audio zuführen (je Sender ein UDP-Port, 16 Bit, mono, 44100 Hz, little endian):

```
ffmpeg -re -i musik.mp3 -f s16le -ar 44100 -ac 1 udp://127.0.0.1:1234
```

![rx1](/images/rx1.jpg)
![rx2](/images/rx2.jpg)

Eine Testaussendung gegen das "offline"-Tool AMWaveSynthFFT ist ebenso möglich:

```
./hdradio_am -b 12 1234:603000:ma1,aab 1235:1017000:ma3,hpp
python3 AMWaveSynthFFT.py
```

![fftpy](/images/AMWaveSynthFFT.png)

Wichtige Optionen: `-b` Bittiefe 8..16, `-s` 5 (nativ) oder 10 (jedes Sample doppelt, wie das Original),
`-o datei -n sek` Ausgabe in Datei, `-i datei` Audio-Datei für Sender mit Port 0 (Port 0 ohne `-i` = Testton),
`-m` Analog-Modulationsgrad, `-G` feste Analog-Verstärkung statt AGC, `-D` Analog-Verzögerung (Standard 5,5 s),
`-L` Trägerpegel relativ zum Vollausschlag, `-N/-S/-M/-C/-T/-A/-P` Stationsname, Slogan, Nachricht,
Land, Titel, Interpret, Programmtyp.

Steuerung per UDP wie beim Original: Port 8888 `freq:gain` (Fading), zusätzlich `title=…`, `artist=…`,
`message=…` (laufende Texte); Port 8889 `amp:ms` (Sferics).

Der Start dauert wegen der Audio-Vorpufferung etwa 1,5 bis 3 s, bis echtes Audio gesendet wird
(ein L1-Frame dauert 1,486 s). Das Signal liegt zunächst als Träger plus Digital-Stille an.

## Python-TK UI

Im Verzeichnis "UI" liegt zusätzlich eine multilinguales PythonTK Nutzerinterface zur Kontrolle des kompletten
Sendeprozesses. 

![ui](/UI/images/mainui.png)

## Prüfung

Als Gegenstelle diente der Open-Source-Decoder **nrsc5** (mit `--am`), davor eine Empfangskette
(`tools/rf_to_nrsc5.py`: Mischen, Dezimieren, Umtasten auf 46511,71875 Hz).

| Test | Ergebnis |
|---|---|
| Referenzdatei aus https://www.sigidwiki.com/wiki/HD_Radio_(AM) | MA1, AAB=8 kHz, PL niedrig, HPP aus, volle Digitalbandbreite |
| MA1, MA1+aab, +pl, +hpp, +rdb (12 Bit) | nrsc5 synchronisiert, meldet die Flags richtig, dekodiert SIS (Name, Land, Slogan, Standort, Zeit), PSD (Titel/Interpret), Audio-Service |
| MA1 mit echtem HDC | dekodiertes Audio korreliert zu 0,93 mit dem Original (Codec verlustbehaftet), BER im Decoder ca. 0,003, unabhängig von der DAC-Tiefe (8 bis 14 Bit) |
| Analog-Pfad | Hüllkurvendemodulation korreliert zu 0,988 mit dem Original, Verzögerung exakt 5,5 s |
| Spektrum (12 Bit) | −58 dBc bei ±16 kHz, −93 dBc bei ±18 kHz, danach nur Quantisierungsboden (−102 dBc/300 Hz), keine Spiegel bei 46,5 kHz |
| Echtzeit (UDP-Audio, TCP, gedrosselt auf 5 MB/s) | 2 Sender (MA1 plus MA3) ohne Underruns |
| Rechenlast | 3 Sender: 20 s Signal in 3,5 s auf einem Kern (rund 5,7-fache Echtzeit). Auf einem Core2Duo benötigen 2 Sender (MA1 plus MA3) 30 bis 40 % CPU (Anwenderbericht) |
| **Hardware-Empfänger Sangean HDR-1, HDR-16, HDR-18** | **MA1 (603 kHz, AAB) und MA3 (1017 kHz, HPP) gleichzeitig, beide Kanäle fehlerfrei dekodiert, Anzeige HDAM / HD AM mit Stations- und PSD-Text, Dauerbetrieb über Stunden ** |

## Wichtige Einschränkungen

* **MA3 und nrsc5:** Der Open-Source-Decoder nrsc5 synchronisiert auf kein MA3-Signal, auch nicht auf ein
  rein synthetisch in Python erzeugtes. Das liegt also am Decoder, nicht am Modulator: Ein echter
  Hardware-Empfänger (Sangean HDR-1, HDR-16, HDR-18) dekodiert MA3 fehlerfrei (siehe Tabelle). Für Tests von MA3
  daher einen Hardware-Empfänger benutzen, nicht nrsc5.
* Nicht umgesetzt: AAS/LOT-Datendienste (Albumcover, SIG) und Notfallmeldungen (EA). Der Datenkanal P3
  wird mit Füllbytes gesendet, nur HD1 (Programm 0) wird übertragen, Audio ist mono.
* Die 5,5 s Analogverzögerung stammt aus dem gr-nrsc5-Flowgraph. Die Norm nennt 4,5 Frames
  (6,69 s) für das Blending im Empfänger. Mit `-D 6.687` einstellbar.
* Die Trägerpegel (MA1 0,33, MA3 0,09 vom Vollausschlag) sind so gewählt, dass die Spitzen des
  OFDM-Signals nicht begrenzt werden. Bei mehreren Sendern wird durch die Anzahl geteilt.
* Die Sferics-Amplitude ist wegen des anderen Pegelbezugs nicht 1:1 mit dem Original vergleichbar. Für die Sferics-Simulation
  können die Sferics-Generatoren aus den Ursprungsprojekt AMWaveSynth und der Ausbreitungssimulation AMWaveSynthPropagationSimulator https://github.com/radiolab81/AMWaveSynthPropagationSimulator
  genutzt werden!
* Der Standort in der SIS (Erfurt), MEZ/EU-Sommerzeit und das Land `DE` sind Voreinstellungen im Code
  (`sis_init`).

## Lizenz

Die Protokollteile (L1, L2, SIS, PSD) sind aus **gr-nrsc5** (Clayton Smith, GPL-3.0) portiert. Der
Quelltext steht damit ebenfalls unter GPL-3.0. Der HDC-Encoder ist das gepatchte fdk-aac
(`argilo/fdk-aac`, Branch `hdc-encoder`) und unterliegt dessen eigener Lizenz.

