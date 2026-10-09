#!/usr/bin/env python3
# ============================================================================
#  hdtxgui.py - Tk-Oberflaeche fuer den HD-Radio-Mittelwellenmodulator (hdradio_am)
#
#  Nachfolger von amtxgui.py. Senderlandschaften (CSV), Internetradio-Datenbank
#  (stations.db), freie URLs/Dateipfade, MA1/MA3 mit Optionen, Start/Stop, Live-Steuerung.
#
#  Dateien (alle im Ordner dieses Skripts):
#     hdradio_am            der Modulator (Pfad in den Einstellungen aenderbar)
#     start_sender_hd.sh    startet ffmpeg-Instanzen und den Modulator
#     start_fl2k.sh         (optional, wie bisher) startet fl2k_tcp, wird per "sudo" aufgerufen
#     stations.db           Internetradio-Datenbank, Zeilen "Name,URL" (wie bisher)
#     hdtx_settings.json    wird von der GUI angelegt (Einstellungen)
# ============================================================================
import csv, json, os, re, socket, subprocess, shutil
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

MAX_STATIONS = 8            # MAX_TX im Modulator
AUDIO_PORT_BASE = 1234      # UDP-Audioports 1234, 1235, ... (wie bisher)
CTRL_PORT, SFERICS_PORT = 8888, 8889   # Steuerports des Modulators

# NRSC-Programmtypen (Zahlenwerte wie in gr-nrsc5 / NRSC-5 SIS); "Emergency" bewusst nicht anbietbar
PROGRAM_TYPES = [(0, "Undefined"), (1, "News"), (2, "Information"), (3, "Sports"), (4, "Talk"), (5, "Rock"),
                 (6, "Classic Rock"), (7, "Adult Hits"), (8, "Soft Rock"), (9, "Top 40"), (10, "Country"),
                 (11, "Oldies"), (12, "Soft"), (13, "Nostalgia"), (14, "Jazz"), (15, "Classical"),
                 (16, "Rhythm and Blues"), (17, "Soft R&B"), (18, "Foreign Language"), (19, "Religious Music"),
                 (20, "Religious Talk"), (21, "Personality"), (22, "Public"), (23, "College"),
                 (24, "Spanish Talk"), (25, "Spanish Music"), (26, "Hip-Hop"), (29, "Weather"), (65, "Traffic"),
                 (76, "Special Reading Services")]

# Belegte halbe Bandbreite des Digitalsignals in kHz (Subtraeger bis +-81 bei MA1, +-52 bei MA3,
# +-27 bei MA3 mit RDB; Subtraegerabstand 181,7 Hz). Nur fuer die Nachbarkanal-Warnung.
def half_bandwidth(mode, opts):
    if mode == "MA1":
        return 14.8
    return 5.0 if "RDB" in opts else 9.6

