#!/bin/sh
# Runs the Cable Link test harness.
#   run.sh [arm] ROM FRAMES [latency_ms] [solo_frames]
# Both use vbam_libretro.so at the root of the repository: without
# "arm", the core built for this machine (make -C src/libretro); with
# "arm", the Miyoo Mini core (make -C src/libretro platform=miyoomini)
# under qemu-arm.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
if [ "$1" = "arm" ]; then
  shift
  make -s -C "$HERE" arm
  exec qemu-arm -L /opt/miyoomini-toolchain/arm-miyoomini-linux-gnueabihf/sysroot \
    "$HERE/dualtest-arm" "$ROOT/src/libretro/vbam_libretro.so" "$@"
fi
make -s -C "$HERE" dualtest
exec "$HERE/dualtest" "$ROOT/src/libretro/vbam_libretro.so" "$@"
