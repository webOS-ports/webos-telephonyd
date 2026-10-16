#!/bin/sh
# Static analysis of the given C files (default: the SIM toolkit units and
# the files they touch), one section per tool:
#
#   gcc -fanalyzer (-O0 and -O2), clang --analyze with the alpha
#   unix/security/core checkers, clang-tidy, cppcheck (all checks,
#   exhaustive, inconclusive), flawfinder, sparse, and the Coccinelle rules
#   in tests/cocci.
#
#   SYSROOT=<x86-64 recipe sysroot> tests/static-analysis.sh [file.c ...]
#
# Findings are printed per tool and file; only findings in the analysed
# files count (glib's and pbnjson's headers are not ours). The exit status
# is the number of tools with findings, so a clean run exits 0. A tool that
# is not installed is reported as skipped, not as clean.
set -u
cd "$(dirname "$0")/.."
TOP=$(pwd)
SYSROOT=${SYSROOT:?set SYSROOT to an x86-64 recipe sysroot}
GEN=${GEN:-$TOP/tests/.build}

if [ $# -gt 0 ]; then
	FILES="$*"
else
	FILES="src/stkcore.c src/stkjson.c src/telephonyservice_stk.c drivers/ofono/ofonostk.c"
fi

mkdir -p "$GEN"
(cd "$GEN" && gdbus-codegen --c-namespace OfonoInterface --generate-c-code ofono-interface \
	--interface-prefix org.ofono. "$TOP/files/xml/ofono.xml")

PC="env PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig pkg-config"
INC="$($PC --cflags glib-2.0 gio-2.0 gio-unix-2.0 luna-service2 pbnjson_c luna-prefs) -Isrc -Idrivers/ofono -I$GEN"
CFLAGS="--sysroot=$SYSROOT $INC"
FILE_RE=$(printf '%s\n' $FILES | sed 's/[.]/[.]/g' | paste -sd'|')
tools_failed=0
skipped=""

report() {   # name output
	name=$1; out=$2
	findings=$(printf '%s\n' "$out" | grep -E "($FILE_RE):[0-9]+(:[0-9]+)?:? *(warning|error|style|performance|portability|\[[1-5]\])" || true)
	if [ -n "$findings" ]; then
		echo "== $name: $(printf '%s\n' "$findings" | wc -l) finding(s)"
		printf '%s\n' "$findings"
		tools_failed=$((tools_failed + 1))
	else
		echo "== $name: clean"
	fi
}

have() {
	if command -v "$1" >/dev/null 2>&1; then
		return 0
	fi
	skipped="$skipped $1"
	echo "== $1: not installed, skipped"
	return 1
}

if have gcc; then
	for opt in -O0 -O2; do
		out=""
		for f in $FILES; do
			# shellcheck disable=SC2086
			out="$out$(gcc -c -o /dev/null $opt -fanalyzer -Wanalyzer-too-complex \
				--param=analyzer-bb-explosion-factor=50 --param=analyzer-max-enodes-per-program-point=64 \
				-Wall -Wextra -Wno-unused-parameter $CFLAGS "$f" 2>&1)
"
		done
		report "gcc -fanalyzer $opt" "$out"
	done
fi

if have clang; then
	out=""
	for f in $FILES; do
		# shellcheck disable=SC2086
		out="$out$(clang --analyze -Xanalyzer -analyzer-output=text \
			-Xanalyzer -analyzer-checker=core,unix,deadcode,security,nullability,optin.portability,optin.core \
			-Xanalyzer -analyzer-checker=alpha.unix,alpha.security,alpha.core \
			-Xanalyzer -analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
			-o /dev/null $CFLAGS "$f" 2>&1)
"
	done
	report "clang --analyze (alpha unix/security/core)" "$out"
fi

if have clang-tidy; then
	# misc-unused-parameters: callback signatures are glib's, LS2's and
	# gdbus-codegen's to set. bugprone-casting-through-void: that is how the
	# GObject cast macros (G_DBUS_PROXY() and friends) are written.
	# shellcheck disable=SC2086
	out=$(clang-tidy --quiet $FILES \
		--checks='clang-analyzer-*,bugprone-*,cert-*,concurrency-*,misc-*,performance-*,portability-*,readability-misleading-indentation,readability-suspicious-call-argument,-bugprone-easily-swappable-parameters,-misc-include-cleaner,-misc-unused-parameters,-cert-err33-c,-bugprone-assignment-in-if-condition,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp,-bugprone-casting-through-void' \
		-- -Wno-unused-parameter $CFLAGS 2>&1)
	report clang-tidy "$out"
fi

if have cppcheck; then
	# shellcheck disable=SC2086
	out=$(cppcheck --enable=all --check-level=exhaustive --inconclusive --std=c11 \
		--library=gnu --library=posix --force --inline-suppr --quiet \
		--suppress=missingIncludeSystem --suppress=missingInclude --suppress=unmatchedSuppression \
		--suppress=checkersReport --suppress=unusedFunction --suppress=staticFunction \
		--template='{file}:{line}: {severity}: {message} [{id}]' \
		-D__linux__ -D__GNUC__ -Isrc -Idrivers/ofono -I"$GEN" $FILES 2>&1)
	report "cppcheck --enable=all --check-level=exhaustive --inconclusive" "$out"
fi

if have flawfinder; then
	# shellcheck disable=SC2086
	out=$(flawfinder --minlevel=1 --falsepositive --columns --dataonly --quiet $FILES 2>&1)
	report flawfinder "$out"
fi

if have sparse; then
	# sparse has no --sysroot: give it the sysroot's include directory
	out=""
	for f in $FILES; do
		# shellcheck disable=SC2086
		out="$out$(sparse -Wsparse-all -Wno-declaration-after-statement -Wno-non-pointer-null \
			-Wno-decl -Wno-shadow -isystem "$SYSROOT/usr/include" $INC "$f" 2>&1)
"
	done
	report sparse "$out"
fi

if have spatch; then
	out=""
	for rule in tests/cocci/*.cocci; do
		for f in $FILES; do
			res=$(spatch --very-quiet --sp-file "$rule" "$f" 2>/dev/null)
			[ -n "$res" ] && out="$out$f:0: warning: $(basename "$rule") matches
$res
"
		done
	done
	report "coccinelle (tests/cocci)" "$out"
fi

[ -n "$skipped" ] && echo "skipped:$skipped"
exit $tools_failed
