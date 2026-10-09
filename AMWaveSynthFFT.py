import tkinter as tk
from tkinter import ttk
import socket
import threading
import numpy as np
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
from collections import deque
import time

# --- KONFIGURATION ---
TCP_IP = "127.0.0.1"
TCP_PORT = 12345
FFT_SIZE = 2048      
WATERFALL_HISTORY = 100

class App:
    def __init__(self, root):
        self.root = root
        self.root.title("SDR Monitor")
        
        self.data_queue = deque(maxlen=15)
        self.running = True
        
        # Einstellungen
        self.sample_rate = tk.DoubleVar(value=10.0)
        self.bit_depth = tk.IntVar(value=8)
        
        self.update_freq_axis()
        self.num_bins = len(self.freqs_khz)
        
        # Daten-Speicher
        self.waterfall_data = np.full((WATERFALL_HISTORY, self.num_bins), -120.0)
        self.peak_hold_data = np.full(self.num_bins, -120.0)
        self.show_peak_hold = tk.BooleanVar(value=True)
        
        self.setup_ui()
        
        self.thread = threading.Thread(target=self.tcp_reader, daemon=True)
        self.thread.start()
        self.update_plot()

    def update_freq_axis(self):
        sr = self.sample_rate.get() * 1e6
        self.freqs_khz = np.fft.rfftfreq(FFT_SIZE, 1/sr) / 1e3

    def setup_ui(self):
        self.main_frame = tk.Frame(self.root)
        self.main_frame.pack(fill=tk.BOTH, expand=1)
        
        self.plot_frame = tk.Frame(self.main_frame)
        self.plot_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=1)
        
        self.ctrl_frame = tk.Frame(self.main_frame, width=220, padx=10, pady=10, relief=tk.RIDGE, borderwidth=1)
        self.ctrl_frame.pack(side=tk.RIGHT, fill=tk.Y)
        self.ctrl_frame.pack_propagate(False)

        # --- MATPLOTLIB SETUP ---
        self.fig = Figure(figsize=(8, 8), dpi=100)
        self.fig.subplots_adjust(hspace=0.3, bottom=0.1)
        
        self.ax_fft = self.fig.add_subplot(211)
        self.ax_fft.set_ylabel("dBFS")
        self.ax_fft.grid(True, alpha=0.3)
        self.ax_fft.set_ylim(-120, 10) 
        self.ax_fft.set_facecolor('black')
        
        self.line, = self.ax_fft.plot(self.freqs_khz, np.zeros(self.num_bins), lw=1, color='cyan')
        self.peak_line, = self.ax_fft.plot(self.freqs_khz, np.zeros(self.num_bins), lw=1, color='red', alpha=0.5)
        
        self.ax_wf = self.fig.add_subplot(212, sharex=self.ax_fft)
        self.ax_wf.set_xlabel("Frequenz (kHz)")
        self.im = self.ax_wf.imshow(self.waterfall_data, aspect='auto', 
                                    extent=[self.freqs_khz[0], self.freqs_khz[-1], WATERFALL_HISTORY, 0], 
                                    cmap='magma', vmin=-100, vmax=-20)
        
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.plot_frame)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=1)
        
        self.toolbar = NavigationToolbar2Tk(self.canvas, self.plot_frame)
        self.toolbar.update()

        # --- CONTROLS (RECHTS) ---
        tk.Label(self.ctrl_frame, text="BIT-TIEFE", font=('Arial', 10, 'bold')).pack(pady=5)
        
        # Erweiterte Bit-Auswahl
        for b in [8, 10, 12, 14, 16]:
            ttk.Radiobutton(self.ctrl_frame, text=f"{b} Bit", variable=self.bit_depth, value=b, 
                            command=self.reset_peak).pack(anchor=tk.W)

        tk.Label(self.ctrl_frame, text="\nSAMPLERATE (MSPS):").pack(anchor=tk.W)
        self.sr_combo = ttk.Combobox(self.ctrl_frame, textvariable=self.sample_rate, values=[5.0, 10.0, 12.5, 25.0])
        self.sr_combo.pack(fill=tk.X)
        tk.Button(self.ctrl_frame, text="Achse aktualisieren", command=self.reset_axis).pack(fill=tk.X, pady=2)

        tk.Label(self.ctrl_frame, text="").pack()

        self.cursor_label = tk.Label(self.ctrl_frame, text="Frequenz: --- kHz\nPegel: --- dB", 
                                     justify=tk.LEFT, fg="darkblue", font=('Courier', 10), bg="#eee", padx=5, pady=5)
        self.cursor_label.pack(fill=tk.X, pady=10)

        tk.Checkbutton(self.ctrl_frame, text="Peak Hold", variable=self.show_peak_hold).pack(anchor=tk.W)
        tk.Button(self.ctrl_frame, text="Reset Peak", command=self.reset_peak).pack(fill=tk.X, pady=5)

        tk.Label(self.ctrl_frame, text="\nWf Max (Helligkeit):").pack(anchor=tk.W)
        self.vmax_scale = tk.Scale(self.ctrl_frame, from_=-50, to=20, orient=tk.HORIZONTAL)
        self.vmax_scale.set(-20)
        self.vmax_scale.pack(fill=tk.X)

        tk.Label(self.ctrl_frame, text="Wf Range (Kontrast):").pack(anchor=tk.W)
        self.vrange_scale = tk.Scale(self.ctrl_frame, from_=20, to=120, orient=tk.HORIZONTAL)
        self.vrange_scale.set(80)
        self.vrange_scale.pack(fill=tk.X)

        self.canvas.mpl_connect('button_press_event', self.on_click)

    def reset_axis(self):
        self.update_freq_axis()
        self.line.set_xdata(self.freqs_khz)
        self.peak_line.set_xdata(self.freqs_khz)
        self.ax_fft.set_xlim(self.freqs_khz[0], self.freqs_khz[-1])
        self.im.set_extent([self.freqs_khz[0], self.freqs_khz[-1], WATERFALL_HISTORY, 0])
        self.reset_peak()

    def on_click(self, event):
        if event.inaxes:
            self.cursor_label.config(text=f"Frequenz: {event.xdata:.1f} kHz\nPegel: {event.ydata:.1f} dBFS")

    def reset_peak(self):
        self.peak_hold_data = np.full(self.num_bins, -120.0)

    def tcp_reader(self):
        raw_buffer = bytearray()
        while self.running:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(2.0)
                sock.connect((TCP_IP, TCP_PORT))
                while self.running:
                    b_depth = self.bit_depth.get()
                    bytes_per_sample = 1 if b_depth == 8 else 2
                    required_size = FFT_SIZE * bytes_per_sample
                    
                    chunk = sock.recv(8192)
                    if not chunk: break
                    raw_buffer.extend(chunk)
                    
                    while len(raw_buffer) >= required_size:
                        data_block = raw_buffer[:required_size]
                        del raw_buffer[:required_size]
                        
                        if bytes_per_sample == 1:
                            samples = np.frombuffer(data_block, dtype=np.int8).astype(np.float32)
                        else:
                            samples = np.frombuffer(data_block, dtype=np.int16).astype(np.float32)
                        
                        # Korrektur für Bittiefe: Normalisierung auf Bereich -1.0 bis +1.0
                        # Ein 10-Bit Signal hat Max-Wert 511, ein 16-Bit Signal 32767.
                        norm_val = 2**(b_depth - 1)
                        self.data_queue.append(samples / norm_val)
            except: 
                time.sleep(1)
            finally: 
                sock.close()

    def update_plot(self):
        if self.data_queue:
            samples = self.data_queue.popleft()
            
            # FFT mit Windowing
            windowed = samples * np.hanning(len(samples))
            fft_res = np.fft.rfft(windowed)
            
            # Amplitude in dB Full Scale (dBFS)
            # Da samples bereits auf -1.0...1.0 normiert ist, entspricht 
            # der Peak eines Sinus exakt 0 dB (nach Korrektur des Window-Verlusts).
            mag = np.abs(fft_res) / (len(samples) / 4) # /4 ist grobe Korrektur für rfft+hanning
            fft_db = 20 * np.log10(mag + 1e-12) 

            fft_db = fft_db[:self.num_bins]

            self.peak_hold_data = np.maximum(self.peak_hold_data, fft_db)
            self.line.set_ydata(fft_db)
            
            if self.show_peak_hold.get():
                self.peak_line.set_ydata(self.peak_hold_data)
                self.peak_line.set_visible(True)
            else:
                self.peak_line.set_visible(False)
            
            self.waterfall_data = np.roll(self.waterfall_data, 1, axis=0)
            self.waterfall_data[0, :] = fft_db
            self.im.set_array(self.waterfall_data)
            
            v_max = self.vmax_scale.get()
            v_min = v_max - self.vrange_scale.get()
            self.im.set_clim(vmin=v_min, vmax=v_max)
            
            self.canvas.draw_idle()
            
        self.root.after(30, self.update_plot)

if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("1200x850")
    app = App(root)
    root.mainloop()