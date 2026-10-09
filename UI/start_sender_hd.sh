#!/bin/bash
# ============================================================================
#  start_sender_hd.sh - Multi-Sender-Start fuer den HD-Radio-Modulator (hdradio_am)
#
#  Aufruf (Gruppen zu je 5 Argumenten, beliebig oft wiederholbar, max. 8 Sender):
#     ./start_sender_hd.sh  FREQ  MODUS  OPTIONEN  URL_ODER_PFAD  UDP-PORT  [naechster Sender ...]
#
#     FREQ     Frequenz in kHz (< 50000) oder in Hz (wie im Original start_sender.sh)
#     MODUS    ma1 (Hybrid) | ma3 (All-Digital)
#     OPTIONEN Komma-Liste aus aab,pl,hpp,rdb  oder  -  fuer keine
#     URL      Internetradio-URL oder lokaler Dateipfad (alles, was ffmpeg lesen kann)
#     UDP-PORT Audio-Port des Senders im Modulator (GUI vergibt 1234, 1235, ...)
#
#  Die Einstellungen unten koennen per Umgebungsvariable (z.B. durch die GUI) ueberschrieben werden.
#  Audio: HD Radio hat eine feste Audiobandbreite. Fuer ffmpeg gibt es daher keinen Tiefpass:
#  der digitale Pfad (HDC-Codec) bekommt das volle Band, der Analogpfad (nur MA1) wird im
#  Modulator selbst auf 5 bzw. 8 kHz (Option aab) begrenzt. Format zum Modulator: s16le, mono, 44100 Hz.
# ============================================================================
echo "--- HD RADIO MULTI-SENDER START ---"

# ************ SDR Settings ******************
SDR_DAC_BITS=${SDR_DAC_BITS:-8}
SDR_SAMPLERATE=${SDR_SAMPLERATE:-10.0}     # 5 = 5 MSPS nativ, 10 = 5 MSPS doppelt ausgegeben (fl2k_tcp -s 10000000)
SDR_IP=${SDR_IP:-127.0.0.1}
SDR_PORT=${SDR_PORT:-1234}
SDR_REMOTE_FL2k=${SDR_REMOTE_FL2k:-false}
# ************ SDR Settings ******************

# ************ Modulator Settings ************
HDTX_BIN=${HDTX_BIN:-./hdradio_am}
HDTX_NAME=${HDTX_NAME:-HDAM}               # SIS-Stationsname (bis 4 Zeichen A-Z = Kurzformat, sonst Langformat bis 12 Zeichen)
HDTX_SLOGAN=${HDTX_SLOGAN:-HD Radio AM Modulator}
HDTX_MESSAGE=${HDTX_MESSAGE:-5 MSPS Integer NRSC-5 MA1/MA3}
HDTX_COUNTRY=${HDTX_COUNTRY:-DE}
HDTX_PTYPE=${HDTX_PTYPE:-0}
HDTX_MD=${HDTX_MD:-0.85}                   # Analog-Modulationsgrad (nur MA1)
HDTX_ADELAY=${HDTX_ADELAY:-5.5}            # Analog-Verzoegerung in Sekunden (nur MA1)
HDTX_LEVEL=${HDTX_LEVEL:-}                 # Traegerpegel (leer = Standard des Modulators)
# ************ Modulator Settings ************

STATIONS=()
N=0

while (( "$#" >= 5 )); do
    FREQ=$1; MODE=${2,,}; OPTS=${3,,}; URL=$4; PORT=$5

    # kHz oder Hz? (Grenze 50000, Nachkommastellen erlaubt)
    FREQ_HZ=$(awk -v f="$FREQ" 'BEGIN { if (f < 50000) f = f * 1000; printf "%d", f + 0.5 }')

    if [ "$MODE" != "ma1" ] && [ "$MODE" != "ma3" ]; then
        echo "FEHLER: Unbekannter Modus '$MODE' (erlaubt: ma1, ma3)"; exit 1
    fi

    SPEC="$PORT:$FREQ_HZ:$MODE"
    if [ -n "$OPTS" ] && [ "$OPTS" != "-" ]; then SPEC="$SPEC,$OPTS"; fi
    STATIONS+=("$SPEC")
    N=$((N + 1))

    echo "Starte ffmpeg Instanz: $FREQ kHz (-> $FREQ_HZ Hz) | ${MODE^^} ${OPTS} | Port: $PORT | $URL"
    ffmpeg -nostdin -loglevel warning -stream_loop -1 -re -i "$URL" \
           -af "volume=0.8, acompressor=threshold=-10dB:ratio=4" \
           -f s16le -ar 44100 -ac 1 "udp://127.0.0.1:$PORT" &

    shift 5
done

if [ "$N" -eq 0 ]; then echo "FEHLER: keine Sender angegeben."; exit 1; fi
if [ ! -x "$HDTX_BIN" ]; then echo "FEHLER: Modulator '$HDTX_BIN' nicht gefunden/ausfuehrbar."; kill $(jobs -p) 2>/dev/null; exit 1; fi

# Alles beenden, wenn das Skript endet oder das Terminal geschlossen wird (GUI: STOPPEN)
cleanup() {
    trap - EXIT INT TERM HUP
    echo "Beende alle Prozesse ..."
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
    exit 0
}
trap cleanup EXIT INT TERM HUP

MOD_CMD=("$HDTX_BIN" -b "$SDR_DAC_BITS" -s "$SDR_SAMPLERATE"
         -N "$HDTX_NAME" -S "$HDTX_SLOGAN" -M "$HDTX_MESSAGE" -C "$HDTX_COUNTRY" -P "$HDTX_PTYPE"
         -m "$HDTX_MD" -D "$HDTX_ADELAY")
if [ -n "$HDTX_LEVEL" ]; then MOD_CMD+=(-L "$HDTX_LEVEL"); fi
MOD_CMD+=("${STATIONS[@]}")

echo "Starte Modulator: ${MOD_CMD[*]}"
"${MOD_CMD[@]}" &
MOD_PID=$!

sleep 3
if [[ "$SDR_IP" != "127.0.0.1" || "$SDR_PORT" != 1234 ]]; then
    echo "Starte socat Bruecke: localhost:12345 -> $SDR_IP:$SDR_PORT"

    if [ "$SDR_REMOTE_FL2k" = true ]; then
       # Startet die Bruecke SERVER -> [CLIENT -> socat -> SERVER] -> CLIENT im Hintergrund
       socat TCP4:localhost:12345,nodelay TCP4-LISTEN:1234,reuseaddr,nodelay &
    else
       # Startet die Bruecke SERVER -> [CLIENT -> socat -> CLIENT] -> SERVER im Hintergrund
       socat -u TCP4:localhost:12345,nodelay TCP4:$SDR_IP:$SDR_PORT,nodelay &
    fi
fi

echo "--------------------------"
echo "Alle Prozesse gestartet. Druecke STRG+C zum Beenden."
# Laeuft, solange der Modulator laeuft (er endet, wenn der SDR die TCP-Verbindung schliesst)
wait $MOD_PID
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    echo "Modulator wurde mit Fehlercode $STATUS beendet."
    read -r -p "Enter zum Schliessen ..."
fi
