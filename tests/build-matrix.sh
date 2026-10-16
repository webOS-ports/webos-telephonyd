#!/bin/sh
# Builds the daemon six ways - GCC and Clang for armv7, aarch64 and x86-64 -
# against a LuneOS build's sysroots, with extra warnings, and lists the
# warnings per build.
#
#   tests/build-matrix.sh <build TMPDIR> <out dir>
#
# x86-64 and aarch64 use the webos-telephonyd recipe sysroots and their
# cross GCC. No armv7 recipe sysroot of telephonyd exists, so one is put
# together from tmp/sysroots-components/cortexa8t2hf-neon-halium, with the
# arm cross GCC from sysroots-components/x86_64. Clang is the build host's,
# pointed at the same sysroots.
#
# Each build leaves <out>/<name>.log and <out>/<name>.warnings (file and
# message, without line numbers, so that two trees can be compared with
# diff). The exit status is non-zero when a build fails.
set -e
cd "$(dirname "$0")/.."
TMPD=${1:?usage: build-matrix.sh <build TMPDIR> <out dir>}
OUT=${2:?usage: build-matrix.sh <build TMPDIR> <out dir>}
SRC=$(pwd)
WARN="-Wall -Wextra -Wshadow -Wformat=2 -Wcast-align -Wpointer-arith -Wvla -Wno-unused-parameter"
PKGS="glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0 luna-service2 pbnjson_c luna-prefs"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
FAILED=$OUT/failed
rm -f "$FAILED"

recipe_dir() {   # arch dir under work/
    ls -d "$TMPD"/work/"$1"/webos-telephonyd/*/ | head -n 1 | sed 's#/$##'
}

# armv7 sysroot out of the sysroot components
ARMSR=$OUT/sysroot-armv7
if [ ! -d "$ARMSR/usr/include/luna-service2" ]; then
    SC=$TMPD/sysroots-components/cortexa8t2hf-neon-halium
    mkdir -p "$ARMSR"
    for c in glibc gcc-runtime libgcc glib-2.0 libffi libpcre2 zlib util-linux luna-service2 \
             libpbnjson luna-prefs json-c pmloglib linux-libc-headers yajl gmp uriparser \
             libpthread-stubs libxcrypt libcap systemd nyx-lib sqlite3; do
        [ -d "$SC/$c" ] && cp -a "$SC/$c/." "$ARMSR/"
    done
    rm -rf "$ARMSR/sysroot-providers"
fi

GEN=$OUT/gen
mkdir -p "$GEN"
(cd "$GEN" && gdbus-codegen --c-namespace OfonoInterface --generate-c-code ofono-interface \
    --interface-prefix org.ofono. "$SRC/files/xml/ofono.xml")

SOURCES="$(ls src/*.c drivers/ofono/*.c) $GEN/ofono-interface.c"

build() {   # name cc sysroot extra-cflags extra-ldflags
    name=$1; cc=$2; sr=$3; cflags=$4; ldflags=$5
    dir=$OUT/$name
    rm -rf "$dir"; mkdir -p "$dir"
    pc="env PKG_CONFIG_SYSROOT_DIR=$sr PKG_CONFIG_LIBDIR=$sr/usr/lib/pkgconfig:$sr/usr/share/pkgconfig pkg-config"
    pkgcflags=$($pc --cflags $PKGS)
    pkglibs=$($pc --libs $PKGS)
    : > "$dir.log"
    objs=""
    for f in $SOURCES; do
        o=$dir/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
        # shellcheck disable=SC2086
        if ! $cc --sysroot="$sr" -O2 -g $WARN $cflags $pkgcflags -Isrc -Idrivers/ofono -I"$GEN" \
                -c "$f" -o "$o" >> "$dir.log" 2>&1; then
            echo "$name: compile of $f FAILED (see $dir.log)"; touch "$FAILED"; return
        fi
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    if ! $cc --sysroot="$sr" $cflags $ldflags -o "$dir/webos-telephonyd" $objs $pkglibs -lrt -lpthread \
            -Wl,--no-undefined -Wl,-rpath-link,"$sr/usr/lib" -Wl,-rpath-link,"$sr/lib" >> "$dir.log" 2>&1; then
        echo "$name: link FAILED (see $dir.log)"; touch "$FAILED"; return
    fi
    grep -E "warning:" "$dir.log" | sed -E "s#^$SRC/##; s#^$GEN/#gen/#; s#:[0-9]+:[0-9]+:#:#" | sort -u > "$dir.warnings" || true
    own=$(grep -v -c "^gen/" "$dir.warnings" || true)
    gen=$(grep -c "^gen/" "$dir.warnings" || true)
    echo "$name: built ($(file -b "$dir/webos-telephonyd" | cut -d, -f1-2)), $own warnings in the sources, $gen in generated code"
}

gcclibdir() {   # sysroot
    dirname "$(find "$1/usr/lib" -name crtbeginS.o | head -n 1)"
}

# x86-64 and aarch64: recipe sysroots
for arch in x86_64 aarch64; do
    if [ $arch = x86_64 ]; then D=$(recipe_dir corei7-64-webos-linux); triple=x86_64-webos-linux
    else D=$(recipe_dir aarch64-webos-linux); triple=aarch64-webos-linux; fi
    SR=$D/recipe-sysroot
    NATIVE=$D/recipe-sysroot-native/usr/bin/$triple
    build "$arch-gcc" "$NATIVE/$triple-gcc" "$SR" "" ""
    gl=$(gcclibdir "$SR")
    build "$arch-clang" "clang --target=$triple" "$SR" "-B$gl" "-L$gl --ld-path=$NATIVE/$triple-ld"
done

# armv7: assembled sysroot, cross GCC from the components
X86C=$TMPD/sysroots-components/x86_64
triple=arm-webos-linux-gnueabi
ARMBIN=$X86C/gcc-cross-arm/usr/bin/$triple
PATH=$X86C/binutils-cross-arm/usr/bin/$triple:$ARMBIN:$PATH
export PATH
cpu="-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=hard -mtune=cortex-a8"
# The cross GCC looks for a plain "as"/"ld", which on the build host are
# the x86 ones; point it at the arm binutils.
ARMTOOLS=$OUT/armv7-tools
mkdir -p "$ARMTOOLS"
for t in as ld; do
    ln -sf "$X86C/binutils-cross-arm/usr/bin/$triple/$triple-$t" "$ARMTOOLS/$t"
done
build armv7-gcc "$ARMBIN/$triple-gcc" "$ARMSR" "$cpu -B$ARMTOOLS" ""
gl=$(gcclibdir "$ARMSR")
build armv7-clang "clang --target=$triple" "$ARMSR" "$cpu -B$gl" "-L$gl --ld-path=$X86C/binutils-cross-arm/usr/bin/$triple/$triple-ld"

[ ! -e "$FAILED" ]
