#!/bin/bash
# Full host-native suite for Muse Bridge (C14 gate G1).
# Builds every native test from source with ASan/UBSan into a scratch
# dir and runs it. Exit 0 only if every suite passes.
set -u
cd "$(dirname "$0")"
ROOT=../..
OUT=${MB_NATIVE_OUT:-/tmp/mb_native}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I$ROOT/core -I$ROOT/modules -I$ROOT/transport"
export ASAN_OPTIONS=detect_leaks=0
FAIL=0

run() { # name, test_src, extra_srcs...
    local name=$1 src=$2; shift 2
    if ! $CC $CFLAGS "$src" "$@" -o "$OUT/$name" 2> "$OUT/$name.build.log"; then
        echo "BUILD-FAIL $name (see $OUT/$name.build.log)"; FAIL=1; return
    fi
    if "$OUT/$name" > "$OUT/$name.log" 2>&1; then
        echo "PASS $name: $(tail -1 "$OUT/$name.log")"
    else
        echo "FAIL $name:"; tail -5 "$OUT/$name.log"; FAIL=1
    fi

}

# Makefile-owned suites (reference core + C04 codec vectors)
make -s clean >/dev/null 2>&1
if make -s all > "$OUT/make.log" 2>&1 && ASAN_OPTIONS=detect_leaks=0 ./reference_test > "$OUT/reference.log" 2>&1 && ASAN_OPTIONS=detect_leaks=0 ./test_c04 > "$OUT/test_c04.log" 2>&1; then
    echo "PASS reference_test + test_c04 (make)"
else
    echo "FAIL reference_test/test_c04 (make)"; FAIL=1
fi

run test_c02 test_c02.c $ROOT/core/bridge_diag.c $ROOT/core/bridge_core.c
run test_c05 test_c05.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c $ROOT/modules/module_fake.c
run test_c06 test_c06.c $ROOT/core/bridge_core.c $ROOT/core/bridge_session.c $ROOT/transport/bridge_codec.c
run test_c07 test_c07.c $ROOT/core/bridge_core.c $ROOT/core/bridge_session.c $ROOT/core/bridge_executor.c $ROOT/transport/bridge_codec.c
run test_c08 test_c08.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c $ROOT/modules/module_fake.c
run test_c09 test_c09.c $ROOT/core/bridge_core.c $ROOT/core/bridge_session.c $ROOT/transport/bridge_codec.c
run test_c10 test_c10.c $ROOT/modules/module_gpio.c $ROOT/core/bridge_core.c $ROOT/core/bridge_executor.c
run test_c11 test_c11.c $ROOT/modules/module_adc.c $ROOT/modules/module_notify.c $ROOT/modules/module_gpio.c $ROOT/core/bridge_core.c $ROOT/core/bridge_executor.c
run test_c12 test_c12.c $ROOT/modules/module_ir.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c
run test_c13 test_c13.c $ROOT/modules/module_ir.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c
run test_c15 test_c15.c $ROOT/modules/module_object.c $ROOT/core/bridge_core.c
run test_c15r test_c15r.c $ROOT/modules/module_ir.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c
run test_c16 test_c16.c $ROOT/modules/module_nfc.c $ROOT/core/bridge_executor.c $ROOT/core/bridge_core.c

if [ $FAIL -eq 0 ]; then echo "NATIVE SUITE: ALL PASS"; else echo "NATIVE SUITE: FAILURES PRESENT"; fi
exit $FAIL