# --- Lokalisierungs-Daten (fehlende Schluessel fallen auf EN zurueck) ---
LANGUAGES = {
    "DE": {
        "win_title": "HD Radio AM (NRSC-5) Multi-Sender Steuerung",
        "col_freq": "Frequenz", "col_mode": "Modus", "col_opts": "Optionen", "col_name": "Programmname", "col_url": "URL / Pfad",
        "btn_start": "ALLE Sender STARTEN", "btn_stop": "ALLE Sender STOPPEN",
        "lbl_mod": "Modulator:", "lbl_sdr": "SDR (fl2k_tcp / socat):",
        "menu_file": "Datei", "menu_load": "Laden", "menu_save": "Speichern", "menu_quit": "Beenden",
        "menu_sender": "Sender", "menu_add": "Hinzufügen", "menu_edit": "Bearbeiten", "menu_del": "Entfernen",
        "menu_mod": "Modulator", "menu_settings": "Einstellungen ...", "menu_lang": "Sprache",
        "dlg_title": "Sender-Parameter", "dlg_plan": "Frequenzplan:", "dlg_freq": "Frequenz:",
        "dlg_mode": "Modus:", "mode_ma1": "MA1 Hybrid (Analog-AM + Digital)", "mode_ma3": "MA3 All-Digital",
        "dlg_opts": "Optionen:", "opt_aab": "Analog-Audio 8 statt 5 kHz (AAB)", "opt_pl": "Sekundär/Tertiär-Leistung hoch (PL)",
        "opt_hpp": "PIDS-Leistung hoch (HPP)", "opt_rdb": "Reduzierte Digitalbandbreite (RDB)",
        "dlg_occ": "Belegung: ca. ±%.1f kHz um den Träger", "dlg_prog": "Programm wählen:",
        "dlg_url": "Programm-URL / Pfad:", "dlg_browse": "Datei ...", "dlg_btn": "Übernehmen", "dlg_cancel": "Abbrechen",
        "err_freq": "Frequenz belegt!", "err_title": "Fehler", "warn_title": "Warnung",
        "err_range": "Frequenz ungültig (erlaubt: 100 bis 2400 kHz).", "err_url": "Bitte URL oder Dateipfad angeben.",
        "warn_overlap": "Das Digitalsignal überlappt mit:\n%s\n\nTrotzdem übernehmen?",
        "err_max": "Maximal %d Sender möglich.", "err_bin": "Modulator nicht gefunden:\n%s",
        "live_title": "Live-Steuerung (nur bei laufendem Modulator)", "live_psd": "Titel:", "live_artist": "Interpret:",
        "live_msg": "Nachricht:", "live_send": "Senden", "live_gain": "Pegel des gewählten Senders (Fading):",
        "live_sferics": "Sferics:", "live_amp": "Stärke", "live_ms": "ms", "live_fire": "Blitz!",
        "set_title": "Modulator-Einstellungen", "set_bits": "DAC-Bittiefe:", "set_rate": "Samplerate (MSPS):",
        "set_name": "Stationsname (SIS, bis 12 Zeichen):", "set_slogan": "Slogan:", "set_msg": "Nachricht:",
        "set_country": "Land (2 Buchstaben):", "set_ptype": "Programmtyp:", "set_md": "Analog-Modulationsgrad (0-1):",
        "set_adelay": "Analog-Verzögerung (s):", "set_level": "Trägerpegel 0-1 (leer = Standard MA1 0,33 / MA3 0,09):",
        "set_bin": "Modulator-Programm:", "set_sdrip": "SDR-IP (Brücke):", "set_sdrport": "SDR-Port:",
        "set_remote": "Remote-fl2k-Modus", "set_fl2k_auto": "fl2k-Skript automatisch starten", "set_fl2k": "fl2k-Befehl:",
        "set_note": "Hinweis: Stationsname, Slogan und Nachricht gelten für alle Sender gemeinsam (SIS des Modulators). Namen mit bis zu 4 Zeichen aus A-Z, Leerzeichen und ?-*$ nutzen das Kurzformat, alle anderen das Langformat.",
        "dlg_save": "Speichern",
    },
    "EN": {
        "win_title": "HD Radio AM (NRSC-5) Multi-Station Control",
        "col_freq": "Frequency", "col_mode": "Mode", "col_opts": "Options", "col_name": "Program Name", "col_url": "URL / Path",
        "btn_start": "START ALL STATIONS", "btn_stop": "STOP ALL STATIONS",
        "lbl_mod": "Modulator:", "lbl_sdr": "SDR (fl2k_tcp / socat):",
        "menu_file": "File", "menu_load": "Load", "menu_save": "Save", "menu_quit": "Quit",
        "menu_sender": "Station", "menu_add": "Add", "menu_edit": "Edit", "menu_del": "Remove",
        "menu_mod": "Modulator", "menu_settings": "Settings ...", "menu_lang": "Language",
        "dlg_title": "Station Parameters", "dlg_plan": "Frequency Plan:", "dlg_freq": "Frequency:",
        "dlg_mode": "Mode:", "mode_ma1": "MA1 Hybrid (analog AM + digital)", "mode_ma3": "MA3 All-digital",
        "dlg_opts": "Options:", "opt_aab": "Analog audio 8 instead of 5 kHz (AAB)", "opt_pl": "Secondary/tertiary power high (PL)",
        "opt_hpp": "PIDS power high (HPP)", "opt_rdb": "Reduced digital bandwidth (RDB)",
        "dlg_occ": "Occupancy: approx. ±%.1f kHz around the carrier", "dlg_prog": "Select Program:",
        "dlg_url": "Program URL / path:", "dlg_browse": "File ...", "dlg_btn": "Apply", "dlg_cancel": "Cancel",
        "err_freq": "Frequency occupied!", "err_title": "Error", "warn_title": "Warning",
        "err_range": "Invalid frequency (allowed: 100 to 2400 kHz).", "err_url": "Please enter a URL or file path.",
        "warn_overlap": "The digital signal overlaps with:\n%s\n\nApply anyway?",
        "err_max": "At most %d stations possible.", "err_bin": "Modulator not found:\n%s",
        "live_title": "Live control (modulator must be running)", "live_psd": "Title:", "live_artist": "Artist:",
        "live_msg": "Message:", "live_send": "Send", "live_gain": "Level of selected station (fading):",
        "live_sferics": "Sferics:", "live_amp": "Strength", "live_ms": "ms", "live_fire": "Lightning!",
        "set_title": "Modulator Settings", "set_bits": "DAC bit depth:", "set_rate": "Sample rate (MSPS):",
        "set_name": "Station name (SIS, up to 12 chars):", "set_slogan": "Slogan:", "set_msg": "Message:",
        "set_country": "Country (2 letters):", "set_ptype": "Program type:", "set_md": "Analog modulation depth (0-1):",
        "set_adelay": "Analog delay (s):", "set_level": "Carrier level 0-1 (empty = default MA1 0.33 / MA3 0.09):",
        "set_bin": "Modulator program:", "set_sdrip": "SDR IP (bridge):", "set_sdrport": "SDR port:",
        "set_remote": "Remote fl2k mode", "set_fl2k_auto": "Start fl2k script automatically", "set_fl2k": "fl2k command:",
        "set_note": "Note: station name, slogan and message apply to all stations together (modulator SIS).",
        "dlg_save": "Save",
    },
    "FR": {
        "win_title": "HD Radio AM (NRSC-5) Contrôle multi-émetteurs",
        "col_freq": "Fréquence", "col_mode": "Mode", "col_opts": "Options", "col_name": "Nom du programme", "col_url": "URL / chemin",
        "btn_start": "DÉMARRER TOUS LES ÉMETTEURS", "btn_stop": "ARRÊTER TOUS LES ÉMETTEURS",
        "lbl_mod": "Modulateur :", "lbl_sdr": "SDR (fl2k_tcp / socat) :",
        "menu_file": "Fichier", "menu_load": "Charger", "menu_save": "Enregistrer", "menu_quit": "Quitter",
        "menu_sender": "Émetteur", "menu_add": "Ajouter", "menu_edit": "Modifier", "menu_del": "Supprimer",
        "menu_mod": "Modulateur", "menu_settings": "Paramètres ...", "menu_lang": "Langue",
        "dlg_title": "Paramètres de l'émetteur", "dlg_plan": "Plan de fréquences :", "dlg_freq": "Fréquence :",
        "dlg_mode": "Mode :", "mode_ma1": "MA1 hybride (AM analogique + numérique)", "mode_ma3": "MA3 tout numérique",
        "dlg_opts": "Options :", "opt_aab": "Audio analogique 8 au lieu de 5 kHz (AAB)", "opt_pl": "Puissance secondaire/tertiaire haute (PL)",
        "opt_hpp": "Puissance PIDS haute (HPP)", "opt_rdb": "Bande numérique réduite (RDB)",
        "dlg_occ": "Occupation : env. ±%.1f kHz autour de la porteuse", "dlg_prog": "Choisir le programme :",
        "dlg_url": "URL / chemin du programme :", "dlg_browse": "Fichier ...", "dlg_btn": "Appliquer", "dlg_cancel": "Annuler",
        "err_freq": "Fréquence occupée !", "err_title": "Erreur", "warn_title": "Avertissement",
        "err_range": "Fréquence invalide (permis : 100 à 2400 kHz).", "err_url": "Veuillez indiquer une URL ou un chemin.",
        "warn_overlap": "Le signal numérique chevauche :\n%s\n\nAppliquer quand même ?",
        "err_max": "Maximum %d émetteurs possibles.", "err_bin": "Modulateur introuvable :\n%s",
        "live_title": "Contrôle en direct (modulateur en marche)", "live_psd": "Titre :", "live_artist": "Artiste :",
        "live_msg": "Message :", "live_send": "Envoyer", "live_gain": "Niveau de l'émetteur choisi (fading) :",
        "live_sferics": "Sferics :", "live_amp": "Force", "live_ms": "ms", "live_fire": "Éclair !",
        "set_title": "Paramètres du modulateur", "set_bits": "Profondeur DAC :", "set_rate": "Fréquence d'échantillonnage (MSPS) :",
        "set_name": "Nom de station (SIS, 12 car. max.) :", "set_slogan": "Slogan :", "set_msg": "Message :",
        "set_country": "Pays (2 lettres) :", "set_ptype": "Type de programme :", "set_md": "Taux de modulation analogique (0-1) :",
        "set_adelay": "Retard analogique (s) :", "set_level": "Niveau porteuse 0-1 (vide = défaut MA1 0,33 / MA3 0,09) :",
        "set_bin": "Programme du modulateur :", "set_sdrip": "IP du SDR (pont) :", "set_sdrport": "Port du SDR :",
        "set_remote": "Mode fl2k distant", "set_fl2k_auto": "Lancer le script fl2k automatiquement", "set_fl2k": "Commande fl2k :",
        "set_note": "Remarque : nom, slogan et message valent pour tous les émetteurs (SIS du modulateur).",
        "dlg_save": "Enregistrer",
    },
    "IT": {
        "win_title": "HD Radio AM (NRSC-5) Controllo multi-stazione",
        "col_freq": "Frequenza", "col_mode": "Modo", "col_opts": "Opzioni", "col_name": "Nome programma", "col_url": "URL / percorso",
        "btn_start": "AVVIA TUTTE LE STAZIONI", "btn_stop": "FERMA TUTTE LE STAZIONI",
        "lbl_mod": "Modulatore:", "lbl_sdr": "SDR (fl2k_tcp / socat):",
        "menu_file": "File", "menu_load": "Carica", "menu_save": "Salva", "menu_quit": "Esci",
        "menu_sender": "Stazione", "menu_add": "Aggiungi", "menu_edit": "Modifica", "menu_del": "Rimuovi",
        "menu_mod": "Modulatore", "menu_settings": "Impostazioni ...", "menu_lang": "Lingua",
        "dlg_title": "Parametri della stazione", "dlg_plan": "Piano di frequenza:", "dlg_freq": "Frequenza:",
        "dlg_mode": "Modo:", "mode_ma1": "MA1 ibrido (AM analogico + digitale)", "mode_ma3": "MA3 tutto digitale",
        "dlg_opts": "Opzioni:", "opt_aab": "Audio analogico 8 invece di 5 kHz (AAB)", "opt_pl": "Potenza secondaria/terziaria alta (PL)",
        "opt_hpp": "Potenza PIDS alta (HPP)", "opt_rdb": "Banda digitale ridotta (RDB)",
        "dlg_occ": "Occupazione: circa ±%.1f kHz attorno alla portante", "dlg_prog": "Seleziona programma:",
        "dlg_url": "URL / percorso del programma:", "dlg_browse": "File ...", "dlg_btn": "Applica", "dlg_cancel": "Annulla",
        "err_freq": "Frequenza occupata!", "err_title": "Errore", "warn_title": "Avviso",
        "err_range": "Frequenza non valida (consentito: da 100 a 2400 kHz).", "err_url": "Inserire un URL o un percorso.",
        "warn_overlap": "Il segnale digitale si sovrappone a:\n%s\n\nApplicare comunque?",
        "err_max": "Al massimo %d stazioni possibili.", "err_bin": "Modulatore non trovato:\n%s",
        "live_title": "Controllo live (modulatore in funzione)", "live_psd": "Titolo:", "live_artist": "Artista:",
        "live_msg": "Messaggio:", "live_send": "Invia", "live_gain": "Livello della stazione scelta (fading):",
        "live_sferics": "Sferics:", "live_amp": "Intensità", "live_ms": "ms", "live_fire": "Fulmine!",
        "set_title": "Impostazioni del modulatore", "set_bits": "Profondità DAC:", "set_rate": "Frequenza di campionamento (MSPS):",
        "set_name": "Nome stazione (SIS, max. 12 car.):", "set_slogan": "Slogan:", "set_msg": "Messaggio:",
        "set_country": "Paese (2 lettere):", "set_ptype": "Tipo di programma:", "set_md": "Profondità di modulazione analogica (0-1):",
        "set_adelay": "Ritardo analogico (s):", "set_level": "Livello portante 0-1 (vuoto = predefinito MA1 0,33 / MA3 0,09):",
        "set_bin": "Programma modulatore:", "set_sdrip": "IP SDR (ponte):", "set_sdrport": "Porta SDR:",
        "set_remote": "Modo fl2k remoto", "set_fl2k_auto": "Avvia automaticamente lo script fl2k", "set_fl2k": "Comando fl2k:",
        "set_note": "Nota: nome, slogan e messaggio valgono per tutte le stazioni (SIS del modulatore).",
        "dlg_save": "Salva",
    },
    "JA": {
        "win_title": "HD Radio AM (NRSC-5) マルチ送信制御",
        "col_freq": "周波数", "col_mode": "モード", "col_opts": "オプション", "col_name": "番組名", "col_url": "URL / パス",
        "btn_start": "全送信機を開始", "btn_stop": "全送信機を停止",
        "lbl_mod": "変調器:", "lbl_sdr": "SDR (fl2k_tcp / socat):",
        "menu_file": "ファイル", "menu_load": "読み込み", "menu_save": "保存", "menu_quit": "終了",
        "menu_sender": "送信局", "menu_add": "追加", "menu_edit": "編集", "menu_del": "削除",
        "menu_mod": "変調器", "menu_settings": "設定 ...", "menu_lang": "言語 (Language)",
        "dlg_title": "送信パラメータ", "dlg_plan": "周波数プラン:", "dlg_freq": "周波数:",
        "dlg_mode": "モード:", "mode_ma1": "MA1 ハイブリッド (アナログAM + デジタル)", "mode_ma3": "MA3 オールデジタル",
        "dlg_opts": "オプション:", "opt_aab": "アナログ音声 8 kHz (AAB)", "opt_pl": "副/第三搬送波の電力を上げる (PL)",
        "opt_hpp": "PIDS 電力を上げる (HPP)", "opt_rdb": "デジタル帯域を縮小 (RDB)",
        "dlg_occ": "占有帯域: 搬送波の周囲 約 ±%.1f kHz", "dlg_prog": "番組を選択:",
        "dlg_url": "番組URL / パス:", "dlg_browse": "ファイル ...", "dlg_btn": "適用", "dlg_cancel": "キャンセル",
        "err_freq": "周波数が重複しています！", "err_title": "エラー", "warn_title": "警告",
        "err_range": "周波数が無効です (100〜2400 kHz)。", "err_url": "URL またはファイルパスを入力してください。",
        "warn_overlap": "デジタル信号が次と重なります:\n%s\n\nそれでも適用しますか？",
        "err_max": "送信局は最大 %d 局です。", "err_bin": "変調器が見つかりません:\n%s",
        "live_title": "ライブ制御 (変調器の実行中のみ)", "live_psd": "タイトル:", "live_artist": "アーティスト:",
        "live_msg": "メッセージ:", "live_send": "送信", "live_gain": "選択局のレベル (フェージング):",
        "live_sferics": "空電:", "live_amp": "強さ", "live_ms": "ms", "live_fire": "雷！",
        "set_title": "変調器の設定", "set_bits": "DAC ビット深度:", "set_rate": "サンプルレート (MSPS):",
        "set_name": "局名 (SIS、最大12文字):", "set_slogan": "スローガン:", "set_msg": "メッセージ:",
        "set_country": "国 (2文字):", "set_ptype": "番組タイプ:", "set_md": "アナログ変調度 (0-1):",
        "set_adelay": "アナログ遅延 (秒):", "set_level": "搬送波レベル 0-1 (空欄 = 既定 MA1 0.33 / MA3 0.09):",
        "set_bin": "変調器プログラム:", "set_sdrip": "SDR の IP (ブリッジ):", "set_sdrport": "SDR ポート:",
        "set_remote": "リモート fl2k モード", "set_fl2k_auto": "fl2k スクリプトを自動起動", "set_fl2k": "fl2k コマンド:",
        "set_note": "注: 局名・スローガン・メッセージは全送信局で共通です (変調器の SIS)。",
        "dlg_save": "保存",
    },
}

