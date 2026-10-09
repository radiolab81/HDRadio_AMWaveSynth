#!/bin/sh
sudo apt update
sudo apt install autoconf automake libtool make gcc g++ unzip curl ffmpeg
# Baut (optional) das HDC-faehige fdk-aac und den Modulator.
# Ohne HDC-Codec:  ./build.sh nohdc   (Dummy-Audioframes, nur fuer Signaltests)
set -e
SRC=hdradio_am_modulator_5MSPS_integer.c
if [ "$1" = "nohdc" ]; then
  gcc -O3 -march=native $SRC -o hdradio_am -lpthread -lm
  exit 0
fi
FDK=$PWD/fdk-aac-hdc
if [ ! -f "$FDK/lib/libfdk-aac.a" ]; then
  # gepatchtes fdk-aac von argilo (HDC-Encoder), wie von gr-nrsc5 benutzt
  curl -L https://codeload.github.com/argilo/fdk-aac/zip/refs/heads/hdc-encoder -o fdk.zip
  unzip -q -o fdk.zip && cd fdk-aac-hdc-encoder
  autoreconf -fi && ./configure --prefix="$FDK" --disable-shared && make -j2 && make install
  cd ..
fi
gcc -O3 -march=native -DUSE_FDK_HDC -I"$FDK/include/fdk-aac" $SRC -o hdradio_am \
    "$FDK/lib/libfdk-aac.a" -lstdc++ -lpthread -lm
