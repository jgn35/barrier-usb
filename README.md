# Barrier USB

USB firmware for lilygo to control isolated computer with software KVM

This firmaware aims to be used with a lilygo T-Dongle S3 device. It handles the wireless connection used by the modified version of input-leap to control the keyboard and mouse of the attached computer.

## How to compile the firmware

Install ESP-IDF framework
source the export.Sh file of the ESP-IDF framework
In the repository run `idf.py build`

Upload it with the command `idf.py flash -p /dev/ttyACM0`

## How to compile the test software (linux only)
Go in the host directory.
copy wifi.conf.template as wifi.conf
modify the configuration to set the used wifi network.
run as sudo test or compile it with the command 
gcc -o test main.c -lusb-1.0 `pkg-config gnutls --cflags --libs` -lpthread -lconfig
and run test as root (or change permission on the USB device)

TODO : add the installation of requiered library
TODO : create documentation of the firmware
