// ============================================================================
//  HD Radio (NRSC-5-D) Mittelwellen-Modulator, 5 MSPS, Integer-Arithmetik
//  Modi: MA1 (Hybrid, Analog-AM + Digital) und MA3 (All-Digital),
//        jeweils mit RDB / HPP / PL / AAB Varianten.
//
//  Kompilieren (ohne HDC-Codec, Testmodus mit Dummy-Audio-Frames):
//    gcc -O3 -march=native hdradio_am_modulator_5MSPS_integer.c -o hdradio_am -lpthread -lm
//  Kompilieren (mit echtem HDC-Encoder, gepatchtes fdk-aac von argilo, Branch hdc-encoder):
//    gcc -O3 -march=native -DUSE_FDK_HDC -I$FDK/include/fdk-aac  (Zeilenfortsetzung)
//        hdradio_am_modulator_5MSPS_integer.c -o hdradio_am -L$FDK/lib -lfdk-aac -lpthread -lm
//
//  Aufruf: ./hdradio_am [Optionen] <Port:Frequenz[:Modus-Optionen]> ...
//  Ausgabe: TCP Port 12345 (fl2k_tcp, smiSDR, parlioSDR) oder Datei (-o)
//
//  ----------------------------------------------------------------------------
//  QUELLEN UND ABKUERZUNGEN IN DEN KOMMENTAREN
//  ----------------------------------------------------------------------------
//  Normseite NRSC-5-D (Standard und Referenzdokumente):
//    https://www.nrscstandards.org/standards-and-guidelines/documents/standards/nrsc-5-d/nrsc-5-d.asp
//  [1012s]  HD Radio Air Interface Design Description - Layer 1 AM, Rev. G (14.12.2016)
//           Hauptquelle fuer alles ab "L1:" und fuer die Signalerzeugung. Die Paragraphen-
//           nummern (z.B. "1012s 10.3.1") wurden gegen das Inhaltsverzeichnis Rev. G geprueft.
//  [1082s]  HD Radio AM Transmission System Specifications, Rev. G
//           Absolute Subtraegerleistungen (dBc), Spektralmasken, Traegerfrequenz/Kanalraster.
//           Genutzt: 4.6.1 (AM Digital Carrier Power: 4.6.1.1 Hybrid MA1, 4.6.1.3 All Digital
//           MA3), 4.10.1.x (Subtraeger-Skalierung: .1 Referenz, .3 Sekundaer/PIDS MA1+PIDS MA3,
//           .4 Sekundaer/Tertiaer MA3, .5 Primaer MA1, .6 Primaer MA3), 4.5.x (Emissionsgrenzen).
//  [1014s]  Layer 2 Channel Multiplex, Rev. J          (PDU-Aufbau, Header-Spreizung)
//  [1017s]  Audio Transport, Rev. H                    (Audio-Pakete, Control Word, Locator)
//  [1020s]  Station Information Service Transport, Rev. J   (SIS-PDU, 80 Bit je L1-Block)
//  [1028s]  Program Service Data, Rev. E  und  [1085s] PSD Transport, Rev. D  (Titel/Interpret)
//  [RFC1662] PPP in HDLC-like Framing (HDLC-Rahmen, Byte-Stuffing, FCS-16)
//  [gr-nrsc5] https://github.com/argilo/gr-nrsc5 (GPL-3.0). Referenzimplementierung, aus der
//           L1/L2/SIS/PSD portiert wurden. Die Vorgaben der Norm sind dort in C++/Python/GNU
//           Radio umgesetzt; wo ein Detail nur dort belegt wurde, steht "gr-nrsc5".
//  [nrsc5]  https://github.com/theori-io/nrsc5 (Open-Source-Decoder, Pruefwerkzeug).
//
//  HINWEIS ZU DEN VERWEISEN: Fuer [1012s] und [1082s] sind die Abschnittsnummern verifiziert.
//  Bei [1014s], [1017s], [1020s], [1028s] aendert sich die Nummerierung zwischen den Revisionen;
//  dort stehen Kapitel bzw. Stichworte, nach denen im jeweiligen Dokument gesucht werden kann.
//  Der HDC-Audiocodec selbst ist NICHT Teil der NRSC-5-Normdokumente (proprietaer); die Norm
//  beschreibt nur den Transport der HDC-Frames ([1017s]).
//
//  ----------------------------------------------------------------------------
//  SIGNALKETTE (je Sender; Zahlen gelten fuer NRSC-5-D, Systemparameter in [1012s] 3.5)
//  ----------------------------------------------------------------------------
//   Audio 44.1 kHz mono (UDP)
//    |- digital: HDC-Encoder -> L2 (PDU, Reed-Solomon, PSD, SIS) -> L1 (Scrambler, Faltungs-
//    |           codierer E1/E2/E3, Interleaver, Diversity-Delay, Subtraeger-Mapping, R-Kanal)
//    |           -> IFFT (hier 4096 Punkte = 16-fach ueberabgetastet) -> Pulsformung
//    |           -> komplexes Basisband, 744187.5 Hz
//    '- analog (nur MA1): Tiefpass 5/8 kHz -> Analog-Diversity-Delay -> 1 + m(t) auf I-Achse
//   Basisband -> kubische Farrow-Interpolation auf 5 MSPS -> NCO-Mischer (Traegerfrequenz)
//   -> Summe aller Sender -> Sferics (optional) -> Begrenzer -> int8/int16 -> TCP/Datei
//
//  FESTKOMMA-KONVENTIONEN
//   Frequenzbereich X[k]:  1.0 = Amplitude des unmodulierten Traegers = 2^22  (QF)
//   Basisband/RF:          1.0 = 2^15 (QB); Spitzen bis ca. +-9 bei MA3 (int32/int64 rechnen)
//   Sinus-LUT, Pulsfenster Q15; FFT-Twiddles Q30; Farrow-Koeffizienten int64
//   Float wird nur beim Programmstart (Tabellen, Filterentwurf) und in der langsamen
//   Analog-AGC (alle 10 ms) benutzt, nicht in der Signalschleife bei 5 MSPS.
//
//  PRUEFSTATUS: nrsc5 --am dekodiert MA1 in allen Varianten (SIS, PSD, Audio). Auf einem
//  Hardware-Empfaenger Sangean HDR-1 wurden MA1 und MA3 gleichzeitig (2 Sender) fehlerfrei
//  dekodiert (Bericht des Anwenders, Oktober 2026), Dauerbetrieb ueber Stunden.
//  Lizenz: Protokollteile aus gr-nrsc5 (GPL-3.0) -> dieser Quelltext steht unter GPL-3.0.
// ============================================================================
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <time.h>

#ifdef USE_FDK_HDC
#include "aacenc_lib.h"
#endif

//  ---------------------------------------------------------------------------
//  SYSTEMKONSTANTEN  (Herkunft: [1012s] 3.5 "AM System Parameters")
//  ---------------------------------------------------------------------------
//  Subtraegerabstand  df = 1488375/8192 Hz = 181.7 Hz           ([1012s] 3.5)
//  Cyclic-Prefix-Weite alpha = 7/128; Symboldauer Ts = (1+alpha)/df = 5.805 ms,
//  Symbolrate Rs = 172.3 Hz; L1-Frame Tf = 65536/44100 s = 256*Ts = 1.486 s;
//  L1-Block Tb = 32*Ts = 0.1858 s (8 Bloecke je Frame).
//  OFDM-FFT-Rate der Norm: 256*df = 46511.71875 Hz, 270 Samples je Symbol (256 + 14 CP).
//  Wir rechnen 16-fach ueberabgetastet (4096-Punkt-IFFT): 744187.5 Hz, 4320 Samples je Symbol,
//  CP = 224 Samples. Der Faktor 16 ist keine Normvorgabe, sondern Implementierungswahl:
//  er macht die spaetere Interpolation auf 5 MSPS mit einem kurzen kubischen Filter moeglich.
//  Die FFT-Groesse 4096 erzeugt dabei exakt die bandbegrenzte Interpolation des Normsignals.
#define FS_BB        744187.5          // Basisband-Rate = 16 * 46511.71875 Hz
#define OSF          16                // Oversampling gegenueber der OFDM-FFT-Rate
#define NFFT         4096              // 256 * OSF
#define SYM_BB       4320              // 270 * OSF  Samples pro OFDM-Symbol (mit CP)
#define CP_BB        224               // 14 * OSF   Cyclic Prefix
//  QF / QB: Festkomma-Skalen, siehe Dateikopf (Frequenzbereich 2^22, Basisband 2^15).
//  BB_RING: Basisband-FIFO je Sender (16384 Samples = 22 ms bei 744187.5 Hz).
//  AUD_RING: Audio-Ringpuffer je Sender, 2^20 Samples = 23.8 s bei 44.1 kHz; muss Analog-
//  Verzoegerung (5.5 s) + Vorpufferung (Frame-Granularitaet) abdecken.
#define QF           22                // Frequenzbereich: Traeger-Amplitude 1.0 = 2^22
#define QB           15                // Basisband-/RF-Bereich: 1.0 = 2^15
#define BB_RING      16384             // Basisband-FIFO je Sender (Samples)
#define AUD_RING     (1<<20)           // Audio-Ringpuffer (int16 Samples @44.1 kHz)
//  SIS_BITS: Groesse einer SIS-PDU = 80 Bit je L1-Block, Logischer Kanal PIDS ([1012s] Tabelle 7-3/7-4).
//  DIV_DELAY: Digitaler Diversity-Delay Tdd = Ndd * Tf mit Ndd = 3 Frames ([1012s] 3.5, Tabelle 10-3);
//  ein Unterframe (BL, BU, EBL, EBU) hat 18000 Bit je Frame, also 3*18000 Bit Verzoegerung.
//  SYMS_FRAME = 256 OFDM-Symbole je L1-Frame, BLOCKS_FRAME = 8 Bloecke ([1012s] 6.6, Bild 6-2).
//  FRAME_AUD = 65536 Audio-Samples = exakt ein L1-Frame bei 44.1 kHz (Tf = 65536/44100 s).
#define SYMS_FRAME   256
#define BLOCKS_FRAME 8
#define FRAME_AUD    65536             // Audio-Samples je L1-Frame (44100 Hz)
#define SIS_BITS     80
#define DIV_DELAY    (18000*3)
//  LUT_BITS: Phasenaufloesung des NCO. Eine Sinus-Tabelle mit 2^14 Eintraegen (32 KiB) bleibt im
//  L1-Cache; die Phasentruncation liegt bei ca. -86 dBc (Spur). Der Cosinus wird aus derselben
//  Tabelle mit +90 Grad Versatz (LUT_SIZE/4) gelesen.
//  MAX_TX: maximale Zahl gleichzeitiger Sender (Traeger) im Summensignal.
//  OUT_CHUNK: Verarbeitungsschritt der Hauptschleife = 10 ms bei 5 MSPS; ein Chunk wird
//  komplett erzeugt und dann per send() an den DAC-Prozess (fl2k_tcp, smiSDR, parlioSDR) gegeben.
#define LUT_BITS     14
#define LUT_SIZE     (1<<LUT_BITS)
#define MAX_TX       8
#define OUT_CHUNK    50000             // RF-Samples je Verarbeitungsschritt (10 ms @ 5 MSPS)

static int16_t sine_lut[LUT_SIZE];     // sin; cos = sin(+90 Grad)
//  pf_win[]: Pulsformungsfenster der OFDM-Symbole, 16-fach ueberabgetastet, Q15, 8192 Werte
//  (zwei Symbolhaelften). Wird in init_tables() berechnet; Herleitung dort.
static int16_t pf_win[2*NFFT];         // Pulsformung, 16-fach, Q15 (8192)
static uint16_t fft_rev[NFFT];

// ------------------------------------------------------------ Hilfsfunktionen
static uint8_t crc8_tab[256];
//  ----------------------------------------------------------------------------
//  init_crc8() - Tabelle fuer CRC-8 (Polynom x^8+x^5+x^4+1 = 0x31, MSB zuerst)
//    Verwendung: Pruefsumme je Audio-Paket (HDC-Frame) im Audio-Transport-PDU, Startwert 0xFF
//    (siehe l2_pdu()). Quelle: [1017s] Audio Transport (Audio-Paket mit CRC-8-Feld);
//    Parameter wie in [gr-nrsc5] l2_encoder, durch nrsc5-Dekodierung bestaetigt.
//  ----------------------------------------------------------------------------
static void init_crc8(void){
    // Skalieren (Q22) und auf die FFT-Bins legen: Subtraeger k = i-128 liegt auf Bin (k mod 4096).
    for(int i=0;i<256;i++){ uint8_t r=i; for(int k=0;k<8;k++) r=(r&0x80)?(r<<1)^0x31:(r<<1); crc8_tab[i]=r; }
}

// FCS16 (HDLC, X.25) bitweise
//  ----------------------------------------------------------------------------
//  fcs16() - Frame Check Sequence 16 Bit (CRC-16/X.25, reflektiertes Polynom 0x8408,
//    Startwert 0xFFFF, Endwert invertiert). Quelle: [RFC1662] Abschnitt "Frame Check
//    Sequence (FCS) Field" / Anhang "16-bit FCS Computation Method".
//    Wird fuer HDLC-Pakete (PSD und Kontrollbytes des Fixed-Data-Subchannels) benoetigt.
//  ----------------------------------------------------------------------------
static uint16_t fcs16(const uint8_t *d, int n){
    uint16_t r=0xffff;
    for(int i=0;i<n;i++){ r^=d[i]; for(int k=0;k<8;k++) r=(r&1)?(r>>1)^0x8408:(r>>1); }
    return r^0xffff;
}

// HDLC-Kapselung (Flag 7e, Byte-Stuffing, FCS16 LSB-first) wie gr-nrsc5 hdlc.cc
//  ----------------------------------------------------------------------------
//  hdlc_encode() - HDLC-Rahmen: Flag 0x7E | Daten | FCS (LSB zuerst) | Flag 0x7E
//    Zeichen 0x7E und 0x7D im Inhalt werden als 0x7D, (Zeichen XOR 0x20) gesendet
//    ("Byte-Stuffing"). Quelle: [RFC1662] Abschnitt 4 (Octet-stuffed framing); die
//    NRSC-5-Dokumente verweisen fuer PSD/AAS-Pakete darauf ([1085s] PSD Transport,
//    [2690s] Referenzliste). Rueckgabe: Laenge des Rahmens in Bytes.
//  ----------------------------------------------------------------------------
static int hdlc_encode(const uint8_t *in, int n, uint8_t *out){
    int o=0; uint16_t f=fcs16(in,n);
    out[o++]=0x7e;
    uint8_t tmp[1024]; memcpy(tmp,in,n); tmp[n]=f&0xff; tmp[n+1]=f>>8;
    for(int i=0;i<n+2;i++){
        if(tmp[i]==0x7e||tmp[i]==0x7d){ out[o++]=0x7d; out[o++]=tmp[i]^0x20; } else out[o++]=tmp[i];
    }
    out[o++]=0x7e;
    return o;
}

// Reed-Solomon (255,247) GF(2^8), Poly 0x11d, fcr=1, prim=1, 8 Paritaetssymbole
static uint8_t gf_exp[512], gf_log[256], rs_gen[9];
//  ----------------------------------------------------------------------------
//  init_rs() / rs_encode() - Reed-Solomon-Code ueber GF(2^8)
//    Koerperpolynom x^8+x^4+x^3+x^2+1 (0x11D), erste Nullstelle alpha^1, 8 Paritaets-
//    symbole: RS(255,247), hier verkuerzt auf (96,88). Es werden die 88 Bytes eines
//    Audio-Transport-PDU nach den Paritaetsbytes geschuetzt (Control Word, Locator,
//    HEF, PSD, Audiodaten, siehe l2_pdu()). Quelle: [1017s] Audio Transport (Schutz
//    des Audio-PDU-Kopfes durch Reed-Solomon); Parameter wie [gr-nrsc5] l2_encoder
//    (init_rs_char(8,0x11d,1,1,8)). Die Paritaet wird per LFSR bestimmt; das Ergebnis
//    wurde mit nrsc5 und einem Hardware-Empfaenger verifiziert.
//  ----------------------------------------------------------------------------
static void init_rs(void){
    int x=1;
    for(int i=0;i<255;i++){ gf_exp[i]=x; gf_log[x]=i; x<<=1; if(x&0x100) x^=0x11d; }
    for(int i=255;i<512;i++) gf_exp[i]=gf_exp[i-255];
    // Generatorpolynom prod (x - a^(1+i)), i=0..7
    uint8_t g[9]={1}; int deg=0;
    for(int i=0;i<8;i++){
        uint8_t ng[9]={0};
        for(int j=0;j<=deg;j++){
            ng[j+1]^=g[j];
            if(g[j]) ng[j]^=gf_exp[(gf_log[g[j]]+1+i)%255];
        }
        deg++; memcpy(g,ng,9);
    }
    memcpy(rs_gen,g,9);   // g[deg]=1 (hoechster Koeffizient), g[0] = konstant
}

// Systematische Kodierung: data[0..n-1] (n=247 fuer volle Laenge), Parity in par[0..7]
// (gleiche Konvention wie Karn encode_rs_char: data[0] ist hoechster Koeffizient)
//  rs_encode() - systematische RS-Kodierung per LFSR: data[0..n-1] (data[0] = hoechster Koeffizient)
//    -> 8 Paritaetsbytes par[0..7]. Register reg[0] = Koeffizient von x^7. Rueckkopplung
//    fb = Eingabe XOR reg[0], danach reg[j] = reg[j+1] XOR fb*g[7-j], reg[7] = fb*g[0] mit dem in
//    init_rs() berechneten Generatorpolynom g(x) = PRODUKT (x - alpha^(1+i)), i = 0..7. Verwendung
//    in l2_pdu(): n = 88 Datenbytes.
static void rs_encode(const uint8_t *data, int n, uint8_t *par){
    uint8_t reg[8]={0};
    for(int i=0;i<n;i++){
        uint8_t fb=data[i]^reg[0];
        for(int j=0;j<7;j++){
            reg[j]=reg[j+1]^(fb?gf_exp[gf_log[fb]+gf_log[rs_gen[7-j]]]:0);
        }
        reg[7]=fb?gf_exp[gf_log[fb]+gf_log[rs_gen[0]]]:0;
    }
    memcpy(par,reg,8);
}

// ============================================================================
//  SIS (Station Information Service) - Port von gr-nrsc5 sis_encoder (AM, ohne EA)
// ============================================================================
//  Die SIS-PDU ([1020s] Kapitel 4 "SIS Protocol Data Unit Format") hat 80 Bit je L1-Block und
//  laeuft ueber den logischen Kanal PIDS ([1012s] 4.4, Tabelle 7-3/7-4: 80 Bit, Rate Rb).
//  Aufbau einer PDU (siehe sis_frame()): Typbit, Nachrichtenzahl-Bit, 1 oder 2 Nachrichten
//  (je 4 Bit Nachrichten-ID + Inhalt), Auffuellen bis Bit 64, 2 reservierte/Zeitbits,
//  2 Bit ALFN-Teil, 12 Bit CRC.
//  Nachrichten-IDs ([1020s] Kapitel 4, je Nachricht ein Unterkapitel):
//    0000 Station ID Number, 0001 Station Name short, 0010 Station Name long,
//    0100 Station Location, 0101 Station Message, 0110 Service Information Message,
//    0111 SIS Parameter Message, 1000 Universal Short Station Name / Station Slogan (4.8).
//  Der Zeitplan der Nachrichten ueber die 8 Bloecke eines Frames lehnt sich an das
//  Beispiel-Scheduling in [1020s] Kapitel 5 ("Example Scheduling of SIS PDU Messages") an;
//  die konkrete Tabelle stammt aus [gr-nrsc5] sis_encoder (schedule_am_short_no_ea / _long_no_ea).
//  Emergency Alert (EA) wird nicht gesendet. Der Strukturtyp sis_t haelt den Fortschritt
//  (long_cur, msg_cur, ...) fuer mehrteilige Nachrichten, die ueber mehrere PDUs laufen.
typedef struct {
    uint32_t alfn;
    char country[3]; uint32_t fcc_id;
    char short_name[16]; int fm_suffix, std_short;
    char slogan[100], message[200];
    int nprog; int prog_type[8];
    float lat, lon, alt;
    int long_cur, long_seq, ussn_cur, slogan_cur, msg_cur, msg_seq, cur_service, cur_param, loc_high;
    int utc_offset, dst_sched;
    uint8_t *bit;
} sis_t;

