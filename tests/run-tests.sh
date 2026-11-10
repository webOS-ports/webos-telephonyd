#!/bin/sh
# Build and run the unit tests standalone.
#
#   ./tests/run-tests.sh
#       with the build host's compiler and GLib. The units under test need
#       GLib/GIO only; the D-Bus test starts a private dbus-daemon of its
#       own (dbus-daemon has to be installed). The pbnjson / luna tests are
#       skipped.
#
#   SYSROOT=<x86-64 recipe sysroot> CC=<its x86_64-webos-linux-gcc> ./tests/run-tests.sh
#       against a LuneOS build's x86-64 sysroot, run through that sysroot's
#       dynamic loader; this includes the pbnjson and luna method tests.
#
# EXTRA_CFLAGS / EXTRA_LDFLAGS are added to every build (sanitizers), and
# RUNNER is put in front of every test (e.g. "valgrind --error-exitcode=1").
#
# The same tests are wired into CMake/CTest behind -DWITH_TESTS=ON for
# builds that go through the full webOS toolchain.

set -e

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
TOP_DIR="$(dirname "$TESTS_DIR")"
BUILD_DIR="${BUILD_DIR:-${TESTS_DIR}/.build}"

mkdir -p "$BUILD_DIR"

if [ -n "$SYSROOT" ]; then
	PC="env PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig pkg-config"
	SYSROOT_FLAGS="--sysroot=$SYSROOT"
	# --library-path rather than LD_LIBRARY_PATH: the D-Bus test spawns
	# the build host's dbus-daemon, which must not load the sysroot's libc.
	LOADER="$SYSROOT/usr/lib/ld-linux-x86-64.so.2 --library-path $SYSROOT/usr/lib"
	STUBS=""
else
	PC="pkg-config"
	SYSROOT_FLAGS=""
	LOADER=""
	STUBS="-I${TESTS_DIR}/stubs"
fi

CFLAGS="$SYSROOT_FLAGS $($PC --cflags gio-2.0) -I${TOP_DIR}/src -I${TOP_DIR}/drivers/ofono -I${BUILD_DIR} -Wall -Werror -g $EXTRA_CFLAGS"
LIBS="$($PC --libs gio-2.0) $EXTRA_LDFLAGS"

(cd "$BUILD_DIR" && gdbus-codegen --c-namespace OfonoInterface --generate-c-code ofono-interface \
	--interface-prefix org.ofono. "$TOP_DIR/files/xml/ofono.xml")

status=0

run_test() {
	name="$1"
	extra="$2"
	shift 2
	# shellcheck disable=SC2086
	${CC:-cc} $CFLAGS $extra -o "$BUILD_DIR/$name" "$@" $LIBS $EXTRA_LIBS
	# shellcheck disable=SC2086
	if $RUNNER $LOADER "$BUILD_DIR/$name" --tap; then
		echo "PASS: $name"
	else
		echo "FAIL: $name"
		status=1
	fi
}

run_test test_timeutils "" "$TESTS_DIR/test_timeutils.c" "$TOP_DIR/src/timeutils.c"
run_test test_netutils "" "$TESTS_DIR/test_netutils.c" "$TOP_DIR/src/netutils.c"
run_test test_ratmode "" "$TESTS_DIR/test_ratmode.c" "$TOP_DIR/src/telephonydriver.c"
run_test test_stkcore "" "$TESTS_DIR/test_stkcore.c" "$TOP_DIR/src/stkcore.c"
run_test test_ofonostk "$STUBS" "$TESTS_DIR/test_ofonostk.c" "$TOP_DIR/drivers/ofono/ofonostk.c" \
	"$TOP_DIR/drivers/ofono/ofonobase.c" "$TOP_DIR/src/stkcore.c" "$BUILD_DIR/ofono-interface.c"
