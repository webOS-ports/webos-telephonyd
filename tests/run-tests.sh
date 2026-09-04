#!/bin/sh
# Build and run the unit tests standalone. The tested units only need GLib,
# so this works on any development machine without the webOS stack:
#
#   ./tests/run-tests.sh
#
# The same tests are wired into CMake/CTest behind -DWITH_TESTS=ON for
# builds that go through the full webOS toolchain.

set -e

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
TOP_DIR="$(dirname "$TESTS_DIR")"
BUILD_DIR="${TESTS_DIR}/.build"

CFLAGS="$(pkg-config --cflags glib-2.0) -I${TOP_DIR}/src -Wall -Werror"
LIBS="$(pkg-config --libs glib-2.0)"

mkdir -p "$BUILD_DIR"

status=0

run_test() {
	name="$1"
	shift
	${CC:-cc} $CFLAGS -o "$BUILD_DIR/$name" "$@" $LIBS
	if "$BUILD_DIR/$name" --tap; then
		echo "PASS: $name"
	else
		echo "FAIL: $name"
		status=1
	fi
}

run_test test_timeutils "$TESTS_DIR/test_timeutils.c" "$TOP_DIR/src/timeutils.c"
run_test test_netutils "$TESTS_DIR/test_netutils.c" "$TOP_DIR/src/netutils.c"
run_test test_ratmode "$TESTS_DIR/test_ratmode.c" "$TOP_DIR/src/telephonydriver.c"

exit $status