enum { S_ID, S_SHORT, S_LONG, S_LOC, S_MSG, S_SVC, S_PAR, S_USSN, S_SLOGAN };
// Zeitplaene AM ohne Emergency-Alert: je Block bis zu 2 Eintraege (-1 = leer)
//  Zeitplaene: je Block (Zeile) bis zu 2 Nachrichten (Spalten), -1 = nicht belegt.
//  "short" wird benutzt, wenn der Stationsname <= 4 Zeichen aus dem 5-Bit-Zeichensatz ist
//  (Short Format, [1020s] 4.2.1); sonst "long" mit Universal Short Station Name
//  ([1020s] 4.8). Ein Frame (8 Bloecke) enthaelt so jede Nachricht mindestens einmal.
static const int sched_am_short[8][2] = {
    {S_SHORT,S_ID},{S_MSG,-1},{S_SVC,S_SHORT},{S_PAR,S_LOC},{S_SHORT,S_ID},{S_SLOGAN,-1},{S_SVC,S_SHORT},{S_LONG,-1} };
static const int sched_am_long[8][2] = {
    {S_USSN,-1},{S_MSG,-1},{S_SVC,S_LOC},{S_PAR,S_ID},{S_SVC,S_SVC},{S_SLOGAN,-1},{S_PAR,S_ID},{S_SVC,S_SVC} };

//  ----------------------------------------------------------------------------
//  sw_bit / sw_int / sw_char5 - Bitschreiber fuer die SIS-PDU
//    sw_int schreibt len Bit MSB zuerst (negative Werte als Zweierkomplement, z.B. Koordinaten).
//    sw_char5 kodiert ein Zeichen im 5-Bit-Stationsnamenalphabet: A..Z = 0..25 (Gross/Klein
//    gleich), Leerzeichen = 26, '?' = 27, '-' = 28, '*' = 29, '$' = 30.
//    Quelle: [1020s] 4.2.1 "Station Name short format", Tabelle "Character Definitions".
//  ----------------------------------------------------------------------------
static void sw_bit(sis_t *s,int b){ *(s->bit++)=b; }
//  sw_int() - schreibt n als vorzeichenbehaftete Zahl mit len Bit, MSB zuerst (Zweierkomplement).
static void sw_int(sis_t *s,int n,int len){ if(n<0) n+=(1<<len); for(int i=0;i<len;i++) sw_bit(s,(n>>(len-i-1))&1); }
//  sw_char5() - schreibt ein Zeichen im 5-Bit-Stationsnamenalphabet (siehe sw_bit-Kopf).
static void sw_char5(sis_t *s,char c){
    int n;
    if(c>='A'&&c<='Z') n=c-'A'; else if(c>='a'&&c<='z') n=c-'a';
    else switch(c){ case '?':n=27;break; case '-':n=28;break; case '*':n=29;break; case '$':n=30;break; default:n=26; }
    sw_int(s,n,5);
}

//  ----------------------------------------------------------------------------
//  sis_crc12() - 12-Bit-CRC ueber die ersten 68 Bit der SIS-PDU
//    Polynom (reflektiert) 0xD010, Anfangswert 0, anschliessend 16 Nullbits nachgeschoben und
//    mit 0x955 XOR verknuepft. Das Ergebnis steht in den letzten 12 Bit der PDU.
//    Quelle: [1020s] Abschnitt "CRC Field" (in aelteren Revisionen 4.7). Konstanten aus
//    [gr-nrsc5] sis_encoder::crc12; Empfaenger (nrsc5, Sangean) pruefen diese CRC.
//  ----------------------------------------------------------------------------
static int sis_crc12(const uint8_t *sis){
    uint16_t poly=0xD010, reg=0; int lowbit;
    for(int i=67;i>=0;i--){ lowbit=reg&1; reg>>=1; reg^=((uint16_t)sis[i]<<15); if(lowbit) reg^=poly; }
    for(int i=0;i<16;i++){ lowbit=reg&1; reg>>=1; if(lowbit) reg^=poly; }
    return reg^0x955;
}

//  ----------------------------------------------------------------------------
//  sis_station_id() - Nachricht 0000 "Station ID Number"
//    Inhalt: 2 Laendercodezeichen (5 Bit je, sw_char5), 3 reservierte Bit, 19 Bit
//    Facility-ID (FCC-Anlagennummer, hier 0). Quelle: [1020s] Kapitel 4 (Station ID Number).
//  ----------------------------------------------------------------------------
static void sis_station_id(sis_t *s){
    sw_int(s,0,4); sw_char5(s,s->country[0]); sw_char5(s,s->country[1]); sw_int(s,0,3); sw_int(s,s->fcc_id,19);
}

//  ----------------------------------------------------------------------------
//  sis_short() - Nachricht 0001 "Station Name short format"
//    4 Zeichen a 5 Bit + 2 Bit Namenserweiterung (01 = Suffix "-FM"). Quelle: [1020s] 4.2.1,
//    Bild "Station Name (short format) - Message Structure", Tabellen "Field Bit Assignments"
//    und "Character Definitions". Kuerzere Namen werden mit Leerzeichen aufgefuellt.
//  ----------------------------------------------------------------------------
static void sis_short(sis_t *s){
    sw_int(s,1,4); int l=strlen(s->short_name);
    for(int i=0;i<4;i++) sw_char5(s,i<l?s->short_name[i]:' ');
    sw_int(s,s->fm_suffix?1:0,2);
}

//  ----------------------------------------------------------------------------
//  sis_long() - Nachricht 0010 "Station Name long format"
//    Mehrteilig: 7 Zeichen je PDU (7-Bit-ASCII), 3 Bit Gesamtzahl-1, 3 Bit Frame-Nummer,
//    3 Bit Sequenznummer. Quelle: [1020s] Kapitel 4, Bild "Station Name (long format)".
//    Als Text wird hier der Slogan benutzt (Verhalten wie [gr-nrsc5]).
//  ----------------------------------------------------------------------------
static void sis_long(sis_t *s){
    sw_int(s,2,4); int nl=strlen(s->slogan); if(nl>56) nl=56;
    int nf=(nl+6)/7; if(nf<1) nf=1;
    sw_int(s,nf-1,3); sw_int(s,s->long_cur,3);
    for(int i=s->long_cur*7;i<s->long_cur*7+7;i++) sw_int(s,i<nl?(uint8_t)s->slogan[i]&0x7f:0,7);
    sw_int(s,s->long_seq,3);
    s->long_cur=(s->long_cur+1)%nf;
}

//  ----------------------------------------------------------------------------
//  sis_location() - Nachricht 0100 "Station Location"
//    Wechselweise obere und untere Haelfte (loc_high): Breite bzw. Laenge als 22-Bit-
//    Zweierkomplement in Einheiten von 1/8192 Grad, dazu je 4 Bit der Hoehe (Einheit 16 m,
//    8 Bit gesamt). Quelle: [1020s] Kapitel 4, Bild "Station Location - Message Structure".
//  ----------------------------------------------------------------------------
static void sis_location(sis_t *s){
    int a=(int)lroundf(s->alt/16); if(a>255)a=255; if(a<0)a=0;
    sw_int(s,4,4); sw_bit(s,s->loc_high);
    if(s->loc_high){ sw_int(s,(int)lroundf(s->lat*8192),22); sw_int(s,a>>4,4); }
    else           { sw_int(s,(int)lroundf(s->lon*8192),22); sw_int(s,a&0xf,4); }
    s->loc_high=!s->loc_high;
}

//  ----------------------------------------------------------------------------
//  sis_message() - Nachricht 0101 "Station Message" (Laufschrift, ISO-8859-1)
//    Frame 0: Textlaenge (8 Bit), 7-Bit-Pruefsumme, erste 4 Zeichen; Folgeframes je 6
//    Zeichen. Pruefsumme: Summe aller Zeichen, (Bits 14:8 + Bits 7:0) modulo 128.
//    Quelle: [1020s] Kapitel 4 "Station Message", Tabelle "Description of Station Message
//    Fields for Frame Number = 0", Tabelle "Text Encoding Definitions".
//  ----------------------------------------------------------------------------
static void sis_message(sis_t *s){
    sw_int(s,5,4); int ml=strlen(s->message); if(ml>190) ml=190;
    int nf=(ml+7)/6; if(nf<1) nf=1;
    sw_int(s,s->msg_cur,5); sw_int(s,s->msg_seq,2);
    if(s->msg_cur==0){
        unsigned cs=0; for(int j=0;j<ml;j++) cs+=(uint8_t)s->message[j];
        cs=(((cs>>8)&0x7f)+(cs&0xff))&0x7f;
        sw_bit(s,0); sw_int(s,0,3); sw_int(s,ml,8); sw_int(s,cs,7);
        for(int i=0;i<4;i++) sw_int(s,i<ml?(uint8_t)s->message[i]:0,8);
    } else {
        sw_int(s,0,3);
        for(int i=s->msg_cur*6-2;i<s->msg_cur*6+4;i++) sw_int(s,i<ml?(uint8_t)s->message[i]:0,8);
    }
    s->msg_cur=(s->msg_cur+1)%nf;
}

//  ----------------------------------------------------------------------------
//  sis_service() - Nachricht 0110 "Service Information Message"
//    Beschreibt die Dienste: hier ein Audio-Dienst (Kategorie 0), Nummer 0 (= HD1),
//    Programmtyp (8 Bit, "Audio Program Types", siehe NRSC Supplemental Information),
//    2 x 5 reservierte/Sound-Experience-Bit. Quelle: [1020s] Kapitel 4, Bilder
//    "Service Information Message" mit "Audio Service Descriptors".
//  ----------------------------------------------------------------------------
static void sis_service(sis_t *s){
    sw_int(s,6,4);
    sw_int(s,0,2); sw_bit(s,0); sw_int(s,s->cur_service,6); sw_int(s,s->prog_type[s->cur_service],8);
    sw_int(s,0,5); sw_int(s,0,5);
    s->cur_service=(s->cur_service+1)%s->nprog;
}

//  ----------------------------------------------------------------------------
//  sis_param() - Nachricht 0111 "SIS Parameter Message", 13 zyklisch gesendete Parameter
//    0 Schaltsekunden (aktuell/ausstehend = 18), 1/2 ALFN der Schaltsekunde, 3 Ortszeit-
//    daten (UTC-Offset in Minuten, 11 Bit; Sommerzeit-Schema 3 Bit; hier 60 min und
//    Schema 2 = Europa), 4-12 Hersteller-/Versionskennungen von Exciter und Importer
//    ("CS", 1.0.0.0) und Konfigurationsnummer. Quelle: [1020s] Kapitel 4 "SIS Parameter
//    Message"; Werte wie [gr-nrsc5] sis_encoder.
//  ----------------------------------------------------------------------------
static void sis_param(sis_t *s){
    sw_int(s,7,4); sw_int(s,s->cur_param,6);
    switch(s->cur_param){
    case 0: sw_int(s,18,8); sw_int(s,18,8); break;
    case 1: sw_int(s,0,16); break;
    case 2: sw_int(s,0,16); break;
    case 3: sw_int(s,s->utc_offset,11); sw_int(s,s->dst_sched,3); sw_bit(s,1); sw_bit(s,1); break;
    case 4: sw_bit(s,0); sw_int(s,'C',7); sw_bit(s,1); sw_int(s,'S',7); break;
    case 5: sw_int(s,1,5); sw_int(s,0,5); sw_int(s,0,5); sw_bit(s,0); break;
    case 6: sw_int(s,1,5); sw_int(s,0,5); sw_int(s,0,5); sw_bit(s,0); break;
    case 7: sw_int(s,0,5); sw_int(s,0,5); sw_int(s,0,3); sw_int(s,0,3); break;
    case 8: sw_bit(s,0); sw_int(s,'C',7); sw_bit(s,0); sw_int(s,'S',7); break;
    case 9: sw_int(s,1,5); sw_int(s,0,5); sw_int(s,0,5); sw_bit(s,0); break;
    case 10: sw_int(s,1,5); sw_int(s,0,5); sw_int(s,0,5); sw_bit(s,0); break;
    case 11: sw_int(s,0,5); sw_int(s,0,5); sw_int(s,0,3); sw_int(s,0,3); break;
    case 12: sw_int(s,0,16); break;
    }
    s->cur_param=(s->cur_param+1)%13;
}

//  ----------------------------------------------------------------------------
//  sis_ussn() - Nachricht 1000, Variante "Universal Short Station Name" (bis 12 Zeichen)
//    Mehrteilig, 6 Zeichen je PDU, 8-Bit-Zeichen, Flag fuer "-FM"-Suffix.
//    Quelle: [1020s] 4.8 und 4.8.1 "Universal Short Station Name".
//  ----------------------------------------------------------------------------
static void sis_ussn(sis_t *s){
    sw_int(s,8,4); int sl=strlen(s->short_name); if(sl>12) sl=12;
    int nf=(sl+5)/6; if(nf<1) nf=1;
    sw_int(s,s->ussn_cur,4); sw_bit(s,0);
    if(s->ussn_cur==0){ sw_int(s,0,3); sw_bit(s,s->fm_suffix); sw_bit(s,nf-1); } else sw_int(s,0,5);
    for(int i=s->ussn_cur*6;i<s->ussn_cur*6+6;i++) sw_int(s,i<sl?(uint8_t)s->short_name[i]:0,8);
    s->ussn_cur=(s->ussn_cur+1)%nf;
}

//  ----------------------------------------------------------------------------
//  sis_slogan() - Nachricht 1000, Variante "Station Slogan" (bis 95 Zeichen)
//    Frame 0: Laenge (7 Bit) + 5 Zeichen; Folgeframes je 6 Zeichen.
//    Quelle: [1020s] 4.8 "Universal Short Station Name / Station Slogan".
//  ----------------------------------------------------------------------------
static void sis_slogan(sis_t *s){
    sw_int(s,8,4); int sl=strlen(s->slogan); if(sl>95) sl=95;
    int nf=(sl+6)/6;
    sw_int(s,s->slogan_cur,4); sw_bit(s,1);
    if(s->slogan_cur==0){
        sw_int(s,0,3); sw_int(s,0,3); sw_int(s,sl,7);
        for(int i=0;i<5;i++) sw_int(s,i<sl?(uint8_t)s->slogan[i]:0,8);
    } else {
        sw_int(s,0,5);
        for(int i=s->slogan_cur*6-1;i<s->slogan_cur*6+5;i++) sw_int(s,i<sl?(uint8_t)s->slogan[i]:0,8);
    }
    s->slogan_cur=(s->slogan_cur+1)%nf;
}