run_test test_mmscore "" "$TESTS_DIR/test_mmscore.c" "$TOP_DIR/src/mmscore.c"
run_test test_mmsdclient "-I$TESTS_DIR" "$TESTS_DIR/test_mmsdclient.c" "$TESTS_DIR/fake_mmsd.c" \
	"$TOP_DIR/src/mmsdclient.c" "$TOP_DIR/src/mmscore.c"
run_test test_ofonodatasim "" "$TESTS_DIR/test_ofonodatasim.c" "$TOP_DIR/drivers/ofono/ofonodatasim.c"

if $PC --exists pbnjson_c 2>/dev/null; then
	EXTRA_LIBS="$($PC --libs pbnjson_c)" \
	run_test test_stkjson "$($PC --cflags pbnjson_c)" "$TESTS_DIR/test_stkjson.c" "$TOP_DIR/src/stkjson.c" \
		"$TOP_DIR/src/stkcore.c"
else
	echo "SKIP: test_stkjson (no pbnjson_c)"
fi

if $PC --exists pbnjson_c 2>/dev/null; then
	EXTRA_LIBS="$($PC --libs pbnjson_c)" \
	run_test test_mmsjson "$($PC --cflags pbnjson_c)" "$TESTS_DIR/test_mmsjson.c" "$TOP_DIR/src/mmsjson.c" \
		"$TOP_DIR/src/mmscore.c"
else
	echo "SKIP: test_mmsjson (no pbnjson_c)"
fi

# The luna methods, with luna-service2 replaced by stubs in the test: only
# its headers are needed.
if $PC --exists pbnjson_c luna-service2 2>/dev/null; then
	EXTRA_LIBS="$($PC --libs pbnjson_c)" \
	run_test test_telephonyservice_stk "$($PC --cflags pbnjson_c luna-service2)" \
		"$TESTS_DIR/test_telephonyservice_stk.c" "$TOP_DIR/src/telephonyservice_stk.c" \
		"$TOP_DIR/src/stkjson.c" "$TOP_DIR/src/stkcore.c" "$TOP_DIR/src/luna_service_utils.c"
else
	echo "SKIP: test_telephonyservice_stk (no pbnjson_c / luna-service2 headers)"
fi

# Pictures made smaller for an MMS: gdk-pixbuf with its JPEG and PNG loaders.
if $PC --exists gdk-pixbuf-2.0 2>/dev/null; then
	EXTRA_LIBS="$($PC --libs gdk-pixbuf-2.0)" \
	run_test test_mmsimage "$($PC --cflags gdk-pixbuf-2.0)" "$TESTS_DIR/test_mmsimage.c" \
		"$TOP_DIR/src/mmsimage.c" "$TOP_DIR/src/mmscore.c"
else
	echo "SKIP: test_mmsimage (no gdk-pixbuf-2.0)"
fi

# MMS from the db8 outbox to a fake mmsd on a private bus and back.
if $PC --exists pbnjson_c luna-service2 gdk-pixbuf-2.0 2>/dev/null; then
	EXTRA_LIBS="$($PC --libs pbnjson_c gdk-pixbuf-2.0)" \
	run_test test_telephonyservice_mms "$($PC --cflags pbnjson_c luna-service2 gdk-pixbuf-2.0) -I$TESTS_DIR" \
		"$TESTS_DIR/test_telephonyservice_mms.c" "$TESTS_DIR/fake_mmsd.c" "$TOP_DIR/src/telephonyservice_mms.c" \
		"$TOP_DIR/src/mmsjson.c" "$TOP_DIR/src/mmscore.c" "$TOP_DIR/src/mmsdclient.c" "$TOP_DIR/src/mmsimage.c" \
		"$TOP_DIR/src/luna_service_utils.c" "$TOP_DIR/src/timeutils.c"
else
	echo "SKIP: test_telephonyservice_mms (no pbnjson_c / luna-service2 headers / gdk-pixbuf-2.0)"
fi

exit $status