def tr(code, key):
    return LANGUAGES.get(code, {}).get(key) or LANGUAGES["EN"].get(key) or key

# ---------------------------------------------------------------- Hilfsfunktionen
def clean(v):
    """Zahl (kHz) aus einem Text wie '603 kHz' ziehen."""
    m = re.search(r"(\d+(?:[.,]\d+)?)", str(v))
    return m.group(1).replace(",", ".") if m else "0"

def freq_khz(v):
    return float(clean(v))

def norm_opts(mode, opts):
    """Optionen als sortierte Komma-Liste in Grossbuchstaben; bei MA3 entfallen AAB und PL."""
    allowed = ("HPP", "RDB") if mode == "MA3" else ("AAB", "PL", "HPP", "RDB")
    have = {o.strip().upper() for o in re.split(r"[,\s;/+]+", str(opts)) if o.strip()}
    return ",".join(o for o in ("AAB", "PL", "HPP", "RDB") if o in have and o in allowed)

def build_station_args(rows, port_base=AUDIO_PORT_BASE):
    """rows: (freq, mode, opts, name, url) -> Argumentliste fuer start_sender_hd.sh (5er-Gruppen)."""
    args = []
    for i, (freq, mode, opts, name, url) in enumerate(rows):
        o = norm_opts(mode, opts).lower() or "-"
        args += [clean(freq), mode.lower(), o, url, str(port_base + i)]
    return args