//  ----------------------------------------------------------------------------
//  sis_init() - SIS-Zustand mit Stationsdaten fuellen
//    ALFN-Startwert 800000000 (willkuerlich, nur Zaehler; die echte ALFN waere die seit
//    dem GPS-Epoch 6.1.1980 gezaehlte Frame-Nummer, [1012s] 6.5 - sie wird nicht gesendet,
//    nur 2 Bit je PDU, deshalb genuegt hier ein Zaehler).
//    Namen mit "-FM"-Suffix werden in Suffix-Flag umgesetzt. std_short = 1, wenn der Name
//    in das 4-Zeichen/5-Bit-Format passt. Standort (Erfurt), UTC+1 und EU-Sommerzeit sind
//    Voreinstellungen; bei Bedarf hier aendern.
//  ----------------------------------------------------------------------------
static void sis_init(sis_t *s,const char *name,const char *slogan,const char *msg,const char *country,int type){
    memset(s,0,sizeof(*s));
    s->alfn=800000000; strncpy(s->country,country,2);
    strncpy(s->short_name,name,15);
    int l=strlen(s->short_name);
    if(l>=3&&!strcmp(s->short_name+l-3,"-FM")){ s->short_name[l-3]=0; s->fm_suffix=1; }
    s->std_short=(strlen(s->short_name)<=4);
    for(unsigned i=0;i<strlen(s->short_name);i++) if(!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ ?-*$",s->short_name[i])) s->std_short=0;
    strncpy(s->slogan,slogan,99); strncpy(s->message,msg,199);
    s->nprog=1; s->prog_type[0]=type;
    s->lat=50.9778f; s->lon=11.0289f; s->alt=200.0f;   // Erfurt als Default-Standort
    s->loc_high=1; s->utc_offset=60; s->dst_sched=2;    // MEZ + EU-Sommerzeit
}

// Erzeugt einen kompletten Frame (8 Blocks a 80 Bit) fuer AM
//  ----------------------------------------------------------------------------
//  sis_frame() - erzeugt die 8 SIS-PDUs (je 80 Bit) fuer einen L1-Frame
//    Je PDU:  Bit 0 = 0 (SIS-Format), Bit 1 = 0/1 (eine bzw. zwei Nachrichten),
//             Nachrichten laut Zeitplan, Auffuellen mit 0 bis Bit 64,
//             2 Bit reserviert/"Zeit nicht gesperrt", 2 Bit ALFN-Ausschnitt, 12 Bit CRC.
//    ALFN-Ausschnitt: bei ALFN mod 4 == 0 die ALFN-Bits (17+2*blk):(16+2*blk), sonst die Bits
//    (1+2*blk):(2*blk) - so sammelt der Empfaenger ueber mehrere Frames die ALFN ein.
//    Quelle: [1020s] Kapitel 4 (PDU-Format, Feld "ALFN" und "CRC Field"), Kapitel 5.
//    Ergebnis geht an l1_frame() (Kanal PIDS, [1012s] Tabelle 10-1: PIDSG 240 Bit je Block).
//  ----------------------------------------------------------------------------
static void sis_frame(sis_t *s,uint8_t out[BLOCKS_FRAME][SIS_BITS]){
    const int (*sched)[2]=s->std_short?sched_am_short:sched_am_long;
    for(int blk=0;blk<BLOCKS_FRAME;blk++){
        uint8_t *start=out[blk]; s->bit=start;
        sw_bit(s,0);                              // PIDS formatted
        int n=(sched[blk][1]>=0)?2:1;
        sw_bit(s,n==1?0:1);
        for(int k=0;k<n;k++){
            switch(sched[blk][k]){
            case S_ID:sis_station_id(s);break; case S_SHORT:sis_short(s);break; case S_LONG:sis_long(s);break;
            case S_LOC:sis_location(s);break; case S_MSG:sis_message(s);break; case S_SVC:sis_service(s);break;
            case S_PAR:sis_param(s);break; case S_USSN:sis_ussn(s);break; case S_SLOGAN:sis_slogan(s);break; }
        }
        while(s->bit<start+64) sw_bit(s,0);
        sw_bit(s,0); sw_bit(s,0);                 // reserved, time not locked
        if((s->alfn&3)==0) sw_int(s,(s->alfn>>(16+blk*2))&3,2); else sw_int(s,(s->alfn>>(blk*2))&3,2);
        sw_int(s,sis_crc12(start),12);
    }
    s->alfn++;
}

// ============================================================================
//  PSD (Program Service Data: Titel/Interpret als ID3 in HDLC-Paketen)
// ============================================================================
//  Program Service Data ([1028s] Kapitel 5 + Anhang A "ID3 Standard Reference", [1085s] PSD
//  Transport): Titel, Interpret usw. werden als ID3v2.3-Nachricht in einem HDLC-Paket
//  uebertragen. Die Bytes laufen in das Audio-Transport-PDU (siehe l2_pdu(): 8 Byte je
//  3750-Bit-PDU, 128 Byte je grossem PDU). Hier: Frames TIT2 (Titel) und TPE1 (Interpret) -
//  [1028s] Tabelle 5-1 "ID3 Frames Supported by PSD" - plus ein XHDR-Frame ohne Bild-Verknuepfung.
typedef struct {
    char title[128], artist[128]; int prog; uint16_t seq;
    uint8_t pkt[1024]; int pkt_len, pkt_off;
} psd_t;

//  ----------------------------------------------------------------------------
//  psd_text_frame() - ein ID3v2.3-Textframe: "ID" (4 Zeichen), Laenge (4 Byte, big endian),
//    2 Flag-Bytes (0), Zeichensatz-Byte 0 (ISO-8859-1), Text. Quelle: [1028s] Bild 5-1
//    "General ID3 Message Structure", Anhang A (ID3v2.3.0). Rueckgabe: Framelaenge in Bytes.
//  ----------------------------------------------------------------------------
static int psd_text_frame(uint8_t *o,const char *id,const char *d){
    int len=strlen(d)+1; memcpy(o,id,4);
    o[4]=len>>24;o[5]=len>>16;o[6]=len>>8;o[7]=len; o[8]=o[9]=o[10]=0; memcpy(o+11,d,len-1);
    return 11+len-1;
}

//  ----------------------------------------------------------------------------
//  psd_next_packet() - baut das naechste PSD-Paket und HDLC-kodiert es
//    Aufbau: Protokoll-ID 0x21, Port (0x5100 = MPS, Hauptprogramm; 0x5201..0x5207 = SPS 1-7)
//    little endian, 16-Bit-Sequenznummer, "ID3" + Version 3.0 + Flags 0 + synchsafe-Laenge
//    (7 Bit je Byte), die Frames TIT2, TPE1 und XHDR (MIME-Hash 0xBE4B7536 = Primary Image,
//    ohne Bild), danach die Endebytes 'U','F' (Konvention aus [gr-nrsc5] psd_encoder).
//    Quelle: [1028s] Kapitel 5 und Anhang A; Paketrahmen/Port [1085s] PSD Transport;
//    HDLC: hdlc_encode(). Die Bedeutung der Endebytes ist in der Norm nicht einzeln geprueft.
//  ----------------------------------------------------------------------------
static void psd_next_packet(psd_t *p){
    static const uint16_t PORT[]={0x5100,0x5201,0x5202,0x5203,0x5204,0x5205,0x5206,0x5207};
    uint8_t raw[600]; int n=0;
    raw[n++]=0x21; raw[n++]=PORT[p->prog]&0xff; raw[n++]=PORT[p->prog]>>8; raw[n++]=p->seq&0xff; raw[n++]=p->seq>>8; p->seq++;
    uint8_t pay[400]; int pl=0;
    pl+=psd_text_frame(pay+pl,"TIT2",p->title); pl+=psd_text_frame(pay+pl,"TPE1",p->artist);
    // XHDR (ohne LOT): mime PRIMARY_IMAGE 0xBE4B7536, param=1, extlen=0
    memcpy(pay+pl,"XHDR",4); pl+=4; pay[pl++]=0;pay[pl++]=0;pay[pl++]=0;pay[pl++]=6;
    pay[pl++]=0;pay[pl++]=0; pay[pl++]=0x36;pay[pl++]=0x75;pay[pl++]=0x4B;pay[pl++]=0xBE; pay[pl++]=1; pay[pl++]=0;
    raw[n++]='I';raw[n++]='D';raw[n++]='3';raw[n++]=3;raw[n++]=0;raw[n++]=0;
    raw[n++]=(pl>>21)&0x7f;raw[n++]=(pl>>14)&0x7f;raw[n++]=(pl>>7)&0x7f;raw[n++]=pl&0x7f;
    memcpy(raw+n,pay,pl); n+=pl; raw[n++]='U'; raw[n++]='F';
    p->pkt_len=hdlc_encode(raw,n,p->pkt); p->pkt_off=0;
}

//  ----------------------------------------------------------------------------
//  psd_bytes() - liefert n Bytes des endlosen PSD-Bytestroms (Pakete werden bei Bedarf
//    nachgeneriert). l2_pdu() holt je PDU psd_bytes Bytes (8 bei 3750-Bit-PDU).
//    Wird per UDP 8888 ein neuer Titel gesetzt, wird das laufende Paket verworfen
//    (pkt_off = pkt_len) und sofort ein neues erzeugt.
//  ----------------------------------------------------------------------------
static void psd_bytes(psd_t *p,uint8_t *out,int n){
    for(int i=0;i<n;i++){ if(p->pkt_off>=p->pkt_len) psd_next_packet(p); out[i]=p->pkt[p->pkt_off++]; }
}

// ============================================================================
//  HDC-Audio-Encoder (gepatchtes fdk-aac) oder Dummy-Frames
// ============================================================================
//  HDC = "High Definition Codec" (Xperi/iBiquity), der Audiocodec der HD-Radio-Digitalprogramme.
//  Der Codec ist nicht Teil der NRSC-5-Normdokumente; [1017s] "Audio Transport" legt nur fest,
//  wie die codierten Audioframes in L2-PDUs verpackt werden (siehe l2_pdu()).
//  Verwendet wird das von [gr-nrsc5] benutzte gepatchte fdk-aac (https://github.com/argilo/fdk-aac,
//  Branch hdc-encoder; Audio Object Type AOT_HDC): 44.1 kHz, mono, 2048 Samples je Frame,
//  ADTS-Verpackung. Pro L2-PDU (3750 Bit) werden 4 HDC-Frames gesendet: 4*2048 = 8192 Samples
//  = genau eine L1-Blockdauer Tb (0.1858 s). Ohne -DUSE_FDK_HDC erzeugt hdc_encode() nur
//  Zufallsdaten mit passender ADTS-Laenge (Signaltest ohne Codec).
#define HDC_SAMPLES 2048
typedef struct {
#ifdef USE_FDK_HDC
    HANDLE_AACENCODER h;
#endif
    int bitrate; int bytes_acc; uint32_t prng;
    uint8_t *buf; int len, cap;       // ADTS-Bytestrom (Ausgabe)
} hdc_t;

//  ----------------------------------------------------------------------------
//  hdc_init() - HDC-Encoder oeffnen und konfigurieren
//    AOT_HDC, 44100 Hz, MODE_1 (mono), Bitrate (Standard 17900 bit/s wie das MA1/MA3-Beispiel
//    in [gr-nrsc5] apps/hd_tx_am_*.py), Transmux 2 = ADTS, Afterburner an.
//    Der erreichbare Bitratenbereich folgt aus der PDU-Groesse: 4 Frames + Overhead muessen in
//    (3750-22)/8 = 466 Byte passen (siehe l2_pdu(), Warnung "HDC Bitrate zu hoch").
//    Rueckgabe 0 = ok, -1 = Fehler (Codec nicht verfuegbar / Parameter abgelehnt).
//  ----------------------------------------------------------------------------
static int hdc_init(hdc_t *e,int bitrate){
    memset(e,0,sizeof(*e)); e->bitrate=bitrate; e->prng=12345; e->cap=1<<16; e->buf=malloc(e->cap);
#ifdef USE_FDK_HDC
    if(aacEncOpen(&e->h,0,1)!=AACENC_OK) return -1;
    if(aacEncoder_SetParam(e->h,AACENC_AOT,AOT_HDC)!=AACENC_OK) return -1;
    aacEncoder_SetParam(e->h,AACENC_SAMPLERATE,44100);
    aacEncoder_SetParam(e->h,AACENC_CHANNELMODE,MODE_1);
    aacEncoder_SetParam(e->h,AACENC_CHANNELORDER,1);
    aacEncoder_SetParam(e->h,AACENC_BITRATE,bitrate);
    aacEncoder_SetParam(e->h,AACENC_TRANSMUX,2);
    aacEncoder_SetParam(e->h,AACENC_AFTERBURNER,1);
    if(aacEncEncode(e->h,NULL,NULL,NULL,NULL)!=AACENC_OK) return -1;
#endif
    return 0;
}

// Ein HDC-Frame (2048 Samples, mono int16) -> ADTS-Bytes an e->buf anhaengen
//  ----------------------------------------------------------------------------
//  hdc_encode() - kodiert genau einen Frame (2048 mono int16-Samples) und haengt die ADTS-
//    Bytes an den Ausgabepuffer e->buf an. Der Bytestrom wird von l2_pdu() verbraucht.
//    Ohne Codec: Dummy-ADTS-Frame (7-Byte-Header mit Laenge, Rest Xorshift-Zufall); die
//    mittlere Laenge folgt der Zielbitrate (bytes_acc als Bit-Akkumulator).
//  ----------------------------------------------------------------------------
static void hdc_encode(hdc_t *e,int16_t *pcm){
    if(e->cap-e->len<4096){ e->cap*=2; e->buf=realloc(e->buf,e->cap); }
#ifdef USE_FDK_HDC
    AACENC_BufDesc ib={0},ob={0}; AACENC_InArgs ia={0}; AACENC_OutArgs oa={0};
    int iid=IN_AUDIO_DATA, isz=HDC_SAMPLES*2, iel=2, oid=OUT_BITSTREAM_DATA, osz=4096-16, oel=1;
    void *ip=pcm,*op=e->buf+e->len;
    ib.numBufs=1; ib.bufs=&ip; ib.bufferIdentifiers=&iid; ib.bufSizes=&isz; ib.bufElSizes=&iel;
    ob.numBufs=1; ob.bufs=&op; ob.bufferIdentifiers=&oid; ob.bufSizes=&osz; ob.bufElSizes=&oel;
    ia.numInSamples=HDC_SAMPLES;
    if(aacEncEncode(e->h,&ib,&ob,&ia,&oa)==AACENC_OK) e->len+=oa.numOutBytes;
#else
    // Dummy: ADTS-Header + Pseudozufall mit Zielbitrate (nur fuer Signaltests)
    (void)pcm;
    e->bytes_acc+=e->bitrate*HDC_SAMPLES/44100;       // Bits
    int payload=e->bytes_acc/8-7; e->bytes_acc-= (payload+7)*8;
    int tot=payload+7; uint8_t *h=e->buf+e->len;
    h[0]=0xff;h[1]=0xf1;h[2]=0x4c;h[3]=0x40|((tot>>11)&3);h[4]=(tot>>3)&0xff;h[5]=((tot&7)<<5)|0x1f;h[6]=0xfc;
    for(int i=0;i<payload;i++){ e->prng^=e->prng<<13; e->prng^=e->prng>>17; e->prng^=e->prng<<5; h[7+i]=e->prng; }
    e->len+=tot;
#endif
}

// ============================================================================
//  L2: Audio-/Daten-PDU Aufbau (Port von gr-nrsc5 l2_encoder)
// ============================================================================
//  Layer 2 ([1014s] Kapitel 4 "Layer 2 Transport", Kapitel 5 "Layer 2 PDU Generation") bildet die
//  Dienste (Audio, PSD, SIS, AAS) auf die logischen Kanaele von Layer 1 ab ([1012s] 4.4: P1, P3,
//  PIDS). Dieser Sender nutzt: P1 = Audio-Transport-PDU mit HDC-Audio, PSD und kleinem Fixed Data
//  Subchannel (Hauptprogramm HD1, 3750 Bit, Rate Rb), P3 = PDU ohne Audio, nur Fuellbytes im Fixed Data Subchannel (24000 Bit
//  bei MA1, 30000 Bit bei MA3, Rate Rf). Zuordnung nach [1014s] Tabelle 5-2 "Mapping of Services/
//  Programs to Logical Channels vs AM Service Mode"; Groessen [1012s] Tabelle 7-3/7-4.
//  PDU-Struktur ([1014s] 5.1, [1017s]): die ersten 8 Bytes Reed-Solomon-Paritaet, dann Control
//  Word (6 Byte), Locator-Feld (12 oder 16 Bit je Audiopaket), HEF (3 Byte), PSD-Bytes, Audio-
//  pakete (je HDC-Frame + CRC-8). Am PDU-Ende optional Fixed Data Subchannel (Kontrollbytes +
//  Daten). Das PCI-Feld (22..24 Bit Header, Tabelle "Generic Header Sequence Indications") wird
//  nicht am Anfang, sondern ueber das PDU verteilt ("Header Spread", [1014s] Tabelle 5-4).
//  PCI-Headersequenzen (24 Bit) nach [1014s] Tabelle 5-3 "Generic Header Sequence Indications":
//    CW0_AUDIO        Audio-PDU ohne Fixed Data Subchannel (hier nicht benutzt)
//    CW2_AUDIO_FIXED  Audio-PDU mit Fixed Data Subchannel (hier: P1, 4 Byte Daten + Kontrollbytes)
//    CW4_FIXED        PDU nur mit Fixed Data Subchannel (hier: P3)
//  Bitmuster wie [gr-nrsc5] l2_encoder_impl.h (CW0_AUDIO usw.); die 22/23-Bit-Varianten ergeben
//  sich aus l2_header_spread() durch Abschneiden (PDU-Groesse mod 8).
static const uint8_t CW0_AUDIO[24]={0,0,1,1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,1,0,0,1,1};
static const uint8_t CW2_AUDIO_FIXED[24]={1,1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,1,0,0,1,1,0,0};
static const uint8_t CW4_FIXED[24]={0,0,1,1,0,1,1,0,0,0,1,1,0,1,0,0,1,1,0,0,1,1,1,0};
//  BBM: vier feste Anfangsbytes (0x7D 0x3A 0xE2 0x42) im Fixed Data Subchannel, danach
//  0x7E-Fuellbytes (HDLC-Flag, "Leerlauf"). Konvention aus [gr-nrsc5]; zugehoerige
//  Normstelle: [1014s] 5.1 (Fixed Data Subchannel). Ohne AAS-Daten wird nur gefuellt.
//  RS_CW_LEN/RS_PAR: Reed-Solomon-Codewort 96 Byte, davon 8 Byte Paritaet (siehe init_rs).
//  CW_LEN: Control Word 6 Byte. HEF_LEN: Header Expansion Field 3 Byte ([1017s]).
static const uint8_t BBM[4]={0x7d,0x3a,0xe2,0x42};
#define RS_CW_LEN 96
#define RS_PAR 8
#define CW_LEN 6
#define HEF_LEN 3

typedef struct {
    int num_progs, size, data_bytes, ccc_width, payload_bytes, total_data_width;
    int lc_bits, psd_bytes, pdu_seq_len, codec_mode, target_nop;
    int pdu_seq_no, start_seq_no, target_seq_no, partial_bytes;
    uint8_t ccc_count, ccc[32]; int ccc_len, ccc_offset, aas_block_offset;
    uint8_t out_buf[4096]; int program_type;
} l2_t;

//  ----------------------------------------------------------------------------
//  l2_init() - Parameter eines L2-Kanals
//    num_progs 1 = Audio-PDU (P1), 0 = reines Daten-PDU (P3). size = PDU-Groesse in Bit
//    (3750 / 24000 / 30000, [1012s] Tabelle 7-3/7-4). payload_bytes = (size-22)/8 (22 Bit sind
//    Header/PCI). ccc/data_bytes: Fixed-Data-Subchannel (Laenge 2048 Byte fuer P3, 4 Byte
//    beim Audiokanal; der Laengen-Kontrollsatz wird als HDLC-Paket "ccc" zyklisch gesendet).
//    Kleine PDUs (<9000 Bit): 12-Bit-Locator, 4 Audiopakete je PDU (target_nop), 8 Byte PSD,
//    codec_mode 13, PDU-Sequenz 0..7; grosse PDUs: 16-Bit-Locator, 32 Pakete, 128 Byte PSD.
//    codec_mode 13 ist der Codec-Modus der AM-P1-Audio-PDUs (Feld "Codec Mode" im Control Word,
//    [1017s]); der Wert stammt aus [gr-nrsc5] und ist durch Empfang bestaetigt.
//  ----------------------------------------------------------------------------
static void l2_init(l2_t *l,int num_progs,int size,int data_bytes,int ccc_width,int ptype){
    memset(l,0,sizeof(*l));
    l->num_progs=num_progs; l->size=size; l->data_bytes=data_bytes; l->ccc_width=ccc_width; l->program_type=ptype;
    l->payload_bytes=(size-22)/8;
    uint8_t c[5]={0,0,0,data_bytes&0xff,data_bytes>>8};
    l->ccc_len=hdlc_encode(c,5,l->ccc); l->ccc_offset=l->ccc_len-1;
    l->total_data_width=(data_bytes>0)?(data_bytes+ccc_width+1):0;
    if(size>=9000){ l->target_nop=32; l->lc_bits=16; l->psd_bytes=128; l->pdu_seq_len=2; l->codec_mode=0; }
    else          { l->target_nop=4;  l->lc_bits=12; l->psd_bytes=8;   l->pdu_seq_len=8; l->codec_mode=13; }
}

//  l2_len_loc(): Laenge des Locator-Feldes in Bytes fuer nop Audiopakete: (lc_bits*nop+4)/8.
//  adts_len(): Nutzdatenlaenge eines HDC-Frames aus dem 13-Bit-Laengenfeld des ADTS-Headers
//  (Bytes 3..5) minus 7 Byte Header.
static int l2_len_loc(l2_t *l,int nop){ return ((l->lc_bits*nop)+4)/8; }

//  adts_len() - Nutzdatenlaenge eines HDC-Frames: 13-Bit-Feld "frame_length" des ADTS-Headers
//    (Bits 1:0 von Byte 3, Byte 4, Bits 7:5 von Byte 5) minus 7 Byte Header. Der ADTS-Header
//    selbst wird nicht uebertragen; [1017s] Audio Transport transportiert nur die Rohframes.
static int adts_len(const uint8_t *h){ return ((h[3]&3)<<11|(h[4]<<3)|(h[5]>>5))-7; }

//  ----------------------------------------------------------------------------
//  l2_write_loc() - schreibt den i-ten Locator (Endposition des Audiopakets im PDU).
//    12-Bit-Locatoren werden paarweise in 3 Bytes gepackt (Byte0 = Bits 7:0 von loc0, Byte1 =
//    Bits 11:8 von loc0 + Bits 3:0 von loc1 << 4, Byte2 = Bits 11:4 von loc1); 16-Bit-
//    Locatoren little endian. Quelle: [1017s] "Audio Packet Locators" (Aufbau des Locator-
//    Feldes); Packungsschema wie [gr-nrsc5] l2_encoder::write_locator.
//  ----------------------------------------------------------------------------
static void l2_write_loc(l2_t *l,uint8_t *o,int i,int loc){
    if(l->lc_bits==16){ o[i*2]=loc&0xff; o[i*2+1]=loc>>8; }
    else if(i%2==0){ o[i/2*3]=loc&0xff; o[i/2*3+1]=loc>>8; }
    else { o[i/2*3+1]|=((loc&0xf)<<4); o[i/2*3+2]=loc>>4; }
}

//  ----------------------------------------------------------------------------
//  l2_header_spread() - verteilt die PCI-Headerbits ueber das PDU ("Header Spread")
//    Die 22 bis 24 Headerbits (hb) werden im Abstand n_offset+1 Bit ab Position n_start in
//    den PDU-Bitstrom eingefuegt. hb haengt von size mod 8 ab: 0 -> 24, 7 -> 23, sonst 22.
//    Fuer size < 72000: n_start = 120, n_offset = 8*INT(((size-120+7)/8)/hb) - 1.
//    Fuer size >= 72000 gelten andere Werte (Tabelle). Zusaetzlich werden die Datenbytes MSB zuerst
//    in den Bitstrom ausgegeben, so wie L1 sie (nach reverse_bytes) erwartet.
//    Quelle: [1014s] Tabelle 5-4 "Header Spread Parameters" (n_start, n_offset je PDU-Groesse).
//  ----------------------------------------------------------------------------
static void l2_header_spread(l2_t *l,const uint8_t *in,uint8_t *out,const uint8_t *pci){
    int size=l->size,n_start,n_offset,hb;
    if(size>=72000){
        n_start=8*((size-30000+7)/8);
        switch(size%8){ case 0:n_offset=1247;hb=24;break; case 7:n_offset=1303;hb=23;break; default:n_offset=1359;hb=22; }
    } else {
        n_start=120;
        switch(size%8){ case 0:hb=24;break; case 7:hb=23;break; default:hb=22; }
        n_offset=8*(((size-120+7)/8)/hb)-1;
    }
    int oo=0,po=0;
    for(int i=0;i<l->payload_bytes;i++) for(int j=0;j<8;j++){
        if(oo>=n_start && po<hb && ((oo-n_start)%(n_offset+1)==0)) out[oo++]=pci[po++];
        out[oo++]=(in[i]>>(7-j))&1;
    }
}

// Erzeugt ein PDU (size Bits, je Bit ein Byte). hdc: ADTS-Bytestrom, *used = verbrauchte Bytes
//  ----------------------------------------------------------------------------
//  l2_pdu() - baut ein komplettes L2-PDU (size Bit, je Bit ein Byte im Array out)
//    Eingabe : hdc/avail = ADTS-Bytestrom der HDC-Frames; psd = PSD-Quelle.
//    Ausgabe : *used = verbrauchte Bytes des HDC-Stroms; out[0..size-1] = Bits.
//
//    Audio-PDU (num_progs > 0), Bytepositionen im Nutzlastpuffer out_buf:
//      [0..7]   Reed-Solomon-Paritaet ueber [8..95]
//      [8..13]  Control Word (6 Byte, [1017s] "Audio Transport Control Word"):
//                 Byte0: Bits 7:6 PDU-Sequenz (1:0), Bits 3:0 Codec-Mode (13)
//                 Byte1: Blend-Control (2 = ENABLE) und PDU-Sequenz Bit 2
//                 Byte2: Latenz Bits 1:0, Common Delay (24)
//                 Byte3: Start-Sequenznummer Bits 4:0, Flags "partial last" / "partial first", Latenz Bit 2
//                 Byte4: HEF-vorhanden-Bit, Anzahl Pakete (nop), Start-Seq-Nr. Bit 5
//                 Byte5: la_loc = Position des letzten PSD-Bytes (Beginn der Audiopakete - 1)
//      Locatoren (12/16 Bit je Paket), dann HEF (3 Byte: u.a. Programmtyp 8 Bit),
//      PSD-Bytes (8), danach die Audiopakete: je HDC-Frame ohne ADTS-Header + CRC-8.
//    Ein Audiopaket darf ueber die PDU-Grenze laufen (partial_bytes): der Rest steht am
//    Anfang des Folge-PDUs ("pf"/"pl"-Flags). Die Zahl der Pakete je PDU richtet sich nach
//    dem Platz (bytes_left) und der Sequenz (target_seq_no - start_seq_no).
//    Die Konstanten Blend=ENABLE, Common Delay 24, Latenz 4 sind die Werte aus dem
//    gr-nrsc5-Flowgraph fuer Programm 0; sie steuern das Ueberblenden Analog<->Digital im
//    Empfaenger (Anzeige im nrsc5-Log: "blend: 2, delay: 96, latency: 8").
//
//    Daten-Subchannel (data_bytes > 0): letztes Byte = Zaehler/Breite des Kontrollfeldes,
//    davor das zyklisch wiederholte HDLC-Paket "ccc" (Laengenangabe), davor die Daten
//    (hier nur BBM + 0x7E-Fuellbytes). Quelle: [1014s] 5.1 "Layer 2 PDU Structure and Content",
//    [1017s] Audio Transport, Locator/HEF-Struktur; Bitbelegung wie [gr-nrsc5] l2_encoder und durch
//    Dekodierung (nrsc5, Sangean HDR-1) bestaetigt.
//  ----------------------------------------------------------------------------
static void l2_pdu(l2_t *l,const uint8_t *hdc,int avail,int *used,psd_t *psd,uint8_t *out){
    memset(l->out_buf,0,l->payload_bytes);
    uint8_t *op=l->out_buf; int hdc_off=0;
    l->target_seq_no+=l->target_nop;
    if(l->num_progs>0){
        int bytes_left=(l->out_buf+l->payload_bytes-l->total_data_width)-op;
        int nop=0,off=hdc_off,audio_length=0,begin_bytes=0,end_bytes=0;
        if(l->partial_bytes){ nop++; audio_length=l->partial_bytes+1; off+=l->partial_bytes; }
        while(nop<l->target_seq_no-l->start_seq_no){
            if(off+7>avail) break;
            int length=adts_len(hdc+off); off+=7;
            if(off+length>avail) break;
            off+=length;
            if(RS_PAR+CW_LEN+l2_len_loc(l,nop+1)+HEF_LEN+l->psd_bytes+audio_length+2>bytes_left) break;
            if(RS_PAR+CW_LEN+l2_len_loc(l,nop+1)+HEF_LEN+l->psd_bytes+audio_length+length+1>bytes_left){
                begin_bytes=bytes_left-(RS_PAR+CW_LEN+l2_len_loc(l,nop+1)+HEF_LEN+l->psd_bytes+audio_length+1);
                end_bytes=length-begin_bytes; nop++; break;
            }
            nop++; audio_length+=length+1;
        }
        int la_loc=RS_PAR+CW_LEN+l2_len_loc(l,nop)+HEF_LEN+l->psd_bytes-1;
        uint8_t *cw=op+RS_PAR;
        int pf=l->partial_bytes?1:0, pl=begin_bytes?1:0, blend=2, cdelay=24, lat=4;
        cw[0]=((l->pdu_seq_no&3)<<6)|(0<<4)|l->codec_mode;
        cw[1]=(0<<3)|(blend<<1)|(l->pdu_seq_no>>2);
        cw[2]=((lat&3)<<6)|cdelay;
        cw[3]=((l->start_seq_no&0x1f)<<3)|(pl<<2)|(pf<<1)|(lat>>2);
        cw[4]=(1<<7)|(nop<<1)|((l->start_seq_no&0x20)>>5);
        cw[5]=la_loc;
        int end=la_loc;
        for(int i=0;i<nop;i++){
            int length;
            if(i==0&&l->partial_bytes) length=l->partial_bytes;
            else if(i==nop-1&&begin_bytes){ length=begin_bytes; hdc_off+=7; l->start_seq_no++; }
            else { length=adts_len(hdc+hdc_off); hdc_off+=7; l->start_seq_no++; }
            uint8_t crc=0xff;
            for(int j=0;j<length;j++){ crc=crc8_tab[crc^hdc[hdc_off]]; op[++end]=hdc[hdc_off++]; }
            op[++end]=crc;
            l2_write_loc(l,op+RS_PAR+CW_LEN,i,end);
        }
        l->partial_bytes=end_bytes;
        uint8_t *hef=op+RS_PAR+CW_LEN+l2_len_loc(l,nop);
        hef[0]=0x90|(0<<1); hef[1]=0xA0|(0<<3)|(l->program_type>>7); hef[2]=l->program_type&0x7f;
        psd_bytes(psd,op+RS_PAR+CW_LEN+l2_len_loc(l,nop)+HEF_LEN,l->psd_bytes);
        // Reed-Solomon ueber out_program[95..8] (absteigend), Parity nach out_program[7..0]
        // Reed-Solomon ueber die Bytes 95..8 (hoechster Index zuerst, wie encode_rs_char in gr-nrsc5); Paritaet nach Byte 7..0.
        uint8_t data[RS_CW_LEN-RS_PAR],par[8];
        for(int i=RS_CW_LEN-1,k=0;i>=RS_PAR;i--,k++) data[k]=op[i];
        rs_encode(data,RS_CW_LEN-RS_PAR,par);
        for(int j=0;j<8;j++) op[7-j]=par[j];
        if(l->target_seq_no-l->start_seq_no>8) fprintf(stderr,"HDC Bitrate zu hoch\n");
    }
    // Fixed Data Subchannel am PDU-Ende: Zaehler-/Breitenbyte, CCC-Bytes, Daten (BBM + 0x7E-Fuellung).
    if(l->data_bytes>0){
        if((l->ccc_count&3)==0) l->out_buf[l->payload_bytes-1]=l->ccc_count;
        else l->out_buf[l->payload_bytes-1]=(l->ccc_width==1)?0x00:(((l->ccc_width/2)<<4)|(l->ccc_width/2));
        l->ccc_count++;
        for(int i=l->payload_bytes-1-l->ccc_width;i<l->payload_bytes-1;i++){
            l->out_buf[i]=l->ccc[l->ccc_offset]; l->ccc_offset=(l->ccc_offset+1)%l->ccc_len;
        }
        for(int i=l->payload_bytes-1-l->ccc_width-l->data_bytes;i<l->payload_bytes-1-l->ccc_width;i++){
            l->out_buf[i]=(l->aas_block_offset<4)?BBM[l->aas_block_offset]:0x7e;
            l->aas_block_offset=(l->aas_block_offset+1)%(255+4);
        }
    }
    // PCI-Headersequenz waehlen: reine Daten -> CW4, Audio mit Datenkanal -> CW2, Audio ohne Datenkanal -> CW0 ([1014s] Tabelle 5-3).
    const uint8_t *pci;
    if(l->num_progs==0) pci=CW4_FIXED; else pci=(l->data_bytes>0)?CW2_AUDIO_FIXED:CW0_AUDIO;
    l2_header_spread(l,l->out_buf,out,pci);
    l->pdu_seq_no=(l->pdu_seq_no+1)%l->pdu_seq_len;
    *used=hdc_off;
}

// ============================================================================
//  L1: Scrambler, Faltungscodierer, Interleaver, OFDM-Mapping (Port l1_am_encoder)
// ============================================================================
//  Layer 1 AM ([1012s]). Verarbeitungskette je L1-Frame (1.486 s):
//    Scrambling (8) -> Kanalcodierung (9) -> Interleaving (10) -> System Control (11)
//    -> OFDM-Subtraegermapping (12) -> OFDM-Signalerzeugung (13) -> Transmission (14)
//  Logische Kanaele ([1012s] 4.4, Tabellen 7-3/7-4):
//    P1   3750 Bit, Rate Rb (8 je Frame), Codierung E1, Primaerbaender
//    P3   MA1: 24000 Bit, E2, Rate Rf (Sekundaer/Tertiaer)   MA3: 30000 Bit, E1
//    PIDS 80 Bit, Rate Rb, E3 (SIS)
//  Relative Robustheit nach Tabelle 7-3 (MA1): P1 5, P3 6 (PL hoch) bzw. 8 (PL niedrig), PIDS 3 bzw. 7;
//  Tabelle 7-4 (MA3): P1 1, P3 4, PIDS 2 (1 = robusteste Stufe). Latenz: P1 Tf+Tdd, P3 Tf (MA1)
//  bzw. Tf+Tdd (MA3), PIDS Tb.
//  Ausgabe von l1_frame() sind die Interleaver-Matrizen PU, PL, S, T, PIDS (je 25 Spalten x
//  256 Zeilen, PIDS 2 Spalten; [1012s] 10.3). l1_map_symbol() entnimmt je Symbol eine Zeile.
//  In l1_t stehen die Zwischenpuffer; bl/bu/ebl/ebu enthalten zusaetzlich DIV_DELAY Bit, die
//  den digitalen Diversity-Delay realisieren.
//  Teilrahmen-Zuordnung (Subframe Generation, [1012s] 10.2, Bilder 10-4 bis 10-6):
//  Der codierte Strom P1G wird je 12 Bit in vier Teilstroeme (BL, ML, BU, MU) zu je 3 Bit aufgeteilt;
//  die Zahlen sind die Bitpositionen (index MOD 12) im 12er-Block:
//    BL = 2,1,5     ML = 11,6,7     BU = 10,8,9     MU = 4,3,0        (je 18000 Bit je Frame)
//  BL/BU = "Backup", um Tdd(+TT1a) verzoegert; ML/MU = "Main", unverzoegert.
//  P3 MA1 (Bild 10-4): index MOD 6:  EL = 0,1 (12000 Bit)   EU = 2,3,5,4 (24000 Bit)
//  P3 MA3 (Bild 10-6): wie P1: EBL, EML, EBU, EMU mit denselben Positionen.
//  PIDS (Bild 10-5, index MOD 24):  IL = 0,1,12,13,6,5,18,17,11,7,23,19   IU = 2,4,14,16,3,8,15,20,9,10,21,22.
static const int bl_delay[]={2,1,5}, ml_delay[]={11,6,7}, bu_delay[]={10,8,9}, mu_delay[]={4,3,0};
static const int el_delay[]={0,1}, eu_delay[]={2,3,5,4};
static const int pids_il_delay[]={0,1,12,13,6,5,18,17,11,7,23,19};
static const int pids_iu_delay[]={2,4,14,16,3,8,15,20,9,10,21,22};

typedef struct {
    int sm,rdb,hpp,pl,aab;
    int p1_bits,p3_bits;
    uint8_t buf[30000];
    uint8_t pids_g[SIS_BITS*3], p1_g[72000], p3_g[72000];
    uint8_t bl[18000+DIV_DELAY],ml[18000],bu[18000+DIV_DELAY],mu[18000],el[12000],eu[24000];
    uint8_t ebl[18000+DIV_DELAY],eml[18000],ebu[18000+DIV_DELAY],emu[18000];
    uint8_t parity[512], sc_symbols[SYMS_FRAME];
    uint8_t pu_m[25][SYMS_FRAME],pl_m[25][SYMS_FRAME],s_m[25][SYMS_FRAME],t_m[25][SYMS_FRAME],pids_m[2][SYMS_FRAME];
    int32_t ch_scale[256];     // Q(QF-1): X = (Konstellation in Halbeinheiten) * ch_scale
} l1_t;

//  ----------------------------------------------------------------------------
//  l1_sc_seq() - System Control Data Sequence (32 Bit) eines L1-Blocks, wird auf die
//    Referenzsubtraeger (+-1) gesendet. Quelle: [1012s] 11.2, Bild 11-2, Tabelle 11-1.
//    Reihenfolge o[0] .. o[31] = Bit 31 .. Bit 0 der Tabelle (links = zuerst gesendet):
//      o[0..6]   Sync10:4 = 0110010          (11.2.1)
//      o[7]      PLI Power Level Indicator   (11.2.2)  o[8] Parity3 = gerade Paritaet von PLI
//      o[9]      Sync3 = 1                   o[10] Reserved4 = 0
//      o[11]     HPPI High-Power PIDS Ind.   (11.2.3)  o[12] AABI Analog Audio BW (11.2.4)
//      o[13]     Parity2 (Reserved4, HPPI, AABI)       o[14] Sync2 = 0
//      o[15]     RDBI Reduced Digital BW     (11.2.5)  o[16] Reserved3 = 0
//      o[17..19] BC2:0 L1-Blockzaehler       (11.2.6)  o[20] Parity1 (RDBI, Reserved3, BC)
//      o[21..22] Sync1:0 = 11                o[23..25] Reserved2:0 = 0
//      o[26..30] SMI4:0 Service Mode Indicator (11.2.7, Tabelle 11-3: MA1=00001, MA3=00010)
//      o[31]     Parity0 (Reserved2:0 und SMI)
//    Jede Paritaet ist "gerade Paritaet" = XOR der geschuetzten Bits.
//    Der Aufrufer (l1_init) erzeugt 8 Sequenzen (BC = 0..7), zusammen R mit 256 Bit; je OFDM-
//    Symbol wird ein Bit auf die Referenzsubtraeger gelegt ([1012s] 12.2.3, Tabelle 12-12).
//  ----------------------------------------------------------------------------
static void l1_sc_seq(uint8_t *o,int pli,int hppi,int aabi,int rdbi,int bc,int smi){
    o[0]=0;o[1]=1;o[2]=1;o[3]=0;o[4]=0;o[5]=1;o[6]=0;o[7]=pli;o[8]=o[7];o[9]=1;
    o[10]=0;o[11]=hppi;o[12]=aabi;o[13]=o[10]^o[11]^o[12];o[14]=0;
    o[15]=rdbi;o[16]=0;o[17]=(bc&4)>>2;o[18]=(bc&2)>>1;o[19]=bc&1;o[20]=o[15]^o[16]^o[17]^o[18]^o[19];
    o[21]=1;o[22]=1;o[23]=0;o[24]=0;o[25]=0;o[26]=(smi&0x10)>>4;o[27]=(smi&8)>>3;o[28]=(smi&4)>>2;o[29]=(smi&2)>>1;o[30]=smi&1;
    o[31]=o[23]^o[24]^o[25]^o[26]^o[27]^o[28]^o[29]^o[30];
}

//  ----------------------------------------------------------------------------
//  l1_channel_power() - Skalierung (Amplitudenfaktor) jedes der 256 Subtraeger
//    ch_scale[i] gehoert zum Subtraeger k = i-128 und ist 10^(dB/20) in Q(QF-1) (das "-1"
//    weil die Konstellationspunkte in Halbeinheiten gespeichert sind, siehe l1_map_symbol()).
//    dB = gewuenschter Pegel des Subtraegers relativ zum unmodulierten Traeger (dBc) minus
//    Mittelleistung der Konstellation (64-QAM 10.2119 dB, 16-QAM 3.9794 dB, QPSK -3.0103 dB,
//    BPSK -6.0206 dB, bezogen auf Punktkoordinaten +-0.5, +-1.5, ... wie in [1012s] Tabellen
//    12-1, 12-4, 12-5, 12-10). So hat jeder Subtraeger im Mittel genau den dBc-Wert der Norm.
//    Symbolische Faktoren der Norm: [1012s] Tabellen 5-1, 5-2, 6-4, 6-5, 12-11 (CHPU, CHPL, CHS1/2,
//    CHT1/2, CHI1..5, CHB bzw. CDP, CDE, CDB, CDI1/2). Zahlenwerte (dBc) aus [1082s] 4.10.1.x
//    bzw. [gr-nrsc5] l1_am_encoder::set_channel_power:
//
//    MA1 (Hybrid), Subtraegernummern k ([1012s] Tabelle 5-1):
//      Primaer oben/unten  +-57..81      -30 dBc je Subtraeger      ([1082s] 4.10.1.5)
//      Sekundaer           +-28..52      PL=0: -43 dBc, PL=1: -37 dBc  (6.3.1)  ([1082s] 4.10.1.3)
//      Tertiaer            +-2..26       PL=1: -44 dBc; PL=0: -44-0.5*c dBc (c<12), danach -50 dBc
//                                        (CHT1[0:24]/CHT2[0:24], jeder Subtraeger eigener Faktor)
//      Referenz            +-1           -26 dBc BPSK                ([1082s] 4.10.1.1)
//      PIDS1               +-27          wie Sekundaer (PL)          ([1082s] 4.10.1.3)
//      PIDS2               +-53          relativ zu Primaer oben/unten (CHPU/CHPL * CHI3/4/5):
//                                        -13 dB (PL=0), -7 dB (PL=1), 0 dB bei HPP=1 oder RDB=1
//    MA3 (All Digital), ([1012s] Tabelle 5-2, 6-5):
//      Primaer             +-2..26       -15 dBc (CDP)               ([1082s] 4.10.1.6)
//      Sekundaer/Tertiaer  +28..52 / -28..-52   -30 dBc (CDE)        ([1082s] 4.10.1.4)
//      Referenz            +-1           -15 dBc (CDB)               ([1082s] 4.10.1.1)
//      PIDS                +-27          -15 dBc plus -15 dB (CDI1) bzw. 0 dB bei HPP=1/RDB=1 (CDI2)
//    Bei RDB=1 sind Sekundaer/Tertiaer (und bei MA1 PIDS1) ausgeschaltet (scale 0, [1012s] 6.3.2).
//    Subtraeger +-54..56 (MA1) werden nicht gesendet ([1012s] 5.3, Schutz des ersten Nachbarkanals).
//    Die Pegel von MA1/MA3 wurden mit dem Hardware-Empfaenger Sangean HDR-1 bestaetigt.
//  ----------------------------------------------------------------------------
static void l1_channel_power(l1_t *l){
    const double qam64_p=10.211893,qam16_p=3.979400,qpsk_p=-3.010300,bpsk_p=-6.020600;
    double db[256]; for(int i=0;i<256;i++) db[i]=-1e9;
    if(l->sm==1){
        double pu=-30,pl=-30,s=(l->pl?-37:-43),ref=-26,p1=(l->pl?-37:-43),p2d,t[25];
        for(int c=0;c<25;c++) t[c]=l->pl?-44:(c<12?-44-0.5*c:-50);
        p2d=(l->rdb||l->hpp)?0:(l->pl?-7:-13);
        for(int c=0;c<25;c++){
            db[128+57+c]=pu-qam64_p; db[128-57-c]=pl-qam64_p;
            db[128+28+c]=s-qam16_p;  db[128-28-c]=s-qam16_p;
            db[128+2+c]=t[c]-qpsk_p; db[128-2-c]=t[c]-qpsk_p;
        }
        db[128+1]=db[128-1]=ref-bpsk_p; db[128+27]=db[128-27]=p1-qam16_p;
        db[128+53]=pu+p2d-qam16_p; db[128-53]=pl+p2d-qam16_p;
    } else {
        double p=-15,e=-30,ref=-15,pd=(l->rdb||l->hpp)?0:-15;
        for(int c=0;c<25;c++){
            db[128+2+c]=p-qam64_p; db[128-2-c]=p-qam64_p; db[128+28+c]=e-qam64_p; db[128-28-c]=e-qam64_p;
        }
        db[128+1]=db[128-1]=ref-bpsk_p; db[128+27]=db[128-27]=p+pd-qam16_p;
    }
    for(int i=0;i<256;i++) l->ch_scale[i]=(db[i]<-1e8)?0:(int32_t)lround(pow(10,db[i]/20)*(double)(1<<(QF-1)));
}

//  ----------------------------------------------------------------------------
//  l1_init() - Betriebsart festlegen: sm = 1 (MA1) oder 3 (MA3, intern; SMI-Wert = 2),
//    rdb/hpp/pl/aab = Steuersignale RDB, HPP, PL, AAB ([1012s] 6.3, 6.4).
//    Initialisiert die Paritaetstabelle (fuer den Faltungscodierer), die 8 System-Control-
//    Sequenzen (BC = 0..7) und die Subtraegerskalierung.
//    Frame-Groessen: P1 3750 Bit; P3 24000 (MA1) / 30000 (MA3) Bit ([1012s] Tabellen 7-3/7-4).
//  ----------------------------------------------------------------------------
static void l1_init(l1_t *l,int sm,int rdb,int hpp,int pl,int aab){
    memset(l,0,sizeof(*l)); l->sm=sm;l->rdb=rdb;l->hpp=hpp;l->pl=pl;l->aab=aab;
    l->p1_bits=3750; l->p3_bits=(sm==1)?24000:30000;
    for(int i=0;i<512;i++){ int t=i; l->parity[i]=0; while(t){ l->parity[i]^=1; t&=t-1; } }
    for(int bc=0;bc<BLOCKS_FRAME;bc++) l1_sc_seq(l->sc_symbols+bc*32,pl,hpp,aab,rdb,bc,sm==1?1:2);
    l1_channel_power(l);
}

//  ----------------------------------------------------------------------------
//  l1_reverse_bytes() - kehrt die Bitreihenfolge innerhalb jedes 8-Bit-Blocks um (LSB zuerst)
//    L2 (l2_header_spread) liefert die PDU-Bits MSB zuerst je Byte; L1 verarbeitet je Byte LSB
//    zuerst. Konvention uebernommen aus [gr-nrsc5] l1_am_encoder::reverse_bytes; die Normstelle
//    ([1012s] 3.3 Darstellungskonventionen: linkes Bit = zuerst, Bit 0 = LSB) wurde nicht einzeln
//    hergeleitet - die Reihenfolge ist durch Dekodierung (nrsc5, Sangean HDR-1) bestaetigt.
//  ----------------------------------------------------------------------------
static void l1_reverse_bytes(const uint8_t *in,uint8_t *out,int len){
    for(int off=0;off<len;off+=8){ int bits=(len-off<8)?len-off:8; for(int i=0;i<bits;i++) out[off+i]=in[off+bits-1-i]; }
}

//  ----------------------------------------------------------------------------
//  l1_scramble() - Scrambler: XOR mit einer maximal langen Folge
//    LFSR mit primitivem Polynom P(x) = 1 + x^2 + x^11, bei jedem neuen Transfer Frame auf den
//    Zustand 0111 1111 111 zurueckgesetzt (hier 0x3FF mit 11 Bit). Das erste Bit wird mit dem
//    Ausgangsbit des Anfangszustands verknuepft, danach Schieben. Quelle: [1012s] 8.1
//    "Scrambler Operation", Bild 8-2; drei identische Scrambler (P1, P3, PIDS), Bild 8-1.
//  ----------------------------------------------------------------------------
static void l1_scramble(uint8_t *b,int len){
    // Scrambler-Anfangszustand 0111 1111 111 ([1012s] 8.1), bei jedem Transfer Frame neu.
    unsigned reg=0x3ff;
    for(int o=0;o<len;o++){ uint8_t nb=((reg>>9)^reg)&1; b[o]^=nb; reg=(reg>>1)|(nb<<10); }
}

//  ----------------------------------------------------------------------------
//  l1_conv() - Faltungscodierer, Constraint Length 9, Mutter-Code Rate 1/3, punktiert
//    Quelle: [1012s] 9.1 und Tabelle 9-1:
//      E1  Rate 5/12  Polynome (oktal) 561, 657, 711   (Tabelle 9-2)  Puncture-Periode 5
//          Ausgabefolge g1,0 g3,0 g1,1 g3,1 g1,2 g3,2 g1,3 g2,3 g3,3 g1,4 g2,4 g3,4   (Bild 9-2)
//          -> g2 nur in den letzten zwei von fuenf Spalten (io MOD 5 >= 3)
//      E2  Rate 2/3   Polynome 561, 753, 711           (Tabelle 9-3)  Puncture-Periode 2
//          Ausgabefolge g1,0 g3,0 g1,1 g1,2 g3,2 g1,3 ...                                (Bild 9-3)
//          -> g1 immer, g3 nur bei geradem Index, g2 nie
//      E3  Rate 1/3   Polynome 561, 753, 711           (Tabelle 9-4)  nicht punktiert
//    Wichtig (9.1.4): "Die letzten 8 Bit eines Transfer Frames werden benutzt, um die Verzoge-
//    rungselemente des Codierers zu initialisieren" - tail-biting: das Schieberegister wird
//    mit den Bits len-8..len-1 vorbelegt (siehe reg-Initialisierung).
//    mode: 1 = E1, 2 = E2, 3 = E3 (wie l1_enc_pdu).
//  ----------------------------------------------------------------------------
static void l1_conv(l1_t *l,int mode,const uint8_t *in,uint8_t *out,int len){
    static const unsigned pe1[3]={0561,0657,0711}, pe2[3]={0561,0753,0711};
    const unsigned *poly=(mode==1)?pe1:pe2;     // E1 -> pe1, E2/E3 -> pe2
    // Anfangszustand des Faltungscodierers: die letzten 8 Bit des Transfer Frames ([1012s] 9.1.4, tail-biting).
    unsigned reg=(in[len-8]<<1)|(in[len-7]<<2)|(in[len-6]<<3)|(in[len-5]<<4)|(in[len-4]<<5)|(in[len-3]<<6)|(in[len-2]<<7)|(in[len-1]<<8);
    int oo=0;
    for(int io=0;io<len;io++){
        reg=(reg>>1)|(in[io]<<8);
        for(int i=0;i<3;i++){
            int use;
            if(mode==1) use=(i==0)||(i==2)||(io%5>=3);
            else if(mode==2) use=(i==0)||((i==2)&&(io%2==0));
            else use=1;
            if(use) out[oo++]=l->parity[reg&poly[i]];
        }
    }
}

// mode: 1=E1, 2=E2, 3=E3
//  ----------------------------------------------------------------------------
//  l1_enc_pdu() - Scrambling + Kanalcodierung eines Transfer Frames (Bit je Byte)
//    Zuordnung ([1012s] 9.2, Bild 9-5 / 9-6):
//      MA1: P1 -> E1, P3 -> E2, PIDS -> E3        MA3: P1 -> E1, P3 -> E1, PIDS -> E3
//    Reihenfolge: Bytes umdrehen -> scramblen -> falten (siehe oben).
//  ----------------------------------------------------------------------------
static void l1_enc_pdu(l1_t *l,int mode,const uint8_t *in,uint8_t *out,int len){
    l1_reverse_bytes(in,l->buf,len); l1_scramble(l->buf,len); l1_conv(l,mode,l->buf,out,len);
}

//  ----------------------------------------------------------------------------
//  bit_map() - setzt "bits" in Interleaver-Matrix m im Block b an die Stelle k
//    Zeile und Spalte nach [1012s] 10.3.1 (PU, PL, S, T):
//      Column(k) = (9*k) MOD 25
//      Row(k)    = [11*((9*k) MOD 25) + 16*INT(k/25) + 11*INT(k/50)] MOD 32
//    mit k = 0..749 fuer Daten, k = 750..799 fuer die 50 Trainingselemente
//    (Tabelle 10-5). Ein Block = 32 Zeilen, 8 Bloecke = 256 Zeilen je Matrix;
//    die Matrix wird hier als m[Spalte][b*32+Zeile] gespeichert. bits wird an der
//    Bitposition p im Element (Tabelle 10-4: 6/4/2 Bit je Element) per ODER eingetragen.
//  ----------------------------------------------------------------------------
static inline void bit_map(uint8_t m[25][SYMS_FRAME],int b,int k,int bits){
    int col=(9*k)%25; int row=(11*col+16*(k/25)+11*(k/50))%32;
    m[col][b*32+row]|=bits;
}

//  ----------------------------------------------------------------------------
//  l1_interleaver_pids() - PIDS-Interleaver ([1012s] 10.3.2, Bild 10-3, Bild 10-5)
//    Die 240 codierten Bit eines PIDS-Blocks werden mit den Mustern IL/IU (index MOD 24) in je
//    120 Bit aufgeteilt (Bild 10-5) und auf die Matrixspalten 0 (IL) und 1 (IU) abgebildet:
//      Row(k) = [11*(k + INT(k/15)) + 3] MOD 32,  k = 0..29
//      IL: k = [n + INT(n/60) + 11] MOD 30         IU: k = [n + INT(n/60)] MOD 30
//      Bitposition p = n MOD 4, n = 0..119 (4 Bit je Element, Tabelle 10-4)
//    Die zwei verbleibenden Zeilen 8 und 24 tragen die Trainingsbits 1001 (= 9, Tabelle 10-5).
//    Arbeitet je L1-Block (kein Frame-Puffer): "Interleaver Depth" = Block, Tabelle 10-3.
//  ----------------------------------------------------------------------------
static void l1_interleaver_pids(l1_t *l,const uint8_t *in,int block){
    uint8_t il[120],iu[120]; int off=block*32;
    memset(l->pids_m[0]+off,0,32); memset(l->pids_m[1]+off,0,32);
    for(int i=0;i<10;i++) for(int j=0;j<12;j++){ il[i*12+j]=in[i*24+pids_il_delay[j]]; iu[i*12+j]=in[i*24+pids_iu_delay[j]]; }
    for(int n=0;n<120;n++){
        int p=n%4,k,row;
        k=(n+(n/60)+11)%30; row=(11*(k+(k/15))+3)%32; l->pids_m[0][off+row]|=(il[n]<<p);
        k=(n+(n/60))%30;    row=(11*(k+(k/15))+3)%32; l->pids_m[1][off+row]|=(iu[n]<<p);
    }
    l->pids_m[0][off+8]=9; l->pids_m[0][off+24]=9; l->pids_m[1][off+8]=9; l->pids_m[1][off+24]=9;
}

//  ----------------------------------------------------------------------------
//  l1_interleaver() - Subframe-Erzeugung, Diversity-Delay und Bit-Mapping fuer P1/P3
//    Eingang: p1_g (8 Bloecke * 9000 Bit), p3_g. Ausgang: Matrizen pu_m, pl_m, s_m, t_m.
//    1) Subframe Generation ([1012s] 10.2): je 12 Bit von P1G werden auf BL, ML, BU, MU verteilt
//       (bl_delay[] usw.); P3: MA1 -> EL/EU (je 6 Bit), MA3 -> EBL/EML/EBU/EMU.
//    2) Diversity Delay: BL, BU (und bei MA3 EBL, EBU) liegen in Puffern mit DIV_DELAY = 3*18000
//       Bit Vorlauf, d.h. sie erscheinen genau Tdd = 3 Frames spaeter (Tabelle 10-3). Der Zusatz
//       TT1a ([1012s] 10.1: "Transmit Time Alignment", justiert den Abstand zwischen Main- und
//       Backup-Strom auf exakt Tdd) ist hier 0, weil das L2 in ganzen Frames liefert.
//       Am Ende werden die letzten DIV_DELAY Bit an den Pufferanfang geschoben (memmove).
//    3) Bit Mapping (Bild 10-4 MA1 / Bild 10-6 MA3, mit n = 0..17999 je Subframe):
//         PL: BL  b = INT(n/2250)            k = [n + INT(n/750) + 1] MOD 750     p = n MOD 3
//             ML  b = (3n+3) MOD 8           k = [n + INT(n/3000) + 3] MOD 750    p = 3 + (n MOD 3)
//         PU: BU  b = INT(n/2250)            k = [n + INT(n/750)] MOD 750         p = n MOD 3
//             MU  b = (3n) MOD 8             k = [n + INT(n/3000) + 2] MOD 750    p = 3 + (n MOD 3)
//       MA1 P3 (n bis 11999 bzw. 23999):
//         T:  EL  b = [3n + INT(n/3000)] MOD 8                    k = [n + INT(n/6000)] MOD 750  p = n MOD 2
//         S:  EU  b = [3n + INT(n/3000) + 2*INT(n/12000)] MOD 8   k = [n + INT(n/6000)] MOD 750  p = n MOD 4
//       MA3 P3 (Bild 10-6):
//         T:  EBL b = (3n+3) MOD 8   k = [n+INT(n/3000)+3] MOD 750  p = n MOD 3
//             EML b = (3n+3) MOD 8   k = [n+INT(n/3000)+3] MOD 750  p = 3 + (n MOD 3)
//         S:  EBU b = (3n) MOD 8     k = [n+INT(n/3000)+2] MOD 750  p = n MOD 3
//             EMU b = (3n) MOD 8     k = [n+INT(n/3000)+2] MOD 750  p = 3 + (n MOD 3)
//    4) Trainingselemente k = 750..799 aller Bloecke ([1012s] Tabelle 10-5):
//         MA1: PU, PL = 100101 (0x25), S = 1001 (0x9), T = 10 (0x2)       MA3: alle 100101 (0x25)
//    Bei RDB = 1 werden S und T nicht belegt (nur Primaer + PIDS).
//  ----------------------------------------------------------------------------
static void l1_interleaver(l1_t *l){
    int rdb=l->rdb;
    memset(l->pu_m,0,sizeof(l->pu_m)); memset(l->pl_m,0,sizeof(l->pl_m));
    if(!rdb){ memset(l->s_m,0,sizeof(l->s_m)); memset(l->t_m,0,sizeof(l->t_m)); }
    for(int i=0;i<6000;i++) for(int j=0;j<3;j++){
        l->bl[DIV_DELAY+i*3+j]=l->p1_g[i*12+bl_delay[j]]; l->ml[i*3+j]=l->p1_g[i*12+ml_delay[j]];
        l->bu[DIV_DELAY+i*3+j]=l->p1_g[i*12+bu_delay[j]]; l->mu[i*3+j]=l->p1_g[i*12+mu_delay[j]];
        if(l->sm==3&&!rdb){
            l->ebl[DIV_DELAY+i*3+j]=l->p3_g[i*12+bl_delay[j]]; l->eml[i*3+j]=l->p3_g[i*12+ml_delay[j]];
            l->ebu[DIV_DELAY+i*3+j]=l->p3_g[i*12+bu_delay[j]]; l->emu[i*3+j]=l->p3_g[i*12+mu_delay[j]];
        }
    }
    if(l->sm==1&&!rdb) for(int i=0;i<6000;i++){
        for(int j=0;j<2;j++) l->el[i*2+j]=l->p3_g[i*6+el_delay[j]];
        for(int j=0;j<4;j++) l->eu[i*4+j]=l->p3_g[i*6+eu_delay[j]];
    }
    // Bit-Mapping aller Subframes, Gleichungen siehe Kopfkommentar ([1012s] Bild 10-4/10-6): b = Block, k = Index im Block, p = Bitposition.
    int b,k,p;
    for(int n=0;n<18000;n++){
        b=n/2250; k=(n+n/750+1)%750; p=n%3;  bit_map(l->pl_m,b,k,l->bl[n]<<p);
        b=(3*n+3)%8; k=(n+n/3000+3)%750; p=3+(n%3); bit_map(l->pl_m,b,k,l->ml[n]<<p);
        b=n/2250; k=(n+n/750)%750; p=n%3;    bit_map(l->pu_m,b,k,l->bu[n]<<p);
        b=(3*n)%8; k=(n+n/3000+2)%750; p=3+(n%3); bit_map(l->pu_m,b,k,l->mu[n]<<p);
        if(l->sm==3&&!rdb){
            b=(3*n+3)%8; k=(n+n/3000+3)%750; p=n%3;   bit_map(l->t_m,b,k,l->ebl[n]<<p);
            b=(3*n+3)%8; k=(n+n/3000+3)%750; p=3+(n%3); bit_map(l->t_m,b,k,l->eml[n]<<p);
            b=(3*n)%8; k=(n+n/3000+2)%750; p=n%3;     bit_map(l->s_m,b,k,l->ebu[n]<<p);
            b=(3*n)%8; k=(n+n/3000+2)%750; p=3+(n%3); bit_map(l->s_m,b,k,l->emu[n]<<p);
        }
    }
    if(l->sm==1&&!rdb){
        for(int n=0;n<12000;n++){ b=(3*n+n/3000)%8; k=(n+(n/6000))%750; p=n%2; bit_map(l->t_m,b,k,l->el[n]<<p); }
        for(int n=0;n<24000;n++){ b=(3*n+n/3000+2*(n/12000))%8; k=(n+(n/6000))%750; p=n%4; bit_map(l->s_m,b,k,l->eu[n]<<p); }
    }
    // Trainingselemente (k = 750..799) jedes Blocks mit dem Muster aus [1012s] Tabelle 10-5.
    for(int blk=0;blk<BLOCKS_FRAME;blk++) for(int kk=750;kk<800;kk++){
        bit_map(l->pu_m,blk,kk,0x25); bit_map(l->pl_m,blk,kk,0x25);
        if(!rdb){
            if(l->sm==1){ bit_map(l->s_m,blk,kk,0x9); bit_map(l->t_m,blk,kk,0x2); }
            else { bit_map(l->s_m,blk,kk,0x25); bit_map(l->t_m,blk,kk,0x25); }
        }
    }
    // Diversity-Delay: die letzten DIV_DELAY Bit (3 Frames) werden fuer die naechsten Frames an den Anfang geschoben ([1012s] Tabelle 10-3).
    memmove(l->bl,l->bl+18000,DIV_DELAY); memmove(l->bu,l->bu+18000,DIV_DELAY);
    if(l->sm==3&&!rdb){ memmove(l->ebl,l->ebl+18000,DIV_DELAY); memmove(l->ebu,l->ebu+18000,DIV_DELAY); }
}

// Ein L1-Frame: pids[8][80], p1[8*3750] (Bits), p3 (Bits, nur wenn !rdb)
//  ----------------------------------------------------------------------------
//  l1_frame() - verarbeitet einen kompletten L1-Frame (1.486 s) aus den L2-PDUs
//    Eingang: pids[8][80] (SIS), p1[8*3750] (Audio-PDUs), p3 (ein PDU, nicht bei RDB=1).
//    Ausgang: Interleaver-Matrizen in l1_t, bereit fuer l1_map_symbol().
//    Eingangsgroessen der Interleaver (Tabellen 10-1/10-2): P1G 8 x 9000 Bit (3750*12/5),
//    P3G 36000 Bit (MA1, E2: 24000*3/2) bzw. 72000 Bit (MA3, E1: 30000*12/5), PIDSG 8 x 240 Bit.
//  ----------------------------------------------------------------------------
static void l1_frame(l1_t *l,uint8_t pids[BLOCKS_FRAME][SIS_BITS],const uint8_t *p1,const uint8_t *p3){
    for(int blk=0;blk<BLOCKS_FRAME;blk++){
        l1_enc_pdu(l,1,p1+blk*l->p1_bits,l->p1_g+blk*l->p1_bits*12/5,l->p1_bits);
        l1_enc_pdu(l,3,pids[blk],l->pids_g,SIS_BITS);
        l1_interleaver_pids(l,l->pids_g,blk);
    }
    if(l->sm==1){ if(!l->rdb) l1_enc_pdu(l,2,p3,l->p3_g,l->p3_bits); }
    else        { if(!l->rdb) l1_enc_pdu(l,1,p3,l->p3_g,l->p3_bits); }
    l1_interleaver(l);
}

// Mapping eines Symbols auf 4096er IFFT-Eingang (Q22), Xr/Xi muessen vorher genullt sein/werden hier genullt
//  ----------------------------------------------------------------------------
//  l1_map_symbol() - OFDM-Subtraegermapping eines Symbols s (0..255) -> Vektor X (4096 Bins)
//    Je Symbol wird eine Zeile jeder Matrix gelesen ([1012s] 12.1, Bild 12-1/12-2), in
//    Konstellationspunkte gewandelt, skaliert (ch_scale, Q22) und auf die Subtraeger gelegt.
//    Konstellationen, Punkte in Halbeinheiten gespeichert (+-1 = +-0.5):
//      64-QAM  Tabelle 12-1  (re = L[idx & 7], im = L[idx >> 3], L = -3.5, 3.5, -0.5, 0.5, -2.5, 2.5, -1.5, 1.5)
//      16-QAM  Tabelle 12-5  (L = -1.5, 1.5, -0.5, 0.5)       QPSK  Tabelle 12-4 (+-0.5, +-0.5)
//      BPSK    Tabelle 12-10 (0, -0.5) fuer 0 und (0, +0.5) fuer 1  (Referenzsubtraeger)
//    Zuordnung (MA1, [1012s] Tabelle 12-2/12-6/12-7; MA3 Tabelle 12-3/12-8/12-9):
//      MA1: PU = +57..81, PL = -57..-81 (64-QAM, unabhaengige Daten); S = +-28..52 (16-QAM,
//           gleiche Daten oben/unten); T = +-2..26 (QPSK, gleiche Daten oben/unten);
//           PIDS1 = +-27 (inner), PIDS2 = +-53 (outer, 16-QAM)
//      MA3: PU = +2..26, PL = -2..-26 (64-QAM); S = +28..52, T = -28..-52 (64-QAM);
//           PIDS = -27 (inner, p0) und +27 (p1)
//    Untere Seitenbaender tragen das konjugierte Symbol mit Vorzeichenwechsel des Realteils:
//    -conj(a + jb) = -a + jb (Stern "*" in Tabelle 5-1). Bei den Paaren mit GLEICHEN Daten
//    (S, T bei MA1 sowie die Referenzen) ergibt das im Zeitbereich ein rein imaginaeres
//    (Quadratur-)Signal, das den in-phase liegenden Analogtraeger nicht stoert: das ist die
//    Grundlage der Abwaertskompatibilitaet des Hybridsignals (Interpretation des Autors).
//    Referenzsubtraeger +-1 (Tabelle 12-12): Bit sc_symbols[s] des System-Control-Vektors R als BPSK.
//  ----------------------------------------------------------------------------
static void l1_map_symbol(l1_t *l,int s,int32_t *Xr,int32_t *Xi){
    int re[256]={0},im[256]={0};
    #define QAM64(I,R,M) do{ static const int8_t L[8]={-7,7,-1,1,-5,5,-3,3}; R=L[(I)&7]; M=L[(I)>>3]; }while(0)
    #define QAM16(I,R,M) do{ static const int8_t L[4]={-3,3,-1,1}; R=L[(I)&3]; M=L[(I)>>2]; }while(0)
    int r,m;
    for(int col=0;col<25;col++){
        if(l->sm==1){
            QAM64(l->pl_m[col][s],r,m); re[128-57-col]=-r; im[128-57-col]=m;
            QAM64(l->pu_m[col][s],r,m); re[128+57+col]=r;  im[128+57+col]=m;
            if(!l->rdb){
                int t=l->t_m[col][s]; int tr=(t&1)?1:-1, tm=(t&2)?1:-1;
                re[128+2+col]=tr; im[128+2+col]=tm; re[128-2-col]=-tr; im[128-2-col]=tm;
                QAM16(l->s_m[col][s],r,m); re[128+28+col]=r; im[128+28+col]=m; re[128-28-col]=-r; im[128-28-col]=m;
            }
        } else {
            QAM64(l->pl_m[col][s],r,m); re[128-2-col]=-r; im[128-2-col]=m;
            QAM64(l->pu_m[col][s],r,m); re[128+2+col]=r;  im[128+2+col]=m;
            if(!l->rdb){
                QAM64(l->t_m[col][s],r,m); re[128-28-col]=-r; im[128-28-col]=m;
                QAM64(l->s_m[col][s],r,m); re[128+28+col]=r;  im[128+28+col]=m;
            }
        }
    }
    int p0r,p0m,p1r,p1m;
    QAM16(l->pids_m[0][s],p0r,p0m); QAM16(l->pids_m[1][s],p1r,p1m);
    if(l->sm==1){
        if(!l->rdb){ re[128-27]=-p0r; im[128-27]=p0m; re[128+27]=p0r; im[128+27]=p0m; }
        re[128-53]=-p1r; im[128-53]=p1m; re[128+53]=p1r; im[128+53]=p1m;
    } else {
        re[128-27]=-p0r; im[128-27]=p0m; re[128+27]=p1r; im[128+27]=p1m;
    }
    // Referenzsubtraeger +-1: BPSK-Bit sc_symbols[s] des System-Control-Vektors R ([1012s] 11.2, Tabelle 12-10/12-12); gleiches Symbol auf +1 und -1 -> reine Quadraturkomponente.
    int sc=l->sc_symbols[s]; re[127]=re[129]=0; im[127]=im[129]=sc?1:-1;   // BPSK (0,+-0.5) in Halbeinheiten
    for(int i=0;i<256;i++){
        int idx=(i-128)&(NFFT-1);
        Xr[idx]=re[i]*l->ch_scale[i]; Xi[idx]=im[i]*l->ch_scale[i];
    }
    #undef QAM64
    #undef QAM16
}

// ============================================================================
//  Tabellen-Initialisierung (nur beim Start, Float erlaubt): NCO, FFT, Pulsformung
// ============================================================================
//  Alles, was hier mit Float/libm berechnet wird, geschieht einmalig beim Start (Tabellen). Die
//  Signalverarbeitung bei 5 MSPS nutzt danach ausschliesslich ganzzahlige Operationen.
static int32_t fft30_cos[NFFT/2], fft30_sin[NFFT/2];

//  ----------------------------------------------------------------------------
//  init_tables() - einmalige Tabellenberechnung
//    1) sine_lut[]: Sinus, 2^14 Stuetzstellen, Q15 (NCO).
//    2) fft30_cos/sin[]: Drehfaktoren e^(+j*2*pi*k/4096), Q30, k = 0..2047 (IFFT).
//    3) fft_rev[]: Bitumkehr-Permutation fuer 12 Bit.
//    4) pf_win[]: Pulsformungsfenster fuer die OFDM-Symbole.
//       Quelle: [1012s] 13.2 "Functionality", Bild 13-2 "Pulse Shaping Function". Die Formel ist
//       hier aus dem Kommentar von [gr-nrsc5] am_pulse_shaper_impl.h uebernommen (dort als
//       512-Werte-Tabelle bei 256 Samples je T):
//          T = 1/df (nutzbare Symboldauer), Ts = (1+alpha)*T, alpha = 7/128
//          H(t) = 1                                     fuer |t| <= (1-alpha)*T/2
//          H(t) = 0.5*(1 + cos(pi/(2*alpha) * (2|t|/T + alpha - 1)))   bis |t| <= (1+alpha)*T/2
//          H(t) = 0                                     sonst
//          G(t) = Gauss mit sigma = Ts/90, Flaeche 1   (exp(-4050*(t/Ts)^2)*90/(Ts*sqrt(2*pi)))
//          w(t) = sqrt( (H * G)(t) )                    (Faltung; hier numerisch mit 801 Stuetzstellen)
//       Abgetastet bei t_j = (j-4096)/4096 * T, j = 0..8191 (Mitte j = 4096 entspricht t = 0).
//       Ueberpruefung: gegen die 512-Werte-Referenztabelle (jeder 16. Wert) betraegt die
//       Abweichung < 1e-6 fuer Werte > 0.1; maximal 0.004 am aeussersten Fensterrand (dort ist
//       der Referenzwert selbst numerisch ungenau, ca. 0.0006).
//       Die Fensterhaelften ueberlappen sich im Bereich des Cyclic Prefix (siehe gen_block()).
//  ----------------------------------------------------------------------------
static void init_tables(void){
    for(int i=0;i<LUT_SIZE;i++) sine_lut[i]=(int16_t)lround(sin(2.0*M_PI*i/LUT_SIZE)*32767.0);
    for(int k=0;k<NFFT/2;k++){
        fft30_cos[k]=(int32_t)lround(cos(2.0*M_PI*k/NFFT)*1073741824.0);
        fft30_sin[k]=(int32_t)lround(sin(2.0*M_PI*k/NFFT)*1073741824.0);
    }
    for(int i=0;i<NFFT;i++){ int r=0; for(int b=0;b<12;b++) if(i&(1<<b)) r|=1<<(11-b); fft_rev[i]=r; }
    // Pulsformung nach NRSC-5 (1012s): sqrt( H * G ), 16-fach ueberabgetastet
    const double alpha=7.0/128.0, df=1488375.0/8192.0, T=1.0/df, Ts=(1.0+alpha)/df, sig=Ts/90.0;
    enum { NG=801 }; static double gw[NG], gt[NG]; double gs=0;
    for(int i=0;i<NG;i++){ gt[i]=(-6.0+12.0*i/(NG-1))*sig; gw[i]=exp(-gt[i]*gt[i]/(2*sig*sig)); gs+=gw[i]; }
    for(int j=0;j<2*NFFT;j++){
        double tt=((double)j-NFFT)/NFFT*T, acc=0;
        for(int i=0;i<NG;i++){
            double x=fabs(tt-gt[i]), h;
            if(x<=(1-alpha)/2*T) h=1; else if(x<=(1+alpha)/2*T) h=0.5*(1+cos(M_PI/(2*alpha)*(2*x/T+alpha-1))); else h=0;
            acc+=h*gw[i];
        }
        double v=sqrt(acc/gs); if(v>1) v=1;
        pf_win[j]=(int16_t)lround(v*32767.0);
    }
}

// In-place IFFT 4096, Eingang Q22, Ausgang unnormiert (Summe der Traeger), Twiddles Q30
//  ----------------------------------------------------------------------------
//  ifft4096() - ganzzahlige inverse FFT, 4096 Punkte, Radix-2 (Decimation in Time)
//    x[n] = SUM_k X[k] * e^(+j*2*pi*k*n/4096), ohne 1/N-Normierung (wie fft_vcc "reverse" in
//    [gr-nrsc5]; entspricht der Summe der Subtraeger, [1012s] 13.2). Eingang Q22 (Traeger = 2^22),
//    Ausgang im selben Format. Drehfaktoren Q30, Produkte in int64, Rundung vor dem Shift.
//    Ueberlaufsicherheit: die Summe aller Subtraegeramplituden bleibt unter ca. 3*10^8 < 2^31
//    (MA3: 100 Primaer-Subtraeger mit je max. 0.27 plus Rest).
//    Subtraeger k steht auf Bin (k mod 4096). Da df gleich dem Subtraegerabstand der 256-Punkt-
//    Norm-IFFT ist, liefert die 4096-Punkt-IFFT exakt die 16-fach interpolierte Wellenform
//    desselben Symbols (Zeitabstand 1/(4096*df) = 1/744187.5 s).
//  ----------------------------------------------------------------------------
static void ifft4096(int32_t *re,int32_t *im){
    for(int i=0;i<NFFT;i++){ int j=fft_rev[i]; if(j>i){ int32_t t=re[i];re[i]=re[j];re[j]=t; t=im[i];im[i]=im[j];im[j]=t; } }
    for(int len=2;len<=NFFT;len<<=1){
        int half=len>>1, step=NFFT/len;
        for(int i=0;i<NFFT;i+=len) for(int k=0;k<half;k++){
            int64_t wr=fft30_cos[k*step], wi=fft30_sin[k*step];
            int64_t xr=re[i+k+half], xi=im[i+k+half];
            int32_t tr=(int32_t)((xr*wr-xi*wi+(1LL<<29))>>30), ti=(int32_t)((xr*wi+xi*wr+(1LL<<29))>>30);
            re[i+k+half]=re[i+k]-tr; im[i+k+half]=im[i+k]-ti;
            re[i+k]+=tr; im[i+k]+=ti;
        }
    }
}

// ============================================================================
//  Sender-Struktur (ein HD-Radio-Sender = ein Traeger)
// ============================================================================
//  Je HD-Radio-Sender gibt es eine Tx-Struktur mit allen Zustandsgroessen:
//    Audio-Ring (UDP-Empfang oder Test-/Dateiquelle), digitaler Pfad (SIS, PSD, L2, HDC, L1),
//    die beiden IFFT-Ergebnisse A/B (aktuelles und naechstes Symbol, siehe gen_block()),
//    Analogpfad (Tiefpass, Polyphasen-Interpolator, AGC), Basisband-FIFO (bbI/bbQ) und RF-Zustand
//    (Farrow-Koeffizienten, Interpolationsphase frac, NCO-Phase phase).
//  Zeitliche Architektur: Die Hauptschleife (main) fordert RF-Samples an (tx_render); sobald das
//  Basisband-FIFO leer laeuft, erzeugt gen_block() das naechste OFDM-Symbol (5.8 ms Signal); sobald
//  ein Symbol ein neues L1-Frame beginnt (sym == 0), laeuft build_frame() (HDC, L2, L1).
//  Der Takt kommt damit allein vom Abnehmer (TCP-Backpressure von fl2k_tcp), nicht von einer Uhr.
//  Analog-Pfad-Konstanten: AN_BLK = 441 Audio-Samples = 10 ms Blockverarbeitung; LPF_TAPS = Laenge
//  des Audio-Tiefpasses; PP_L/PP_M = Interpolationsverhaeltnis 135/8: 44100 * 135/8 = 744187.5 Hz
//  (Audio-Abtastrate -> Basisband-Abtastrate des OFDM-Pfads); PP_K = Taps je Polyphase.
#define AN_BLK   441          // Analog-Verarbeitungsblock (10 ms @ 44.1 kHz)
#define LPF_TAPS 255
#define PP_L     135          // 744187.5 / 44100 = 135/8
#define PP_M     8
#define PP_K     16
typedef struct {
    // Konfiguration
    int port, sm, rdb, hpp, pl, aab; double freq; float md, fixed_gain; int src_kind; // 0 UDP,1 Generator,2 Datei
    float ext_gain; double analog_delay_s; int bitrate;
    // Audio-Ring (int16, 44.1 kHz, mono)
    int sock; int16_t *ring; volatile uint64_t head; FILE *src_file; uint64_t gen_n; uint64_t file_pos;
    // Digitaler Pfad
    l1_t *l1; sis_t sis; psd_t psd; l2_t l2a,l2b; hdc_t hdc;
    uint8_t pids[BLOCKS_FRAME][SIS_BITS]; uint8_t *p1bits,*p3bits;
    int sym; uint64_t dpos; int d_started; uint64_t frames_built, underruns;
    int32_t *Ar,*Ai,*Br,*Bi,*Xr,*Xi;
    // Analoger Pfad
    int32_t lpf[LPF_TAPS]; int16_t hx[LPF_TAPS*2]; int hxp; int32_t pp[PP_L][PP_K];
    int32_t xa[4096]; uint64_t a_rel; uint64_t a_filled; int a_ph; uint64_t a_na; int a_started;
    float agc_gain, agc_peak, last_peak;
    // Basisband-FIFO und RF
    int32_t *bbI,*bbQ; uint64_t bw, br;
    int64_t c_i[4], c_q[4]; uint32_t frac, frac_inc, phase, phase_inc; int bb_primed;
    // Stats
    float cur_mod;
} Tx;

// ----------------------------------------------------------------- Audio-Quellen
//  audio_gen() - interner Testton (Sender mit Port 0 ohne Datei): 440 Hz + 1 kHz (langsam
//    amplitudenmoduliert) + 2.5 kHz (an/aus alle 2 s), je mit kleiner Amplitude. Nur fuer Tests.
static void audio_gen(Tx *t,int n){
    for(int i=0;i<n;i++){
        double tt=(double)t->gen_n/44100.0;
        double v=0.22*sin(2*M_PI*440*tt)+0.22*sin(2*M_PI*1000*tt)*(0.6+0.4*sin(2*M_PI*0.5*tt))+0.1*sin(2*M_PI*2500*tt)*(sin(2*M_PI*0.25*tt)>0);
        t->ring[(t->head+i)&(AUD_RING-1)]=(int16_t)lround(v*32767);
        t->gen_n++;
    }
}

//  audio_fill_to() - fuellt den Audio-Ring bei Datei-/Testquellen "on demand" bis Position upto
//    (die Quellen laufen so schneller als Echtzeit, wenn in eine Datei geschrieben wird). Bei UDP-
//    Quellen tut die Funktion nichts: dort schreibt audio_receiver() den Ring.
static void audio_fill_to(Tx *t,uint64_t upto){
    while(t->src_kind!=0 && t->head<upto){
        int n=4096;
        if(t->src_kind==1) audio_gen(t,n);
        else {
            int16_t tmp[4096]; size_t got=fread(tmp,2,n,t->src_file);
            if(got<(size_t)n){ rewind(t->src_file); size_t g2=fread(tmp+got,2,n-got,t->src_file); got+=g2; if(got==0){ memset(tmp,0,sizeof(tmp)); got=n; } }
            for(size_t i=0;i<got;i++) t->ring[(t->head+i)&(AUD_RING-1)]=tmp[i];
            n=got;
        }
        __atomic_store_n(&t->head,t->head+n,__ATOMIC_RELEASE);
    }
}

//  ring_head() - liest den Schreibzeiger des Audio-Rings mit Acquire-Semantik (Gegenstueck zum
//    Release-Store in audio_receiver/audio_fill_to); Zaehler zaehlt Samples seit Start.
static uint64_t ring_head(Tx *t){ return __atomic_load_n(&t->head,__ATOMIC_ACQUIRE); }

// Holt 65536 Samples fuer einen L1-Frame; Rueckgabe 1 = echte Daten, 0 = Stille
//  ----------------------------------------------------------------------------
//  fetch_frame_audio() - holt 65536 Samples (= ein L1-Frame, 1.486 s) fuer den digitalen Pfad
//    dpos ist der Leseindex des digitalen Pfads im Ring (Zaehler seit Start der Audioquelle).
//    Nur wenn mindestens ein volles Frame vorliegt, wird gelesen und dpos vorgerueckt; sonst
//    wird Stille geliefert und dpos NICHT vorgerueckt (Underrun, "Audio-Underruns" im Log).
//    Folge: Beim Start laeuft der digitale Pfad zunaechst mit Stille, bis ca. 1.5 s Audio
//    gepuffert sind; im Beharrungszustand liegt der Rueckstand zwischen einem und zwei Frames.
//    Bei mehr als 3 Frames Rueckstand (Quelle schneller als Echtzeit) wird auf 2 Frames
//    aufgeholt (Latenzbegrenzung, Audio wird verworfen).
//  ----------------------------------------------------------------------------
static int fetch_frame_audio(Tx *t,int16_t *dst){
    audio_fill_to(t,t->dpos+FRAME_AUD+4096);
    uint64_t h=ring_head(t);
    // Underrun: noch kein volles Frame im Ring -> Stille senden und dpos nicht vorruecken.
    if(h==0||h<t->dpos+FRAME_AUD){ memset(dst,0,FRAME_AUD*2); if(t->src_kind==0&&h>0) t->underruns++; return 0; }
    if(t->src_kind==0 && h-t->dpos>3*FRAME_AUD) t->dpos=h-2*FRAME_AUD;          // zu viel Rueckstand: aufholen
    for(int i=0;i<FRAME_AUD;i++) dst[i]=t->ring[(t->dpos+i)&(AUD_RING-1)];
    t->dpos+=FRAME_AUD; return 1;
}

// ----------------------------------------------------------------- L2/L1 Frame
//  ----------------------------------------------------------------------------
//  build_frame() - erzeugt alle Daten fuer einen L1-Frame (1.486 s) und startet L1
//    1. 65536 Audio-Samples holen und in 32 HDC-Frames (je 2048 Samples) kodieren.
//    2. SIS: 8 PDUs a 80 Bit (sis_frame) -> Kanal PIDS ([1012s] Tabelle 7-3).
//    3. P1: 8 Audio-PDUs a 3750 Bit (eines je L1-Block, Rate Rb); jedes nimmt ca. 4 HDC-Frames auf
//       (4*2048 Samples = 8192 = ein Block). Nicht verbrauchte HDC-Bytes bleiben im Puffer.
//    4. P3: ein PDU je Frame (24000 Bit MA1 / 30000 Bit MA3, Rate Rf), nur Fuelldaten; entfaellt
//       bei RDB = 1 ([1012s] 6.3.2: S/T ausgeschaltet).
//    5. l1_frame(): Scrambling, Codierung, Interleaving -> Matrizen fuer die 256 Symbole.
//    Latenz: Dieser Frame wird zu dem Zeitpunkt berechnet, in dem das erste Symbol benoetigt
//    wird; die tatsaechliche Aussendung des Audios liegt dadurch um Frame-Interleaver und
//    Diversity-Delay ([1012s] 7.2.2, Tabelle 7-2: Tf bzw. Tf + Tdd) hinter der Eingabe.
//  ----------------------------------------------------------------------------
static void build_frame(Tx *t){
    static int16_t pcm[FRAME_AUD];
    fetch_frame_audio(t,pcm);
    for(int f=0;f<FRAME_AUD/HDC_SAMPLES;f++) hdc_encode(&t->hdc,pcm+f*HDC_SAMPLES);
    sis_frame(&t->sis,t->pids);
    for(int b=0;b<BLOCKS_FRAME;b++){
        int used; l2_pdu(&t->l2a,t->hdc.buf,t->hdc.len,&used,&t->psd,t->p1bits+b*3750);
        memmove(t->hdc.buf,t->hdc.buf+used,t->hdc.len-used); t->hdc.len-=used;
    }
    if(!t->rdb){ int u; l2_pdu(&t->l2b,NULL,0,&u,&t->psd,t->p3bits); }
    l1_frame(t->l1,t->pids,t->p1bits,t->p3bits);
    t->frames_built++;
}

// Erzeugt die IFFT des naechsten OFDM-Symbols in (re,im) als Q15
//  ----------------------------------------------------------------------------
//  next_symbol_ifft() - erzeugt die IFFT (Q15, 4096 Werte komplex) des naechsten OFDM-Symbols
//    Zaehlt die Symbole 0..255; bei Symbol 0 wird zuerst build_frame() aufgerufen.
//    Ablauf: Vektor X nullen -> l1_map_symbol() (Subtraegermapping, Q22) -> ifft4096() ->
//    Umrechnung Q22 -> Q15 (Rundung, Division durch 2^7). Das Ergebnis ist die unfensterte
//    periodische Symbolwellenform; die Fensterung/Ueberlappung geschieht in gen_block().
//  ----------------------------------------------------------------------------
static void next_symbol_ifft(Tx *t,int32_t *re,int32_t *im){
    // Beginn eines neuen L1-Frames: alle Daten fuer die naechsten 256 Symbole berechnen.
    if(t->sym==0) build_frame(t);
    memset(t->Xr,0,NFFT*4); memset(t->Xi,0,NFFT*4);
    l1_map_symbol(t->l1,t->sym,t->Xr,t->Xi);
    t->sym=(t->sym+1)%SYMS_FRAME;
    ifft4096(t->Xr,t->Xi);
    for(int i=0;i<NFFT;i++){ re[i]=(t->Xr[i]+(1<<(QF-QB-1)))>>(QF-QB); im[i]=(t->Xi[i]+(1<<(QF-QB-1)))>>(QF-QB); }
}

// ----------------------------------------------------------------- Analogpfad (nur MA1)
//  ----------------------------------------------------------------------------
//  analog_design() - Filterentwurf des Analogpfads (einmalig, Float erlaubt)
//    1) Audio-Tiefpass, 255 Taps, Kaiser-Fenster (beta 6.5), Grenzfrequenz 4.5 kHz (AAB = 0,
//       5 kHz Audiobandbreite) bzw. 7.5 kHz (AAB = 1, 8 kHz); Verstaerkung 1 bei 0 Hz. Der
//       Tiefpass haelt die Analogseitenbaender aus den Digitalbereichen heraus
//       ([1012s] 6.4 Analog Audio Bandwidth Control, 14.2.2 Low-Pass Filtering).
//    2) Polyphasen-Interpolator 135/8: Prototypfilter mit 135*16 Taps auf dem 5.95-MHz-Raster
//       (Kaiser beta 7, Grenzfrequenz 19 kHz), je Phase auf Gleichanteil 1 normiert. Er hebt die
//       Audiorate 44.1 kHz auf die Basisbandrate 744187.5 kHz (Faktor 135/8 = 16.875).
//       Die Gruppenlaufzeit (8 Eingangssamples) ist ohne Bedeutung, da sie nur in die
//       einstellbare Analogverzoegerung eingeht.
//  ----------------------------------------------------------------------------
static void analog_design(Tx *t){
    double fc=t->aab?7500.0:4500.0;                       // Audio-Bandbreite 8 bzw. 5 kHz (NRSC-5 AAB)
    #define KAISER_I0(x) ({ double s=1,tm=1; for(int k=1;k<40;k++){ tm*=((x)/(2.0*k))*((x)/(2.0*k)); s+=tm; } s; })
    double sum=0, h[LPF_TAPS]; int M=LPF_TAPS-1; double beta=6.5;
    for(int n=0;n<LPF_TAPS;n++){
        double x=n-M/2.0, w=KAISER_I0(beta*sqrt(1-pow(2.0*n/M-1,2))/1.0)/KAISER_I0(beta);
        double s=(x==0)?2*fc/44100.0:sin(2*M_PI*fc*x/44100.0)/(M_PI*x);
        h[n]=s*w; sum+=h[n];
    }
    for(int n=0;n<LPF_TAPS;n++) t->lpf[n]=(int32_t)lround(h[n]/sum*32768.0);
    // Polyphasen-Interpolator 44.1 kHz -> 744.1875 kHz (135/8)
    int N=PP_L*PP_K; double c=(N-1)/2.0, fcp=19000.0, Fh=44100.0*PP_L, ps[PP_L]={0}; double *hp=malloc(sizeof(double)*N);
    for(int n=0;n<N;n++){
        double x=n-c, w=KAISER_I0(7.0*sqrt(1-pow(2.0*n/(N-1)-1,2)))/KAISER_I0(7.0);
        hp[n]=(2*fcp/Fh)*PP_L*((x==0)?1.0:sin(2*M_PI*fcp*x/Fh)/(2*M_PI*fcp*x/Fh))*w;
        ps[n%PP_L]+=hp[n];
    }
    for(int p=0;p<PP_L;p++) for(int k=0;k<PP_K;k++) t->pp[p][k]=(int32_t)lround(hp[k*PP_L+p]/ps[p]*32768.0);
    free(hp);
}

//  ----------------------------------------------------------------------------
//  analog_block() - verarbeitet 441 Audio-Samples (10 ms) des Analogpfads
//    a) Verzoegerung: gelesen wird ring[a_rel - Da], Da = analog_delay_s * 44100. Das ist der
//       "Analog Diversity Delay" Tad ([1012s] 14.2.3, Wert 3.5: Tad = 4.5*Tf = 6.687 s). Standard
//       hier 5.5 s (Wert aus dem gr-nrsc5-Flowgraph); fuer exakt normgerechtes Blending
//       -D 6.687 setzen. Davor/ohne Daten wird Stille gelesen.
//    b) Audio-Tiefpass (FIR 255 Taps, Q15) siehe analog_design().
//    c) AGC (Float, langsam): Spitzenwert mit Zeitkonstante ca. 2 s, Verstaerkung auf 0.9
//       Vollausschlag geregelt, Glaettung ca. 5 s, Bereich 0.1 .. 8; nur wenn Signal vorhanden.
//       Alternativ feste Verstaerkung (-G).
//    d) Modulationstiefe: Ausgang m(t) = Audio * Verstaerkung * md, hart auf +-md begrenzt
//       (md = Option -m, Standard 0.85 = 85 % Spitzenmodulation). Der Rest (>= 15 %) bleibt
//       als Reserve fuer die in-phase-Anteile des Digitalsignals ([1012s] 14.2.4 Analog AM
//       Modulator: s = (1 + m(t)) * Traeger). Die AGC-/md-Werte sind Betriebsentscheidungen,
//       keine Normwerte.
//    Ergebnis: Q15-Werte relativ zum Traeger (32768 = 100 % Modulation) im Ring xa[].
//  ----------------------------------------------------------------------------
static void analog_block(Tx *t){
    // 441 Eingangssamples (verzoegert), LPF, AGC, Modulationstiefe
    int16_t in[AN_BLK]; float pk=0;
    uint64_t h=ring_head(t);
    // Analog Diversity Delay in Audio-Samples (siehe Funktionskopf).
    int64_t Da=(int64_t)(t->analog_delay_s*44100.0);
    for(int j=0;j<AN_BLK;j++){
        int64_t idx=(int64_t)(t->a_rel+j)-Da;
        int16_t v=0;
        if(idx>=0&&(uint64_t)idx<h&&h-(uint64_t)idx<(uint64_t)(AUD_RING-8192)) v=t->ring[idx&(AUD_RING-1)];
        in[j]=v;
    }
    int32_t y[AN_BLK];
    for(int j=0;j<AN_BLK;j++){
        t->hx[t->hxp]=in[j]; t->hx[t->hxp+LPF_TAPS]=in[j];
        const int16_t *x=&t->hx[t->hxp+1]; int64_t acc=0;
        // x[0..LPF_TAPS-1] ist der aeltere->neuere Verlauf (Doppelpuffer)
        for(int k=0;k<LPF_TAPS;k++) acc+=(int64_t)t->lpf[k]*x[LPF_TAPS-1-k];
        y[j]=(int32_t)(acc>>15);
        t->hxp=(t->hxp+1)%LPF_TAPS;
        float a=fabsf((float)y[j])/32768.0f; if(a>pk) pk=a;
    }
    float g;
    if(t->fixed_gain>0) g=t->fixed_gain;
    else {
        if(pk>0.003f||pk>t->agc_peak){
            t->agc_peak=0.995f*t->agc_peak+0.005f*pk;
            // Ziel: Spitzenwert 0.9 Vollausschlag; Glaettung folgt in der naechsten Zeile (Zeitkonstante ca. 5 s).
            float tg=0.9f/(t->agc_peak+0.001f);
            t->agc_gain=0.998f*t->agc_gain+0.002f*tg;
            if(t->agc_gain>8.0f) t->agc_gain=8.0f;
            if(t->agc_gain<0.1f) t->agc_gain=0.1f;
        }
        g=t->agc_gain;
    }
    int32_t gq=(int32_t)(g*t->md*32768.0f);                    // Q15: Audio(+-1)*gain*md
    float mx=0;
    for(int j=0;j<AN_BLK;j++){
        int64_t m=((int64_t)y[j]*gq)>>15;                      // Q15, relativ zum Traeger 1.0=32768
        int32_t lim=(int32_t)(t->md*32768.0f);
        if(m>lim) m=lim;
        if(m<-lim) m=-lim;
        t->xa[(t->a_filled+j)&4095]=(int32_t)m;
        if(fabsf((float)m)>mx) mx=fabsf((float)m);
    }
    t->cur_mod=mx/32768.0f; t->last_peak=pk;
    t->a_rel+=AN_BLK; t->a_filled+=AN_BLK;
}

// Ein Analog-Sample bei 744.1875 kHz (Traeger 1.0 + Modulation), Q15
//  analog_sample() - liefert das Analogsignal fuer ein Basisband-Sample bei 744187.5 Hz: Traeger
//    1.0 (32768) + interpolierte Modulation. Schreitet im Raster 8/135 durch die Audio-Samples
//    (Polyphasenindex a_ph, Audioindex a_na) und ruft bei Bedarf analog_block() nach.
static inline int32_t analog_sample(Tx *t){
    while(t->a_na>=t->a_filled) analog_block(t);
    int64_t acc=0;
    for(int k=0;k<PP_K;k++) acc+=(int64_t)t->pp[t->a_ph][k]*t->xa[(t->a_na-k)&4095];
    t->a_ph+=PP_M; if(t->a_ph>=PP_L){ t->a_ph-=PP_L; t->a_na++; }
    return (1<<QB)+(int32_t)(acc>>15);
}

// ----------------------------------------------------------------- Basisband-Block
// Erzeugt einen OFDM-Symbolblock (4320 Samples) und schreibt (Analog + Digital) ins FIFO
// ----------------------------------------------------------------------------
//  gen_block() - erzeugt 4320 Basisband-Samples (ein OFDM-Symbol, 5.8 ms) im FIFO
//    OFDM-Signalerzeugung und Pulsformung ([1012s] 13.2, Bild 13-2):
//      Das Fenster w(t) ist 2*T lang (8192 Werte bei 16-fach). Aufeinanderfolgende Symbole
//      werden mit Ueberlappung addiert, Symbolabstand Ts = (1+alpha)*T = 4320 Samples.
//      Ausgabeblock n (Index i = 0..4319):
//          out[i]          = A[i] * w[4096 + i]          fuer i < 4096   (abfallende Haelfte, Symbol n)
//          out[CP + i]    += B[i] * w[i]                 fuer i < 4096   (ansteigende Haelfte, Symbol n+1)
//      mit CP = 224 (14*16). A ist das IFFT-Ergebnis des aktuellen, B das des naechsten
//      Symbols (history 2 beim gr-nrsc5-Block am_pulse_shaper). Dadurch entsteht je Symbol
//      effektiv "Cyclic Prefix + Nutzteil" mit weichen Uebergaengen.
//    Danach A <- B, B <- naechstes Symbol (next_symbol_ifft).
//    Transmission Subsystem ([1012s] 14.2.1 Symbol Concatenation, 14.2.5 Analog/Digital Combiner):
//      I = Re(digital) + Analogsignal (Traeger 1.0 + m(t) bei MA1; bei MA3 nur der unmodulierte
//      Traeger 1.0), Q = Im(digital). Bei MA3 entfaellt der Analogpfad, der Traeger bleibt
//      ([1012s] 5.4: "The unmodulated AM carrier is retained").
//  ----------------------------------------------------------------------------
static void gen_block(Tx *t){
    static __thread int32_t blk_r[SYM_BB], blk_i[SYM_BB];
    for(int i=0;i<NFFT;i++){
        int64_t w=pf_win[NFFT+i];
        blk_r[i]=(int32_t)((t->Ar[i]*w)>>15); blk_i[i]=(int32_t)((t->Ai[i]*w)>>15);
    }
    // Die letzten 224 Samples (Cyclic-Prefix-Bereich) enthalten nur den ansteigenden Anteil des naechsten Symbols.
    for(int i=NFFT;i<SYM_BB;i++){ blk_r[i]=0; blk_i[i]=0; }
    for(int i=0;i<NFFT;i++){
        int64_t w=pf_win[i];
        blk_r[CP_BB+i]+=(int32_t)((t->Br[i]*w)>>15); blk_i[CP_BB+i]+=(int32_t)((t->Bi[i]*w)>>15);
    }
    // naechstes Symbolpaar vorbereiten: A <- B, B <- neues IFFT
    // Rolle tauschen: das bisherige 'naechste' Symbol wird 'aktuell', der freie Puffer nimmt die neue IFFT auf.
    int32_t *tr=t->Ar,*ti=t->Ai; t->Ar=t->Br; t->Ai=t->Bi; t->Br=tr; t->Bi=ti;
    next_symbol_ifft(t,t->Br,t->Bi);
    for(int i=0;i<SYM_BB;i++){
        // Traeger = 1.0; bei MA1 durch Analogsignal ersetzt (Traeger + Modulation), bei MA3 konstant.
        int32_t a=(1<<QB);
        if(t->sm==1) a=analog_sample(t);
        t->bbI[(t->bw+i)&(BB_RING-1)]=blk_r[i]+a;
        t->bbQ[(t->bw+i)&(BB_RING-1)]=blk_i[i];
    }
    t->bw+=SYM_BB;
}
//  tx_prime() - Startzustand: IFFT der ersten zwei Symbole (A = Symbol 0, B = Symbol 1);
//    Lesezeiger br = 1 (Farrow braucht ein Sample Vorlauf).
static void tx_prime(Tx *t){
    next_symbol_ifft(t,t->Ar,t->Ai); next_symbol_ifft(t,t->Br,t->Bi);
    t->bw=0; t->br=1; t->bb_primed=1;
}

// ============================================================================
//  RF-Engine: kubische Lagrange/Farrow-Interpolation 744.1875 kHz -> 5 MSPS, NCO, Mischer
// ============================================================================
//  Von 744187.5 Hz Basisband (I/Q) auf die Ausgaberate (Standard 5 MSPS) und auf die Traeger-
//  frequenz. Up-Conversion: [1012s] 14.2.6; Traegerfrequenz/Kanalraster: [1082s] 4.2 "Carrier
//  Frequency and Channel Spacing" (Mittelwelle 9 kHz in Europa, 10 kHz in Amerika).
//  Die Interpolation ist eine kubische Lagrange-Interpolation (Farrow-Struktur) mit Phasen-
//  akkumulator: ratio = 744187.5/5e6 = 0.1488375 = frac_inc/2^32 (Fehler < 1e-9; irrelevant,
//  weil die DAC-Taktgenauigkeit weit darunter liegt). Weil das Basisband 16-fach ueberabgetastet
//  ist (Nutzband +-15 kHz bei 744 kHz Rate), liegen die Spiegel bei 744 kHz - 15 kHz; die
//  Daempfung des kubischen Interpolators dort betraegt rechnerisch mehr als 90 dB.
//  Gemessen (12 Bit, MA1): -58 dBc bei +-16 kHz, -93 dBc bei +-18 kHz, danach nur
//  Quantisierungsrauschen (-102 dBc je 300 Hz), keine Spiegelspektren bei 46.5/744 kHz.
//  ----------------------------------------------------------------------------
//  farrow_coef() - Polynomkoeffizienten der kubischen Lagrange-Interpolation
//    Stuetzstellen xm1, x0, x1, x2 (Indizes br-1 .. br+2 im FIFO), gesucht y(mu), 0 <= mu < 1
//    zwischen x0 und x1:  y = c0 + c1*mu + c2*mu^2 + c3*mu^3  mit
//      c0 = x0
//      c1 = (-2*xm1 - 3*x0 + 6*x1 - x2) / 6
//      c2 = (xm1 + x1 - 2*x0) / 2
//      c3 = (x2 - xm1 + 3*(x0 - x1)) / 6
//    Die Koeffizienten werden nur beim Weiterschalten des Basisband-Samples (ca. jedes 6.7.
//    Ausgabesample) neu berechnet; je Ausgabesample bleibt nur das Horner-Schema (3 Multiplikationen
//    je Komponente). Division durch 6 per Ganzzahl (Rundungsfehler < 1 LSB bei Q15).
//  ----------------------------------------------------------------------------
static void farrow_coef(Tx *t){
    int64_t xm1,x0,x1,x2;
    for(int ch=0;ch<2;ch++){
        int32_t *b=ch?t->bbQ:t->bbI;
        xm1=b[(t->br-1)&(BB_RING-1)]; x0=b[t->br&(BB_RING-1)]; x1=b[(t->br+1)&(BB_RING-1)]; x2=b[(t->br+2)&(BB_RING-1)];
        int64_t *c=ch?t->c_q:t->c_i;
        c[0]=x0;
        c[1]=(-2*xm1-3*x0+6*x1-x2)/6;
        c[2]=(xm1+x1-2*x0)/2;
        c[3]=(x2-xm1+3*(x0-x1))/6;
    }
}

// Rendert n RF-Samples eines Senders und addiert sie (skaliert auf DAC) in acc[]
//  ----------------------------------------------------------------------------
//  tx_render() - erzeugt n RF-Samples eines Senders und addiert sie in acc[]
//    Je Ausgabesample:
//      1. frac += frac_inc; bei Ueberlauf (Carry) naechstes Basisband-Sample (br++), bei Bedarf
//         gen_block() nachladen und Koeffizienten neu berechnen.
//      2. I, Q = Horner-Auswertung mit mu = frac >> 17 (Q15), int64-Rechnung.
//      3. NCO: phase += phase_inc (32 Bit, phase_inc = freq/rate * 2^32); Index = phase >> 18
//         (14 Bit); sin und cos aus derselben Tabelle.
//      4. Mischung auf die Traegerfrequenz: m = (I*cos - Q*sin) >> 15, d.h. s(t) = Re{(I+jQ) e^(j*w*t)}
//         (positives Basisband-Spektrum = oberes Seitenband, [1012s] 14.2.6).
//      5. Skalierung auf DAC-Bereich: acc += (m * gq) >> 16. gq = 2*Pegel*DAC-Vollausschlag*
//         ext_gain/Anzahl_Sender (siehe main). m ist Q15 mit 32768 = Traegeramplitude 1.0.
//    Hot loop: nur Ganzzahlen, keine Funktionsaufrufe ausser dem seltenen gen_block().
//  ----------------------------------------------------------------------------
static void tx_render(Tx *t,int32_t *acc,int n,int32_t gq){
    uint32_t frac=t->frac,inc=t->frac_inc,phase=t->phase,pinc=t->phase_inc;
    int64_t ci0=t->c_i[0],ci1=t->c_i[1],ci2=t->c_i[2],ci3=t->c_i[3];
    int64_t cq0=t->c_q[0],cq1=t->c_q[1],cq2=t->c_q[2],cq3=t->c_q[3];
    for(int k=0;k<n;k++){
        // Interpolationsphase (Q32); Carry = naechstes Basisband-Sample.
        frac+=inc;
        if(frac<inc){                                   // Uebertrag: naechstes Basisband-Sample
            t->br++;
            while(t->bw<t->br+3) gen_block(t);
            farrow_coef(t);
            ci0=t->c_i[0];ci1=t->c_i[1];ci2=t->c_i[2];ci3=t->c_i[3];
            cq0=t->c_q[0];cq1=t->c_q[1];cq2=t->c_q[2];cq3=t->c_q[3];
        }
        // mu in Q15 (obere 15 Bit der Phase).
        int64_t mu=frac>>17;                            // Q15
        int64_t yi=ci3; yi=((yi*mu)>>15)+ci2; yi=((yi*mu)>>15)+ci1; yi=((yi*mu)>>15)+ci0;
        int64_t yq=cq3; yq=((yq*mu)>>15)+cq2; yq=((yq*mu)>>15)+cq1; yq=((yq*mu)>>15)+cq0;
        // NCO-Phase der Trageroszillation; 14-Bit-Tabellenindex aus den oberen Bits.
        phase+=pinc;
        uint32_t idx=phase>>(32-LUT_BITS);
        int64_t s=sine_lut[idx], c=sine_lut[(idx+LUT_SIZE/4)&(LUT_SIZE-1)];
        // Mischer: Re{(I + jQ) * e^(j*phase)} = I*cos - Q*sin (Q15 * Q15 >> 15).
        int64_t m=(yi*c-yq*s)>>15;                      // Q15, Traeger = 32768
        // Skalierung auf DAC-Bereich und Summation ueber alle Sender.
        acc[k]+=(int32_t)((m*gq)>>16);
    }
    t->frac=frac; t->phase=phase;
}

// ============================================================================
//  Netzwerk-Threads
// ============================================================================
//  Hilfsthreads: audio_receiver (je Sender, UDP-Audio), control_receiver (UDP 8888: Fading
//  "freq:gain" sowie "title=", "artist=", "message=" fuer PSD/SIS-Laufschrift), sferics_receiver
//  (UDP 8889: Blitzstoerungen "amp:dauer_ms", unveraendert zum Basisprogramm am_modulator_5MSPS_integer).
//  Datenaustausch mit dem Hauptthread ueber einzelne int32-/uint64-Variablen; fuer den Audio-
//  Ringpuffer sorgen __atomic-Zugriffe (Release/Acquire) auf den Schreibzeiger head dafuer, dass
//  die Samples vor dem Zeiger sichtbar sind.
volatile int32_t sferics_trigger_amp=0, sferics_trigger_decay=0;
static Tx *g_tx; static int g_ntx; static double g_rate=5e6;

//  audio_receiver() - Thread je Sender: empfaengt UDP-Datagramme (s16le, mono, 44100 Hz), schreibt
//    die Samples in den Ring und veroeffentlicht den Schreibzeiger. Ungerade Byteanzahl am
//    Paketende wird bis zum naechsten Paket gemerkt.
static void* audio_receiver(void *arg){
    Tx *t=(Tx*)arg; uint8_t buf[8192]; int have_odd=0; uint8_t odd=0;
    while(1){
        ssize_t n=recv(t->sock,buf,sizeof(buf),0);
        if(n<=0){ usleep(100); continue; }
        uint64_t h=t->head; ssize_t i=0;
        if(have_odd){ t->ring[h&(AUD_RING-1)]=(int16_t)(odd|(buf[0]<<8)); h++; i=1; have_odd=0; }
        for(;i+1<n;i+=2){ t->ring[h&(AUD_RING-1)]=(int16_t)(buf[i]|(buf[i+1]<<8)); h++; }
        if(i<n){ odd=buf[i]; have_odd=1; }
        __atomic_store_n(&t->head,h,__ATOMIC_RELEASE);
    }
    return NULL;
}

// Steuerung (UDP 8888): "freq:gain"  |  "title=Text"  |  "artist=Text"  |  "message=Text"
//  control_receiver() - UDP 8888, Textkommandos (ein Befehl je Datagramm):
//    "603000:0.5"       Frequenz:Verstaerkung (ext_gain) des passenden Senders (+-1 Hz) -> Fading/Pegel
//    "title=Text"       PSD-Titel   (setzt neues PSD-Paket)     "artist=Text"  PSD-Interpret
//    "message=Text"     SIS "Station Message" (Laufschrift)
static void* control_receiver(void *arg){
    (void)arg;
    int sock=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(8888),.sin_addr.s_addr=INADDR_ANY};
    if(bind(sock,(struct sockaddr*)&a,sizeof(a))<0) return NULL;
    char buf[256];
    while(1){
        ssize_t n=recv(sock,buf,sizeof(buf)-1,0);
        if(n<=0) continue;
        buf[n]=0; while(n>0&&(buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
        float f,g;
        if(!strncmp(buf,"title=",6)){ for(int i=0;i<g_ntx;i++){ snprintf(g_tx[i].psd.title,sizeof g_tx[i].psd.title,"%s",buf+6); g_tx[i].psd.pkt_off=g_tx[i].psd.pkt_len; } }
        else if(!strncmp(buf,"artist=",7)){ for(int i=0;i<g_ntx;i++){ snprintf(g_tx[i].psd.artist,sizeof g_tx[i].psd.artist,"%s",buf+7); g_tx[i].psd.pkt_off=g_tx[i].psd.pkt_len; } }
        else if(!strncmp(buf,"message=",8)){ for(int i=0;i<g_ntx;i++) snprintf(g_tx[i].sis.message,sizeof g_tx[i].sis.message,"%s",buf+8); }
        else if(sscanf(buf,"%f:%f",&f,&g)==2){
            for(int i=0;i<g_ntx;i++) if(fabs(g_tx[i].freq-f)<1.0){ g_tx[i].ext_gain=g; break; }
        }
    }
    return NULL;
}

// Sferics/Blitz (UDP 8889): "amp:dauer_ms"
//  sferics_receiver() - UDP 8889, "amp:dauer_ms": loest einen exponentiell abklingenden
//    Rauschimpuls (Blitz/Sferics) aus; Umsetzung in main(). Unveraendert aus dem Basisprogramm.
static void* sferics_receiver(void *arg){
    (void)arg;
    int sock=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(8889),.sin_addr.s_addr=INADDR_ANY};
    if(bind(sock,(struct sockaddr*)&a,sizeof(a))<0) return NULL;
    char buf[256];
    while(1){
        ssize_t n=recv(sock,buf,sizeof(buf)-1,0);
        if(n<=0) continue;
        buf[n]=0; float amp,ms;
        if(sscanf(buf,"%f:%f",&amp,&ms)==2){
            float total=(ms/1000.0f)*(float)g_rate;
            int32_t d=(int32_t)(expf(-4.60517f/(total+1.0f))*32768.0f); if(d>32767) d=32767;
            sferics_trigger_decay=d; sferics_trigger_amp=(int32_t)amp;
        }
    }
    return NULL;
}

// ============================================================================
//  Sender anlegen
// ============================================================================
//  tx_setup() legt Speicher, L1/L2/SIS/PSD/HDC, Analogfilter, Audioquelle und RF-Zustand je Sender an
//  und berechnet die ersten zwei OFDM-Symbole voraus, damit tx_render() sofort starten kann.
//  main() wertet die Optionen aus, erzeugt die Sender und fuehrt die Ausgabeschleife aus.
typedef struct { const char *name,*slogan,*message,*country,*title,*artist; int ptype; } Meta;

//  ----------------------------------------------------------------------------
//  tx_setup() - einen Sender komplett initialisieren
//    Reihenfolge: Ring/L1/SIS/PSD/L2 (P1: num_progs 1, 3750 Bit, 4 Byte Fixed Data, CCC 1;
//    P3: num_progs 0, 24000 bzw. 30000 Bit, 2048 Byte Fixed Data, CCC 24) / HDC / Analogfilter /
//    Audioquelle (UDP-Port > 0, sonst Test- bzw. Dateiquelle) / NCO- und Interpolationsinkremente /
//    tx_prime() und erstes gen_block().
//    phase_inc = freq / rate * 2^32 (Aufloesung rate/2^32 = 1.16 mHz bei 5 MSPS).
//    frac_inc = 744187.5 / rate * 2^32.
//  ----------------------------------------------------------------------------
static int tx_setup(Tx *t,int bits_unused,const Meta *m,const char *audio_file){
    (void)bits_unused;
    t->ring=calloc(AUD_RING,sizeof(int16_t));
    t->l1=calloc(1,sizeof(l1_t)); l1_init(t->l1,t->sm,t->rdb,t->hpp,t->pl,t->aab);
    sis_init(&t->sis,m->name,m->slogan,m->message,m->country,m->ptype);
    snprintf(t->psd.title,sizeof t->psd.title,"%s",m->title); snprintf(t->psd.artist,sizeof t->psd.artist,"%s",m->artist); t->psd.prog=0;
    l2_init(&t->l2a,1,3750,4,1,m->ptype);
    l2_init(&t->l2b,0,t->sm==1?24000:30000,2048,24,0);
    if(hdc_init(&t->hdc,t->bitrate)<0){ fprintf(stderr,"HDC-Encoder konnte nicht initialisiert werden\n"); return -1; }
    t->p1bits=calloc(8*3750,1); t->p3bits=calloc(30000,1);
    t->Ar=calloc(NFFT,4);t->Ai=calloc(NFFT,4);t->Br=calloc(NFFT,4);t->Bi=calloc(NFFT,4);t->Xr=calloc(NFFT,4);t->Xi=calloc(NFFT,4);
    t->bbI=calloc(BB_RING,4); t->bbQ=calloc(BB_RING,4);
    if(t->sm==1) analog_design(t);
    t->agc_gain=1.0f; t->agc_peak=0.1f;
    if(t->port==0){
        if(audio_file){ t->src_file=fopen(audio_file,"rb"); if(!t->src_file){ perror("Audiodatei"); return -1; } t->src_kind=2; }
        else t->src_kind=1;
    } else {
        t->src_kind=0;
        t->sock=socket(AF_INET,SOCK_DGRAM,0);
        struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(t->port),.sin_addr.s_addr=INADDR_ANY};
        if(bind(t->sock,(struct sockaddr*)&a,sizeof(a))<0){ perror("bind Audio-UDP"); return -1; }
        pthread_t tid; pthread_create(&tid,NULL,audio_receiver,t);
    }
    t->phase_inc=(uint32_t)llround(t->freq/g_rate*4294967296.0);
    t->frac_inc=(uint32_t)llround(FS_BB/g_rate*4294967296.0);
    tx_prime(t);
    while(t->bw<t->br+3) gen_block(t);
    farrow_coef(t);
    return 0;
}

