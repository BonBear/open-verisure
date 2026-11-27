#!/usr/bin/env bash
# cppcheck over the firmware sources (CI job "cppcheck"; also runs locally with cppcheck on PATH).
# No config files are needed: both CC1110 main.c fall back to config.h.example and the gateway
# builds without secrets.h. Never copy the *.example files over the real config files on a dev
# machine. Runs all six groups and exits non-zero if any of them reported a finding.
set -uo pipefail
cd "$(dirname "$0")/../.."

common=(--enable=warning,portability --check-level=exhaustive --inline-suppr --error-exitcode=1 --quiet)
rc=0

# CC1110 (SDCC 8051): storage classes and interrupt attributes have no meaning for the analysis;
# avr8 is the closest platform to SDCC mcs51 (16-bit int)
sdcc_defs=(-D__xdata= -D__data= -D__idata= -D__pdata= -D__code= "-D__interrupt(x)=" "-D__using(x)="
           -DCTR_SEED=1UL -DSDCC)
echo "== CC1110 firmware"
cppcheck "${common[@]}" --platform=avr8 --language=c --std=c99 "${sdcc_defs[@]}" -I cc1110_common \
    VerisureDoorSensor/main.c MD_CAM/firmware/main.c cc1110_common/*.c || rc=1

echo "== Blackfin snap_link"
cppcheck "${common[@]}" --platform=unix32 --language=c --std=c99 -I MD_CAM/blackfin \
    MD_CAM/blackfin/*.c || rc=1

# the host-test stub Arduino.h defines PROGMEM and String for the analysis
echo "== Gateway (ESP32-S2)"
cppcheck "${common[@]}" --platform=unix32 --language=c++ --std=c++17 -I 868Receiver/src \
    -I 868Receiver/test_host/stubs 868Receiver/src/*.cpp || rc=1

echo "== Siren (MSP430F149)"
cppcheck "${common[@]}" --platform=avr8 --language=c --std=c99 \
    -DSIREN_USE_EXAMPLE_CONFIG -D__MSP430F149__ -DCTR_SEED=1UL \
    -I SIREN/firmware/include -I SIREN/firmware/src SIREN/firmware/src/*.c || rc=1

# the Gecko SDK headers are not in the repository; cppcheck runs on the project sources alone
echo "== Smoke detector (EFR32FG14)"
cppcheck "${common[@]}" --platform=unix32 --language=c --std=c99 -D__code= -DEFR32FG14P231F256GM48 \
    -I SMOKE_DETECTOR/firmware/src SMOKE_DETECTOR/firmware/src/*.c || rc=1

echo "== BSL flasher (ESP32-C3)"
cppcheck "${common[@]}" --platform=unix32 --language=c++ --std=c++17 \
    -I 868Receiver/test_host/stubs tools/esp32_msp430_bsl/src/main.cpp || rc=1

exit "$rc"