def read_landscape(path):
    """CSV laden. Neues Format: Frequenz;Modus;Optionen;Name;URL. Altes Format (amtxgui):
    Frequenz;Bandbreite;Name;URL -> wird als MA1 ohne Optionen uebernommen (Bandbreite entfaellt)."""
    rows = []
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        for n, row in enumerate(csv.reader(f, delimiter=";")):
            row = [c.strip() for c in row]
            if not row or not any(row):
                continue
            if n == 0 and not re.search(r"\d", row[0]):   # Kopfzeile (beliebige Sprache)
                continue
            if len(row) >= 5:
                mode = row[1].upper() if row[1].upper() in ("MA1", "MA3") else "MA1"
                rows.append((row[0], mode, norm_opts(mode, row[2]), row[3], ";".join(row[4:])))
            elif len(row) == 4:
                rows.append((row[0], "MA1", "", row[2], row[3]))
    return rows

def write_landscape(path, rows, lang):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow([tr(lang, k) for k in ("col_freq", "col_mode", "col_opts", "col_name", "col_url")])
        for r in rows:
            w.writerow(r)

def udp_send(port, text):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.sendto(text.encode("utf-8", "replace"), ("127.0.0.1", port))
    finally:
        s.close()

def is_running(pattern, exact=False):
    try:
        return subprocess.run(["pgrep", "-x" if exact else "-f", pattern], capture_output=True).returncode == 0
    except Exception:
        return None