//  usage() - Hilfetext. Die Modus-Optionen je Sender (3. Feld, durch Komma getrennt):
//    ma1 | ma3 (Servicemodus), aab, pl, hpp, rdb (Steuersignale, [1012s] 6.3 und 6.4).
static void usage(const char *p){
    fprintf(stderr,
    "HD Radio (NRSC-5-D) Mittelwellen-Modulator, Integer, 5 MSPS\n"
    "Nutzung: %s [Optionen] <Port:Frequenz[:Modus]> ...\n"
    "  Port      UDP-Port fuer Audio (s16le, mono, 44100 Hz); 0 = interner Testton bzw. -i Datei\n"
    "  Frequenz  Traegerfrequenz in Hz (z.B. 603000)\n"
    "  Modus     Komma-Liste: ma1 (Default, Hybrid) | ma3 (All-Digital) | rdb | hpp | pl | aab\n"
    "Optionen:\n"
    "  -b <bits>   DAC-Bittiefe 8..16 (Default 8)\n"
    "  -s <msps>   5 (Default, 5 MSPS) | 10 (5 MSPS doppelt ausgegeben, wie am_modulator) | andere Raten direkt\n"
    "  -o <datei>  Ausgabe in Datei statt TCP 12345 (rohe int8/int16), mit -n <sek> Dauer\n"
    "  -i <datei>  Audio-Datei (s16le mono 44100 Hz) fuer Sender mit Port 0 (Endlosschleife)\n"
    "  -m <0..1>   Analog-Modulationsgrad bei MA1 (Default 0.85)   -G <g> feste Analog-Verstaerkung (statt AGC)\n"
    "  -D <sek>    Analog-Verzoegerung (Default 5.5 s, NRSC-5 Diversity-Delay)\n"
    "  -r <bps>    HDC-Bitrate (Default 17900)    -L <0..1> Traegerpegel relativ zum Vollausschlag\n"
    "  -N name  -S slogan  -M nachricht  -C land(2)  -T titel  -A interpret  -P programmtyp\n"
    "Steuerung per UDP: 8888 \"freq:gain\", \"title=..\", \"artist=..\", \"message=..\"; 8889 Sferics \"amp:ms\"\n"
    "Beispiel: %s -b 12 1234:603000 1235:828000:ma3\n"
    "          sudo fl2k_tcp -a 127.0.0.1 -p 12345 -s 5000000\n",p,p);
}

