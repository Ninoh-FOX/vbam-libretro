# PGO profile for the Miyoo Mini build

`profile/` is the GCC profile (`.gcda`) recorded on a Miyoo Mini Plus at 1200 MHz:
three games (Mario Kart: Super Circuit, The Minish Cap, Final Fight One) and a
two-console Cable Link session, so the hidden GBAs (`obj/miyoomini/b`, copied as
B, C and D) have their own profile.

`make platform=miyoomini` (in src/libretro) uses it automatically
(`-fprofile-use`); `PGO=0` builds without it. It only fits the current sources:
after changing a source file, record it again (GCC ignores the profile of the
functions that changed and prints a `-Wcoverage-mismatch` warning).

Same output as without it (video and audio CRCs identical, Cable Link test with
4 consoles passes).

## Recording it again

1. `make platform=miyoomini clean`, then `make platform=miyoomini PGO=generate`
2. `arm-linux-gcc -O2 -I libretro-common/include -o lrbench pgo/miyoomini/lrbench.c -ldl`
   and `make -C ../../tools/dualtest arm` (Cable Link harness)
3. Copy `vbam_libretro.so`, `lrbench`, `train.sh` and `dualtest-arm` to one folder on
   the device (e.g. /tmp/pgo_tools) and run
   `train.sh /tmp/pgo_tools/vbam_libretro.so VBA-M 1800 /tmp/pgo_tools/dualtest-arm`
4. Unpack `/tmp/pgo.tar` into `pgo/miyoomini/profile/` (replacing it); `make clean` and rebuild.

`lrbench core.so rom frames [core.opt]` is also a benchmark (fps and CRCs of the
video and audio). `LR_KEYS=1` presses START/A on a schedule, `LR_SYSDIR=dir` is the BIOS folder.
