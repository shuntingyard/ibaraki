#!/bin/sh

TCHAIN="arm-linux-gnueabi-"
TUXDIR="../../raspi/buildroot/output/build/linux-rpi-4.1.y/"

make ARCH=arm CROSS_COMPILE=${TCHAIN} KDIR=${TUXDIR} $@