# ---------------------------------------------------------------- Sender-Dialog
class SenderDialog(tk.Toplevel):
    PLANS = {
        "Europa (9 kHz Raster)": [f"{f} kHz" for f in range(153, 280, 9)] + [f"{f} kHz" for f in range(531, 1603, 9)],
        "USA (10 kHz Raster)": [f"{f} kHz" for f in range(530, 1710, 10)],
        "CH HF-Telefonrundspruch": ["175 kHz", "208 kHz", "241 kHz", "274 kHz", "307 kHz", "340 kHz"],
        "IT Filodiffusione (RAI)": ["178 kHz", "211 kHz", "244 kHz", "277 kHz", "310 kHz", "343 kHz"],
        "Manuelle Eingabe": [],
    }

    def __init__(self, parent, station_db, others, lang_code="DE", initial=None):
        """others: Liste (freq_kHz, halbe Bandbreite, Beschriftung) der uebrigen Sender."""
        super().__init__(parent)
        self.lang_code = lang_code
        self.t = lambda k: tr(lang_code, k)
        self.title(self.t("dlg_title"))
        self.station_db, self.others = station_db, others
        self.result = None
        self.transient(parent); self.wait_visibility(); self.grab_set()

        self.var_plan = tk.StringVar(value="Europa (9 kHz Raster)")
        self.var_mode = tk.StringVar(value="MA1")
        self.var_opt = {k: tk.BooleanVar(value=False) for k in ("AAB", "PL", "HPP", "RDB")}

        pad = dict(padx=10, pady=4)
        tk.Label(self, text=self.t("dlg_plan")).grid(row=0, column=0, sticky="w", **pad)
        self.cb_plan = ttk.Combobox(self, values=list(self.PLANS), textvariable=self.var_plan, state="readonly", width=38)
        self.cb_plan.grid(row=0, column=1, columnspan=2, sticky="w", **pad)
        self.cb_plan.bind("<<ComboboxSelected>>", self.update_freq_list)

        tk.Label(self, text=self.t("dlg_freq")).grid(row=1, column=0, sticky="w", **pad)
        self.cb_freq = ttk.Combobox(self, width=38)
        self.cb_freq.grid(row=1, column=1, columnspan=2, sticky="w", **pad)

        tk.Label(self, text=self.t("dlg_mode")).grid(row=2, column=0, sticky="nw", **pad)
        mf = tk.Frame(self); mf.grid(row=2, column=1, columnspan=2, sticky="w", **pad)
        for val, key in (("MA1", "mode_ma1"), ("MA3", "mode_ma3")):
            tk.Radiobutton(mf, text=self.t(key), variable=self.var_mode, value=val, command=self.mode_changed).pack(anchor="w")

        tk.Label(self, text=self.t("dlg_opts")).grid(row=3, column=0, sticky="nw", **pad)
        of = tk.Frame(self); of.grid(row=3, column=1, columnspan=2, sticky="w", **pad)
        self.chk = {}
        for k, key in (("AAB", "opt_aab"), ("PL", "opt_pl"), ("HPP", "opt_hpp"), ("RDB", "opt_rdb")):
            self.chk[k] = tk.Checkbutton(of, text=self.t(key), variable=self.var_opt[k], command=self.update_occ)
            self.chk[k].pack(anchor="w")
        self.lbl_occ = tk.Label(self, fg="#555555"); self.lbl_occ.grid(row=4, column=1, columnspan=2, sticky="w", padx=10)

        tk.Label(self, text=self.t("dlg_prog")).grid(row=5, column=0, sticky="w", **pad)
        self.cb_name = ttk.Combobox(self, values=sorted(self.station_db), width=38)
        self.cb_name.grid(row=5, column=1, columnspan=2, sticky="w", **pad)
        self.cb_name.bind("<<ComboboxSelected>>", self.autofill_url)

        tk.Label(self, text=self.t("dlg_url")).grid(row=6, column=0, sticky="w", **pad)
        self.ent_url = tk.Entry(self, width=41); self.ent_url.grid(row=6, column=1, sticky="w", **pad)
        tk.Button(self, text=self.t("dlg_browse"), command=self.browse).grid(row=6, column=2, sticky="w", padx=(0, 10))

        bf = tk.Frame(self); bf.grid(row=7, column=0, columnspan=3, pady=14)
        tk.Button(bf, text=self.t("dlg_btn"), command=self.confirm, bg="#d5e8d4", width=14).pack(side=tk.LEFT, padx=8)
        tk.Button(bf, text=self.t("dlg_cancel"), command=self.destroy, width=14).pack(side=tk.LEFT, padx=8)

        self.update_freq_list()
        if initial:
            freq, mode, opts, name, url = initial
            self.var_plan.set("Manuelle Eingabe")           # verhindert Ueberschreiben der Frequenz
            self.cb_freq.set(freq)
            self.var_mode.set(mode)
            for k in self.var_opt:
                self.var_opt[k].set(k in norm_opts(mode, opts).split(","))
            self.cb_name.set(name); self.ent_url.insert(0, url)
        self.mode_changed()
        self.bind("<Return>", lambda e: self.confirm())
        self.bind("<Escape>", lambda e: self.destroy())

    def update_freq_list(self, e=None):
        vals = self.PLANS[self.var_plan.get()]
        self.cb_freq["values"] = vals
        if vals:
            self.cb_freq.set(vals[0])

    def mode_changed(self):
        ma3 = self.var_mode.get() == "MA3"
        for k in ("AAB", "PL"):                       # nur bei MA1 sinnvoll
            if ma3:
                self.var_opt[k].set(False)
            self.chk[k].config(state=tk.DISABLED if ma3 else tk.NORMAL)
        self.update_occ()

    def selected_opts(self):
        return ",".join(k for k in ("AAB", "PL", "HPP", "RDB") if self.var_opt[k].get())

    def update_occ(self):
        self.lbl_occ.config(text=self.t("dlg_occ") % half_bandwidth(self.var_mode.get(), self.selected_opts()))

    def autofill_url(self, e=None):
        name = self.cb_name.get()
        if name in self.station_db:
            self.ent_url.delete(0, tk.END); self.ent_url.insert(0, self.station_db[name])

    def browse(self):
        p = filedialog.askopenfilename(parent=self, filetypes=(("Audio", "*.mp3 *.wav *.flac *.ogg *.opus *.m4a *.aac *.m3u *.pls"), ("*", "*.*")))
        if p:
            self.ent_url.delete(0, tk.END); self.ent_url.insert(0, p)
            if not self.cb_name.get().strip():
                self.cb_name.set(os.path.splitext(os.path.basename(p))[0])

    def confirm(self):
        t = self.t
        try:
            f = float(clean(self.cb_freq.get()))
        except ValueError:
            f = 0
        if not 100 <= f <= 2400:
            messagebox.showerror(t("err_title"), t("err_range"), parent=self); return
        if any(abs(f - o[0]) < 0.5 for o in self.others):
            messagebox.showerror(t("err_title"), t("err_freq"), parent=self); return
        url = self.ent_url.get().strip()
        if not url:
            messagebox.showerror(t("err_title"), t("err_url"), parent=self); return
        mode, opts = self.var_mode.get(), norm_opts(self.var_mode.get(), self.selected_opts())
        hw = half_bandwidth(mode, opts)
        clash = [f"{o[2]} ({o[0]:g} kHz)" for o in self.others if abs(f - o[0]) < hw + o[1]]
        if clash and not messagebox.askyesno(t("warn_title"), t("warn_overlap") % "\n".join(clash), parent=self):
            return
        name = self.cb_name.get().strip() or os.path.splitext(os.path.basename(url.rstrip("/")))[0] or url
        self.result = (f"{f:g} kHz", mode, opts, name, url)
        self.destroy()

# ---------------------------------------------------------------- Einstellungen
DEFAULT_SETTINGS = {
    "lang": "DE", "bits": 8, "rate": "10", "name": "HDAM", "slogan": "HD Radio AM Modulator",
    "message": "5 MSPS Integer NRSC-5 MA1/MA3", "country": "DE", "ptype": 0, "md": "0.85", "adelay": "5.5",
    "level": "", "bin": "./hdradio_am", "sdr_ip": "127.0.0.1", "sdr_port": "1234", "remote": False,
    "fl2k_auto": True, "fl2k_cmd": "sudo ./start_fl2k.sh",
}

