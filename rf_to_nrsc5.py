#!/usr/bin/env python3
"""RF (int8/int16, 5 MSPS) -> komplexes Basisband 46511.71875 Hz (cs16) fuer nrsc5 --am"""
import sys, numpy as np
from scipy.signal import decimate, resample, welch
inp, fc, bits, outp = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
skip = float(sys.argv[5]) if len(sys.argv)>5 else 0.0
dt = np.int8 if bits==8 else np.int16
x = np.fromfile(inp, dtype=dt).astype(np.float32)
fs = 5e6
x = x[int(skip*fs):]
n = np.arange(len(x))
# in Stuecken mischen (Phasen-Praezision)
bb = np.empty(len(x), dtype=np.complex64)
for i in range(0, len(x), 1_000_000):
    k = np.arange(i, min(i+1_000_000, len(x)))
    bb[i:i+len(k)] = x[k] * np.exp(-2j*np.pi*((fc*k/fs) % 1.0)).astype(np.complex64)
bb = decimate(bb, 4, ftype='fir', zero_phase=True)      # 1.25 MHz
bb = decimate(bb, 5, ftype='fir', zero_phase=True)      # 250 kHz
N = (len(bb)//64000)*64000
bb = bb[:N]
y = resample(bb, N*11907//64000)                        # 46511.71875 Hz
y = y/np.max(np.abs(y))*12000
o = np.empty(2*len(y), dtype='<i2'); o[0::2]=np.round(y.real); o[1::2]=np.round(y.imag)
o.tofile(outp)
f,p = welch(y, fs=46511.71875, nperseg=4096, return_onesided=False)
f=np.fft.fftshift(f); p=np.fft.fftshift(p); pdb=10*np.log10(p/p.max()+1e-20)
print("Samples:",len(y),"Dauer %.2f s"%(len(y)/46511.71875))
for fo in (-14000,-12000,-10000,-8000,-6000,-4000,-2000,0,2000,4000,6000,8000,10000,12000,14000,16000,20000):
    i=np.argmin(abs(f-fo)); print("%7d Hz %6.1f dB"%(fo,pdb[i-2:i+3].mean()))
