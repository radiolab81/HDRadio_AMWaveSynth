# HD-Radio-Modulator GUI (hdtxgui.py)

Python3/Tk-Oberfläche für `hdradio_am`, Nachfolger von `amtxgui.py`.

## Dateien (alle in einen Ordner, zusammen mit dem Modulatorprogramm `hdradio_am` legen)

| Datei | Zweck |
|---|---|
| `hdtxgui.py` | die GUI (`python3 hdtxgui.py`) |
| `start_sender_hd.sh` | startet je Sender eine ffmpeg-Instanz und den Modulator, beendet alles sauber beim Stoppen |
| `hdradio_am` | der Modulator (Pfad in *Modulator → Einstellungen* änderbar) |
| `start_fl2k.sh` | wie bisher das Startskript für `fl2k_tcp`, wird per `sudo` im eigenen xterm gestartet, bei smiSDR/parlioSDR/anderen SDR-TXs auskommentieren |
| `stations.db` | Internetradio-Datenbank, Zeilen `Name,URL` (Format wie bisher, Beispiel: `stations_example.db`) |
| `hdtx_settings.json` | wird von der GUI angelegt (Einstellungen, Sprache) |

Voraussetzungen: `python3-tk`, `ffmpeg`, `xterm`; `socat` nur für die SDR-Brücke. Das alte `stop_sender.sh` wird nicht mehr gebraucht.

## Sender anlegen (Sender → Hinzufügen, Doppelklick = Bearbeiten)

* **Frequenzplan / Frequenz** wie bisher (Europa 9 kHz, USA 10 kHz, CH/IT-Sonderfälle, manuelle Eingabe). Erlaubt sind 100 bis 2400 kHz.
* **Modus:** MA1 (Hybrid, Analog-AM plus Digital) oder MA3 (All-Digital).
* **Optionen:** AAB (Analog-Audio 8 statt 5 kHz), PL (Sekundär/Tertiär-Leistung hoch), HPP (PIDS-Leistung hoch), RDB (reduzierte Digitalbandbreite). Bei MA3 sind AAB und PL nicht wählbar, weil sie dort keine Wirkung haben.
* **Programm wählen:** Auswahl aus `stations.db` füllt die URL automatisch.
* **URL / Pfad:** frei einfügbar, alles was ffmpeg lesen kann (Stream-URL, lokale Datei, Playlist). *Datei ...* öffnet einen Dateidialog.
* Es gibt **keine Audiobandbreite** mehr: ffmpeg liefert immer das volle Band (s16le, mono, 44,1 kHz) mit `volume=0.8` und dem Kompressor aus dem alten Skript. Die Begrenzung auf 5/8 kHz für den Analogpfad macht der Modulator selbst.
* Unter den Optionen steht die ungefähre belegte Bandbreite. Überlappt das Digitalsignal mit einem anderen Sender (MA1 ca. ±14,8 kHz, MA3 ca. ±9,6 kHz, MA3+RDB ca. ±5 kHz), kommt eine Warnung, die Sie bestätigen können. Maximal 8 Sender.

## CSV-Senderlandschaften (Datei → Laden / Speichern)

Neues Format, Trennzeichen `;`: `Frequenz;Modus;Optionen;Programmname;URL / Pfad`, Beispiel `Beispiel_Senderlandschaft_HD.csv`.
Alte Dateien aus `amtxgui.py` (`Frequenz;Bandbreite;Programmname;URL` werden erkannt und als MA1 ohne Optionen geladen. Die Bandbreite entfällt.
Die Kopfzeile darf in jeder Sprache stehen.

## Start / Stopp und Anzeigen

*ALLE Sender STARTEN* öffnet ein xterm mit `start_sender_hd.sh` (UDP-Audioports ab 1234 in Tabellenreihenfolge) und startet nach 3 s optional Ihr `start_fl2k.sh`. *STOPPEN* schließt das xterm, das Skript beendet daraufhin ffmpeg, Modulator und socat. Zwei Ampeln: Modulator läuft (`hdradio_am`) und SDR-Verbindung (`fl2k_tcp` oder `socat`).

## Live-Steuerung (bei laufendem Modulator, UDP 8888/8889)

* Titel/Interpret (PSD) und Nachricht (SIS) ändern (RDS-like)
* Pegel des gewählten Senders per Schieberegler (Fading, 0 bis 1,5) -> für automatische Fadingsansteuerung des Modulators siehe Projekt AMWaveSynthPropagationSimulator https://github.com/radiolab81/AMWaveSynthPropagationSimulator
* Sferics/Blitz mit Stärke und Dauer, nur Single-Trigger einer Blitzentladung (Testfall), für automatisch durchziehende Gewitterfronten siehe Projekt AMWaveSynthPropagationSimulator https://github.com/radiolab81/AMWaveSynthPropagationSimulator

## Modulator → Einstellungen

DAC-Bittiefe, Samplerate (5 oder 10 MSPS, 10 = doppelt ausgegeben wie bisher), Stationsname, Slogan, Nachricht, Land, Programmtyp, Analog-Modulationsgrad und -Verzögerung, Trägerpegel, Modulator-Pfad, SDR-Brücke (IP, Port, Remote-fl2k) und der fl2k-Befehl.

## Grenzen

* **Stationsname, Slogan, Nachricht, Land und Programmtyp gelten für alle Sender gemeinsam**, weil der Modulator sie nur einmal entgegennimmt. Eigene Namen je Sender wären eine kleine Änderung im Modulator.
* Der **Trägerpegel** (leer lassen = Standard MA1 0,33, MA3 0,09) gilt für alle Sender. Hohe Werte übersteuern vor allem MA3.
* Die Spalte *Programmname* dient nur der Anzeige und der Datenbankauswahl, sie wird nicht gesendet.