class SettingsDialog(tk.Toplevel):
    def __init__(self, parent, settings, lang_code):
        super().__init__(parent)
        self.t = lambda k: tr(lang_code, k)
        self.title(self.t("set_title"))
        self.transient(parent); self.wait_visibility(); self.grab_set()
        self.s = dict(settings); self.result = None
        self.v = {}
        rows = [("bits", "set_bits", "combo", [str(b) for b in range(8, 17)]),
                ("rate", "set_rate", "combo", ["5", "10"]),
                ("name", "set_name", "entry", None), ("slogan", "set_slogan", "entry", None), ("message", "set_msg", "entry", None),
                ("country", "set_country", "entry", None), ("ptype", "set_ptype", "ptype", None),
                ("md", "set_md", "entry", None), ("adelay", "set_adelay", "entry", None), ("level", "set_level", "entry", None),
                ("bin", "set_bin", "entry", None), ("sdr_ip", "set_sdrip", "entry", None), ("sdr_port", "set_sdrport", "entry", None),
                ("remote", "set_remote", "check", None), ("fl2k_auto", "set_fl2k_auto", "check", None), ("fl2k_cmd", "set_fl2k", "entry", None)]
        for i, (key, label, kind, vals) in enumerate(rows):
            if kind == "check":
                var = tk.BooleanVar(value=bool(self.s.get(key)))
                tk.Checkbutton(self, text=self.t(label), variable=var).grid(row=i, column=0, columnspan=2, sticky="w", padx=10)
            else:
                tk.Label(self, text=self.t(label)).grid(row=i, column=0, sticky="w", padx=10, pady=3)
                if kind == "combo":
                    var = tk.StringVar(value=str(self.s.get(key))); ttk.Combobox(self, textvariable=var, values=vals, width=12, state="readonly").grid(row=i, column=1, sticky="w", padx=10)
                elif kind == "ptype":
                    names = [f"{n} - {s}" for n, s in PROGRAM_TYPES]
                    cur = int(self.s.get(key, 0)); var = tk.StringVar(value=next((x for x in names if x.startswith(f"{cur} -")), names[0]))
                    ttk.Combobox(self, textvariable=var, values=names, state="readonly", width=28).grid(row=i, column=1, sticky="w", padx=10)
                else:
                    var = tk.StringVar(value=str(self.s.get(key, ""))); tk.Entry(self, textvariable=var, width=38).grid(row=i, column=1, sticky="w", padx=10)
            self.v[key] = var
        tk.Label(self, text=self.t("set_note"), fg="#555555", wraplength=460, justify="left").grid(row=len(rows), column=0, columnspan=2, padx=10, pady=8, sticky="w")
        tk.Button(self, text=self.t("dlg_save"), bg="#d5e8d4", width=14, command=self.ok).grid(row=len(rows) + 1, column=0, columnspan=2, pady=10)

    def ok(self):
        out = {}
        for k, var in self.v.items():
            val = var.get()
            if k == "ptype":
                val = int(str(val).split(" - ")[0])
            elif k == "bits":
                val = int(val)
            out[k] = val
        out["country"] = (out["country"].strip().upper() + "DE")[:2] if out["country"].strip() else "DE"
        self.result = out
        self.destroy()

