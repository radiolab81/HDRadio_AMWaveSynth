#!/bin/bash
apt update
apt install cmake
apt install libusb-1.*
cd osmo-fl2k
mkdir build
cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make -j 3
sudo make install
sudo ldconfig