//  ----------------------------------------------------------------------------
//  main() - Optionen, Sender anlegen, Ausgabeschleife
//    Pro Schleifendurchlauf (OUT_CHUNK = 50000 Samples = 10 ms bei 5 MSPS):
//      1. acc[] nullen, dann je Sender tx_render() (Summe aller Traeger).
//      2. Pro Sample: Sferics-Impuls addieren (falls aktiv), hart auf die DAC-Bittiefe begrenzen
//         (clip-Zaehler im Log), in int8 (8 Bit) oder int16 (9..16 Bit) schreiben; bei -s 10 jedes
//         Sample doppelt.
//      3. Chunk per send() an fl2k_tcp (oder fwrite in Datei). send() blockiert, wenn der
//         Abnehmer nicht schnell genug liest - das ist der einzige Takt des Systems.
//    Pegel: Traegeramplitude = lv * DAC-Vollausschlag / Anzahl_Sender mit lv = 0.33 (MA1) bzw. 0.09
//    (MA3). Die Werte sind so gewaehlt, dass die Spitzen des OFDM-Signals nicht begrenzt werden:
//    MA1: Traeger 1 + Analog bis +0.85 + Digital (ca. 3-fach Spitze); MA3: Spitzenwert bis ca. 9-fach
//    der Traegeramplitude. Mit -L ueberschreibbar. Alle Sender teilen sich den Vollausschlag.
//    Statusausgabe alle 2 s: DAC-Spitzen, Clip-Zaehler, Frames je Sender, Audio-Underruns.
//  ----------------------------------------------------------------------------
int main(int argc,char **argv){
    int bits=8; double msps=5.0; const char *ofile=NULL,*afile=NULL; double secs=10; float md=0.85f,fg=0,level=-1; double adel=5.5; int bitrate=17900;
    Meta meta={"HDAM","HD Radio AM Modulator","5 MSPS Integer NRSC-5 MA1/MA3","DE","Titel","Interpret",0};
    int opt;
    while((opt=getopt(argc,argv,"b:s:o:n:i:m:G:D:r:L:N:S:M:C:T:A:P:h"))!=-1){
        switch(opt){
        case 'b':bits=atoi(optarg);break; case 's':msps=atof(optarg);break; case 'o':ofile=optarg;break; case 'n':secs=atof(optarg);break;
        case 'i':afile=optarg;break; case 'm':md=atof(optarg);break; case 'G':fg=atof(optarg);break; case 'D':adel=atof(optarg);break;
        case 'r':bitrate=atoi(optarg);break; case 'L':level=atof(optarg);break;
        case 'N':meta.name=optarg;break; case 'S':meta.slogan=optarg;break; case 'M':meta.message=optarg;break; case 'C':meta.country=optarg;break;
        case 'T':meta.title=optarg;break; case 'A':meta.artist=optarg;break; case 'P':meta.ptype=atoi(optarg);break;
        default:usage(argv[0]);return 1; }
    }
    if(optind>=argc||bits<8||bits>16||strlen(meta.country)!=2){ usage(argv[0]); return 1; }
    int ntx=argc-optind; if(ntx>MAX_TX){ fprintf(stderr,"max. %d Sender\n",MAX_TX); return 1; }
    int mult=1; g_rate=msps*1e6; if(msps==10.0){ g_rate=5e6; mult=2; }
    init_crc8(); init_rs(); init_tables();
    Tx *tx=calloc(ntx,sizeof(Tx)); g_tx=tx; g_ntx=ntx;
    for(int i=0;i<ntx;i++){
        char *c=strdup(argv[optind+i]),*sp; char *p=strtok_r(c,":",&sp),*f=strtok_r(NULL,":",&sp),*o=strtok_r(NULL,":",&sp);
        if(!p||!f){ usage(argv[0]); return 1; }
        tx[i].port=atoi(p); tx[i].freq=atof(f); tx[i].sm=1; tx[i].md=md; tx[i].fixed_gain=fg; tx[i].ext_gain=1.0f;
        tx[i].analog_delay_s=adel; tx[i].bitrate=bitrate;
        if(o){ char *sp2,*w=strtok_r(o,",",&sp2); while(w){
            if(!strcmp(w,"ma1"))tx[i].sm=1; else if(!strcmp(w,"ma3"))tx[i].sm=3; else if(!strcmp(w,"rdb"))tx[i].rdb=1;
            else if(!strcmp(w,"hpp"))tx[i].hpp=1; else if(!strcmp(w,"pl"))tx[i].pl=1; else if(!strcmp(w,"aab"))tx[i].aab=1;
            else { fprintf(stderr,"Unbekannte Option '%s'\n",w); return 1; }
            w=strtok_r(NULL,",",&sp2);} }
        free(c);
        if(tx_setup(&tx[i],bits,&meta,afile)<0) return 1;
    }
    pthread_t tid; pthread_create(&tid,NULL,control_receiver,NULL);
    pthread_create(&tid,NULL,sferics_receiver,NULL);

    int out_fd=-1; FILE *of=NULL;
    if(ofile){ of=fopen(ofile,"wb"); if(!of){ perror("Ausgabedatei"); return 1; } }
    else {
        int lfd=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        struct sockaddr_in s={.sin_family=AF_INET,.sin_port=htons(12345),.sin_addr.s_addr=INADDR_ANY};
        if(bind(lfd,(struct sockaddr*)&s,sizeof(s))<0){ perror("bind 12345"); return 1; }
        listen(lfd,1);
        fprintf(stderr,"Warte auf SDR (%g MSPS, %d Bit) an Port 12345...\n",msps,bits);
        out_fd=accept(lfd,NULL,NULL);
        int sb=8<<20; setsockopt(out_fd,SOL_SOCKET,SO_SNDBUF,&sb,sizeof(sb));
    }
    int32_t *acc=malloc(OUT_CHUNK*4); size_t osz=(size_t)OUT_CHUNK*mult*(bits==8?1:2); uint8_t *ob=malloc(osz);
    int32_t maxv=(1<<(bits-1))-1, minv=-(1<<(bits-1));
    uint32_t sf_prng=2463534242u; int32_t sf_env=0,sf_amp=0,sf_dec=0;
    uint64_t done=0,total=(uint64_t)(secs*g_rate),clip=0; time_t last=time(NULL);
    int32_t pk_pos=0,pk_neg=0;
    while(1){
        memset(acc,0,OUT_CHUNK*4);
        for(int i=0;i<ntx;i++){
            // Traegerpegel relativ zum DAC-Vollausschlag (Spitzenreserve, siehe main()-Kopf).
            double lv=(level>0)?level:(tx[i].sm==1?0.33:0.09);
            tx_render(&tx[i],acc,OUT_CHUNK,(int32_t)(2.0*lv*(double)(1<<(bits-1))*tx[i].ext_gain/ntx));
        }
        size_t o=0;
        for(int k=0;k<OUT_CHUNK;k++){
            int32_t v=acc[k];
            if(sferics_trigger_amp>0){ sf_amp=sferics_trigger_amp; sf_dec=sferics_trigger_decay; sf_env=32767; sferics_trigger_amp=0; }
            if(sf_env>0){
                sf_prng^=sf_prng<<13; sf_prng^=sf_prng>>17; sf_prng^=sf_prng<<5;
                int32_t noise=(int32_t)(sf_prng&0xFFFF)-32768;
                int32_t burst=(((noise*sf_env)>>15)*sf_amp)>>8;
                v+=(int32_t)(((int64_t)burst*(1<<(bits-8)))/(230*ntx));
                sf_env=(sf_env*sf_dec)>>15; if(sf_env<10) sf_env=0;
            }
            // Harte Begrenzung auf den DAC-Bereich; clip zaehlt die Ereignisse (sollte 0 sein).
            if(v>maxv){ v=maxv; clip++; } else if(v<minv){ v=minv; clip++; }
            if(v>pk_pos) pk_pos=v;
            if(v<pk_neg) pk_neg=v;
            for(int q=0;q<mult;q++){
                if(bits==8) ((int8_t*)ob)[o++]=(int8_t)v; else { ((int16_t*)ob)[o++]=(int16_t)v; }
            }
        }
        size_t bytes=(size_t)OUT_CHUNK*mult*(bits==8?1:2);
        if(of){ fwrite(ob,1,bytes,of); }
        else { size_t sent=0; while(sent<bytes){ ssize_t w=send(out_fd,ob+sent,bytes-sent,MSG_NOSIGNAL); if(w<=0){ fprintf(stderr,"Verbindung beendet\n"); return 0; } sent+=w; } }
        done+=OUT_CHUNK;
        if(time(NULL)!=last&&(time(NULL)-last>=2||ofile)){
            last=time(NULL);
            fprintf(stderr,"--- %.1f s | DAC-Spitzen %d..%d (Bereich %d..%d) clip=%llu\n",done/g_rate,pk_neg,pk_pos,minv,maxv,(unsigned long long)clip);
            for(int i=0;i<ntx;i++) fprintf(stderr,"  TX%d %8.1f kHz %s%s%s%s%s | Frames %llu, Audio-Underruns %llu | Analog: Mod %.0f%% AGC %.1fx | Ext-Gain %.2f\n",
                i,tx[i].freq/1000,tx[i].sm==1?"MA1":"MA3",tx[i].rdb?",RDB":"",tx[i].hpp?",HPP":"",tx[i].pl?",PL":"",tx[i].aab?",AAB":"",
                (unsigned long long)tx[i].frames_built,(unsigned long long)tx[i].underruns,tx[i].cur_mod*100,tx[i].agc_gain,tx[i].ext_gain);
            pk_pos=pk_neg=0;
        }
        if(ofile&&done>=total) break;
    }
    if(of) fclose(of);
    return 0;
}