# ---------------------------------------------------------------- Hauptprogramm
class RadioApp:
    def __init__(self, root):
        self.root = root
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        self.settings_path = os.path.join(self.base_dir, "hdtx_settings.json")
        self.settings = dict(DEFAULT_SETTINGS)
        self.load_settings()
        self.current_lang = self.settings.get("lang", "DE")
        self.station_db = self.load_stations_db()
        self.main_proc = None
        self.gains = {}                       # Frequenz(Hz) -> aktueller Pegel (nur Sitzung)

        self.tree = ttk.Treeview(root, show="headings", selectmode="browse", height=8)
        self.tree["columns"] = ("Frequenz", "Modus", "Optionen", "Programmname", "URL")
        self.tree.pack(fill=tk.BOTH, expand=True, padx=10, pady=(10, 4))
        self.tree.bind("<Double-1>", self.edit_entry)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)
        self.tree.bind("<Delete>", lambda e: self.delete_entry())

        # Live-Steuerung
        self.live = tk.LabelFrame(root)
        self.live.pack(fill=tk.X, padx=10, pady=4)
        self.var_title, self.var_artist, self.var_msg = tk.StringVar(), tk.StringVar(), tk.StringVar()
        self.lbl_psd = tk.Label(self.live); self.lbl_psd.grid(row=0, column=0, sticky="w", padx=4)
        tk.Entry(self.live, textvariable=self.var_title, width=22).grid(row=0, column=1, padx=4, pady=2)
        self.lbl_artist = tk.Label(self.live); self.lbl_artist.grid(row=0, column=2, sticky="w", padx=4)
        tk.Entry(self.live, textvariable=self.var_artist, width=22).grid(row=0, column=3, padx=4)
        self.btn_psd = tk.Button(self.live, command=self.send_psd); self.btn_psd.grid(row=0, column=4, padx=6)
        self.lbl_msg = tk.Label(self.live); self.lbl_msg.grid(row=1, column=0, sticky="w", padx=4)
        tk.Entry(self.live, textvariable=self.var_msg, width=52).grid(row=1, column=1, columnspan=3, sticky="we", padx=4, pady=2)
        self.btn_msg = tk.Button(self.live, command=self.send_message); self.btn_msg.grid(row=1, column=4, padx=6)
        self.lbl_gain = tk.Label(self.live); self.lbl_gain.grid(row=2, column=0, columnspan=2, sticky="w", padx=4)
        self.scale = tk.Scale(self.live, from_=0.0, to=1.5, resolution=0.05, orient=tk.HORIZONTAL, length=240)
        self.scale.set(1.0); self.scale.grid(row=2, column=2, columnspan=2, sticky="w", padx=4)
        self.scale.bind("<ButtonRelease-1>", self.send_gain)
        self.lbl_sf = tk.Label(self.live); self.lbl_sf.grid(row=3, column=0, sticky="w", padx=4)
        sf = tk.Frame(self.live); sf.grid(row=3, column=1, columnspan=3, sticky="w")
        self.lbl_amp = tk.Label(sf); self.lbl_amp.pack(side=tk.LEFT)
        self.var_amp = tk.StringVar(value="800"); tk.Spinbox(sf, from_=50, to=5000, increment=50, textvariable=self.var_amp, width=6).pack(side=tk.LEFT, padx=4)
        self.lbl_ms = tk.Label(sf); self.lbl_ms.pack(side=tk.LEFT)
        self.var_ms = tk.StringVar(value="120"); tk.Spinbox(sf, from_=10, to=2000, increment=10, textvariable=self.var_ms, width=6).pack(side=tk.LEFT, padx=4)
        self.btn_sf = tk.Button(self.live, command=self.send_sferics); self.btn_sf.grid(row=3, column=4, padx=6)

        # Buttons & Ampeln
        btn_panel = tk.Frame(root); btn_panel.pack(fill=tk.X, padx=10, pady=10)
        self.btn_start = tk.Button(btn_panel, bg="#90ee90", command=self.start_all, width=24, height=2); self.btn_start.pack(side=tk.LEFT, padx=5)
        self.btn_stop = tk.Button(btn_panel, bg="#ffcccb", command=self.stop_all, width=24, height=2); self.btn_stop.pack(side=tk.LEFT, padx=5)
        ampel_frame = tk.Frame(btn_panel); ampel_frame.pack(side=tk.RIGHT, padx=10)
        self.lbl_mod = tk.Label(ampel_frame, font=("Arial", 10)); self.lbl_mod.grid(row=0, column=0, sticky="e")
        self.ampel_mod = tk.Canvas(ampel_frame, width=26, height=26, highlightthickness=0); self.light_mod = self.ampel_mod.create_oval(4, 4, 22, 22, fill="red")
        self.ampel_mod.grid(row=0, column=1, padx=5)
        self.lbl_sdr = tk.Label(ampel_frame, font=("Arial", 10)); self.lbl_sdr.grid(row=1, column=0, sticky="e")
        self.ampel_sdr = tk.Canvas(ampel_frame, width=26, height=26, highlightthickness=0); self.light_sdr = self.ampel_sdr.create_oval(4, 4, 22, 22, fill="red")
        self.ampel_sdr.grid(row=1, column=1, padx=5)

        self.menubar = tk.Menu(root); root.config(menu=self.menubar)
        root.protocol("WM_DELETE_WINDOW", self.quit_app)
        self.update_ui_language()
        self.check_status()

    # ---- Sprache / Menues
    def t(self, key):
        return tr(self.current_lang, key)

    def update_ui_language(self):
        t = self.t
        self.root.title(t("win_title"))
        for col, key, w in zip(self.tree["columns"], ("col_freq", "col_mode", "col_opts", "col_name", "col_url"), (100, 70, 110, 230, 420)):
            self.tree.heading(col, text=t(key)); self.tree.column(col, width=w)
        self.btn_start.config(text=t("btn_start")); self.btn_stop.config(text=t("btn_stop"))
        self.lbl_mod.config(text=t("lbl_mod")); self.lbl_sdr.config(text=t("lbl_sdr"))
        self.live.config(text=t("live_title"))
        self.lbl_psd.config(text=t("live_psd")); self.lbl_artist.config(text=t("live_artist")); self.lbl_msg.config(text=t("live_msg"))
        self.btn_psd.config(text=t("live_send")); self.btn_msg.config(text=t("live_send"))
        self.lbl_gain.config(text=t("live_gain")); self.lbl_sf.config(text=t("live_sferics"))
        self.lbl_amp.config(text=t("live_amp")); self.lbl_ms.config(text=t("live_ms")); self.btn_sf.config(text=t("live_fire"))
        self.menubar.delete(0, tk.END)
        fm = tk.Menu(self.menubar, tearoff=0)
        fm.add_command(label=t("menu_load"), command=self.load_csv); fm.add_command(label=t("menu_save"), command=self.save_csv)
        fm.add_separator(); fm.add_command(label=t("menu_quit"), command=self.quit_app)
        self.menubar.add_cascade(label=t("menu_file"), menu=fm)
        sm = tk.Menu(self.menubar, tearoff=0)
        sm.add_command(label=t("menu_add"), command=self.add_entry); sm.add_command(label=t("menu_edit"), command=self.edit_entry)
        sm.add_command(label=t("menu_del"), command=self.delete_entry)
        self.menubar.add_cascade(label=t("menu_sender"), menu=sm)
        mm = tk.Menu(self.menubar, tearoff=0); mm.add_command(label=t("menu_settings"), command=self.open_settings)
        self.menubar.add_cascade(label=t("menu_mod"), menu=mm)
        lm = tk.Menu(self.menubar, tearoff=0)
        for code, name in (("DE", "Deutsch"), ("EN", "English"), ("FR", "Français"), ("IT", "Italiano"), ("JA", "日本語")):
            lm.add_command(label=name, command=lambda c=code: self.set_language(c))
        self.menubar.add_cascade(label=t("menu_lang"), menu=lm)

    def set_language(self, code):
        self.current_lang = code; self.settings["lang"] = code; self.save_settings(); self.update_ui_language()

    # ---- Einstellungen / Datenbank
    def load_settings(self):
        try:
            with open(self.settings_path, "r", encoding="utf-8") as f:
                self.settings.update(json.load(f))
        except (OSError, ValueError):
            pass

    def save_settings(self):
        try:
            with open(self.settings_path, "w", encoding="utf-8") as f:
                json.dump(self.settings, f, indent=2, ensure_ascii=False)
        except OSError:
            pass

    def open_settings(self):
        d = SettingsDialog(self.root, self.settings, self.current_lang); self.root.wait_window(d)
        if d.result:
            self.settings.update(d.result); self.save_settings()

    def load_stations_db(self):
        """stations.db: Zeilen 'Name,URL' (wie bisher). Kommas in der URL bleiben erhalten."""
        db = {}
        path = os.path.join(self.base_dir, "stations.db")
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for row in csv.reader(f):
                    if len(row) >= 2 and row[0].strip():
                        db[row[0].strip()] = ",".join(row[1:]).strip()
        return db

    # ---- Tabelle
    def rows(self):
        return [tuple(self.tree.item(k)["values"]) for k in self.tree.get_children()]

    def sort_tree(self):
        items = sorted(((freq_khz(self.tree.set(k, "Frequenz")), k) for k in self.tree.get_children()))
        for i, (_, k) in enumerate(items):
            self.tree.move(k, "", i)

    def others(self, skip=None):
        out = []
        for k in self.tree.get_children():
            if k == skip:
                continue
            v = self.tree.item(k)["values"]
            out.append((freq_khz(v[0]), half_bandwidth(str(v[1]), str(v[2])), str(v[3])))
        return out

    def add_entry(self):
        if len(self.tree.get_children()) >= MAX_STATIONS:
            messagebox.showerror(self.t("err_title"), self.t("err_max") % MAX_STATIONS); return
        d = SenderDialog(self.root, self.station_db, self.others(), self.current_lang); self.root.wait_window(d)
        if d.result:
            self.tree.insert("", tk.END, values=d.result); self.sort_tree()

    def edit_entry(self, event=None):
        sel = self.tree.selection()
        if not sel:
            return
        item = sel[0]
        d = SenderDialog(self.root, self.station_db, self.others(skip=item), self.current_lang, initial=self.tree.item(item, "values"))
        self.root.wait_window(d)
        if d.result:
            self.tree.item(item, values=d.result); self.sort_tree()

    def delete_entry(self):
        for i in self.tree.selection():
            self.tree.delete(i)

    # ---- CSV
    def save_csv(self):
        p = filedialog.asksaveasfilename(defaultextension=".csv", filetypes=(("CSV Files", "*.csv"),))
        if p:
            write_landscape(p, self.rows(), self.current_lang)

    def load_csv(self):
        p = filedialog.askopenfilename(filetypes=(("CSV Files", "*.csv"), ("*", "*.*")))
        if not p:
            return
        try:
            rows = read_landscape(p)
        except (OSError, UnicodeDecodeError, csv.Error) as e:
            messagebox.showerror(self.t("err_title"), str(e)); return
        for i in self.tree.get_children():
            self.tree.delete(i)
        for r in rows:
            self.tree.insert("", tk.END, values=r)
        self.sort_tree()
        if len(rows) > MAX_STATIONS:
            messagebox.showwarning(self.t("warn_title"), self.t("err_max") % MAX_STATIONS)

    # ---- Start / Stop
    def env(self):
        s = self.settings
        e = dict(os.environ)
        e.update({"SDR_DAC_BITS": str(s["bits"]), "SDR_SAMPLERATE": str(s["rate"]), "SDR_IP": s["sdr_ip"], "SDR_PORT": str(s["sdr_port"]),
                  "SDR_REMOTE_FL2k": "true" if s["remote"] else "false", "HDTX_BIN": s["bin"], "HDTX_NAME": s["name"],
                  "HDTX_SLOGAN": s["slogan"], "HDTX_MESSAGE": s["message"], "HDTX_COUNTRY": s["country"], "HDTX_PTYPE": str(s["ptype"]),
                  "HDTX_MD": str(s["md"]), "HDTX_ADELAY": str(s["adelay"]), "HDTX_LEVEL": str(s["level"])})
        return e

    def start_all(self):
        rows = self.rows()
        if not rows:
            return
        if len(rows) > MAX_STATIONS:
            messagebox.showerror(self.t("err_title"), self.t("err_max") % MAX_STATIONS); return
        binpath = self.settings["bin"]
        if not os.path.isabs(binpath):
            binpath = os.path.join(self.base_dir, binpath)
        if not os.path.exists(binpath):
            messagebox.showerror(self.t("err_title"), self.t("err_bin") % binpath); return
        if self.main_proc:
            self.stop_all()
        env = self.env(); env["HDTX_BIN"] = binpath
        script = os.path.join(self.base_dir, "start_sender_hd.sh")
        cmd = ["/bin/bash", script] + build_station_args([(str(r[0]), str(r[1]), str(r[2]), str(r[3]), str(r[4])) for r in rows])
        self.gains.clear(); self.scale.set(1.0)
        try:
            if shutil.which("xterm"):
                self.main_proc = subprocess.Popen(["xterm", "-T", "HD-AM-Broadcaster", "-e"] + cmd, cwd=self.base_dir, env=env)
            else:                                           # ohne xterm: Ausgabe in Logdatei
                self.main_proc = subprocess.Popen(cmd, cwd=self.base_dir, env=env, stdin=subprocess.DEVNULL,
                                                  stdout=open(os.path.join(self.base_dir, "hdtx.log"), "w"), stderr=subprocess.STDOUT)
            if self.settings.get("fl2k_auto") and self.settings.get("fl2k_cmd", "").strip():
                fl2k = self.settings["fl2k_cmd"]
                self.root.after(3000, lambda: self.start_fl2k(fl2k))   # Modulator soll zuerst auf Port 12345 lauschen
        except Exception as e:
            messagebox.showerror(self.t("err_title"), str(e))

    def start_fl2k(self, cmd):
        if not (self.main_proc and self.main_proc.poll() is None):
            return
        try:
            if shutil.which("xterm"):
                subprocess.Popen(["xterm", "-T", "fl2k_tcp DAC Transfer", "-e", "/bin/bash", "-c", cmd], cwd=self.base_dir)
                #subprocess.Popen(["xterm", "-T", "fl2k_tcp DAC Transfer", "-e", "/bin/bash", "-c", "./start_fl2k.sh"])
            else:
                subprocess.Popen(["/bin/bash", "-c", cmd], cwd=self.base_dir)
        except Exception as e:
            messagebox.showerror(self.t("err_title"), str(e))

    def stop_all(self):
        """xterm beenden -> das Skript bekommt SIGHUP -> start_sender_hd.sh beendet ffmpeg, Modulator, socat."""
        if self.main_proc:
            try:
                self.main_proc.terminate()
                self.main_proc.wait(timeout=4)
            except Exception:
                try:
                    self.main_proc.kill()
                except Exception:
                    pass
            self.main_proc = None
        subprocess.run(["pkill", "-x", "hdradio_am"], capture_output=True)   # Nachzuegler/Sicherheitsnetz

    def quit_app(self):
        if self.main_proc and self.main_proc.poll() is None:
            self.stop_all()
        self.root.destroy()

    def check_status(self):
        for canvas, light, pat, exact in ((self.ampel_mod, self.light_mod, "hdradio_am", True), (self.ampel_sdr, self.light_sdr, "fl2k_tcp|socat", False)):
            r = is_running(pat, exact)
            canvas.itemconfig(light, fill="gray" if r is None else ("lime" if r else "red"))
        self.root.after(1000, self.check_status)

    # ---- Live-Steuerung (UDP-Steuerports des Modulators)
    def on_select(self, event=None):
        sel = self.tree.selection()
        if sel:
            hz = int(round(freq_khz(self.tree.set(sel[0], "Frequenz")) * 1000))
            self.scale.set(self.gains.get(hz, 1.0))

    def send_gain(self, event=None):
        sel = self.tree.selection()
        if not sel:
            return
        hz = int(round(freq_khz(self.tree.set(sel[0], "Frequenz")) * 1000))
        g = float(self.scale.get()); self.gains[hz] = g
        udp_send(CTRL_PORT, f"{hz}:{g:.3f}")

    def send_psd(self):
        udp_send(CTRL_PORT, "title=" + self.var_title.get()); udp_send(CTRL_PORT, "artist=" + self.var_artist.get())

    def send_message(self):
        udp_send(CTRL_PORT, "message=" + self.var_msg.get())

    def send_sferics(self):
        try:
            udp_send(SFERICS_PORT, f"{float(self.var_amp.get()):g}:{float(self.var_ms.get()):g}")
        except ValueError:
            pass

if __name__ == "__main__":
    root = tk.Tk()
    app = RadioApp(root)
    root.mainloop()
