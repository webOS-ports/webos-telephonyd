#!/bin/sh
# Runs telephonyd's SIM toolkit agent (tests/phonesim/stk-harness) against a
# real ofono and phonesim on the build host, without root and without
# touching the host's ofono or D-Bus:
#
#   tests/phonesim/run-phonesim.sh <work dir> [stk-harness arguments]
#
#   e.g. tests/phonesim/run-phonesim.sh ~/scratch/phonesim --select=1 --pick=0,3
#
# The first run fetches the distribution's ofono and ofono-phonesim packages
# with "apt-get download" (Debian/Ubuntu; nothing is installed) and unpacks
# them into <work dir>. Each run then, inside "unshare -rm" (a user and mount
# namespace):
#   - overlays /etc and /var/lib so that ofono finds /etc/ofono/phonesim.conf
#     and can write /var/lib/ofono,
#   - starts a private dbus-daemon used as both system and session bus,
#   - starts phonesim (-gui, offscreen) and ofonod on it,
#   - powers the phonesim modem up and waits for org.ofono.SimToolkit,
#   - starts phonesim's "Demo SIM Application" through its script interface,
#   - runs stk-harness with the given arguments.
# Logs: <work dir>/ofonod.log, phonesim.log, dbus.log. STK_HARNESS_RUNNER is
# put in front of the harness (e.g. "valgrind --leak-check=full").
#
# Needs: apt-get, dpkg-deb, unshare with unprivileged user namespaces and
# overlayfs, dbus-daemon, dbus-send, the Qt 5 libraries phonesim links
# against, and for building the harness GLib/GIO plus pbnjson_c (pkg-config;
# PKG_CONFIG_PATH may point at a pbnjson taken from a LuneOS sysroot).
set -e

if [ "${STK_PHONESIM_INNER:-}" = 1 ]; then
	W=$1; shift
	mount -t overlay overlay -o lowerdir=/etc,upperdir="$W/ov/etc-upper",workdir="$W/ov/etc-work" /etc
	mount -t overlay overlay -o lowerdir=/var/lib,upperdir="$W/ov/var-upper",workdir="$W/ov/var-work" /var/lib
	rm -f "$W/bus.sock"
	dbus-daemon --config-file="$W/bus.conf" --nofork --nopidfile > "$W/dbus.log" 2>&1 &
	DBUS=$!
	export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$W/bus.sock"
	export DBUS_SESSION_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS"
	i=0; while [ ! -S "$W/bus.sock" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
	QT_QPA_PLATFORM=offscreen "$W/root/usr/bin/ofono-phonesim" -gui -p 12345 \
		"$W/root/usr/share/ofono-phonesim/default.xml" > "$W/phonesim.log" 2>&1 &
	PSIM=$!
	sleep 1
	OFONO_AT_DEBUG=1 "$W/root/usr/sbin/ofonod" -n -d '*' > "$W/ofonod.log" 2>&1 &
	OFONO=$!
	trap 'kill $OFONO $PSIM $DBUS 2>/dev/null; wait 2>/dev/null' EXIT
	sleep 2

	prop() { dbus-send --system --print-reply --dest=org.ofono /phonesim org.ofono.Modem.GetProperties 2>/dev/null; }
	for p in Powered Online; do
		i=0
		until prop | grep -A1 "\"$p\"" | grep -q true; do
			dbus-send --system --print-reply --dest=org.ofono /phonesim org.ofono.Modem.SetProperty \
				string:$p variant:boolean:true > /dev/null 2>&1 || true
			sleep 1; i=$((i+1)); [ $i -gt 20 ] && { echo "modem not $p"; exit 1; }
		done
	done
	i=0
	until prop | grep -q '"org.ofono.SimToolkit"'; do
		sleep 0.5; i=$((i+1)); [ $i -gt 40 ] && { echo "SimToolkit did not appear"; exit 1; }
	done
	echo "phonesim modem up with SimToolkit"

	( sleep 2
	  dbus-send --session --print-reply --dest=org.ofono.phonesim / \
		org.ofono.phonesim.Script.SetPath string:"$W/scripts" > /dev/null
	  dbus-send --session --print-reply --dest=org.ofono.phonesim / \
		org.ofono.phonesim.Script.Run string:start.js > /dev/null ) &

	# shellcheck disable=SC2086
	$STK_HARNESS_RUNNER "$W/stk-harness" /phonesim "$@"
	exit $?
fi

W=${1:?usage: run-phonesim.sh <work dir> [stk-harness arguments]}
shift
mkdir -p "$W"
W=$(cd "$W" && pwd)
TOP=$(cd "$(dirname "$0")/../.." && pwd)

if [ ! -x "$W/root/usr/sbin/ofonod" ] || [ ! -x "$W/root/usr/bin/ofono-phonesim" ]; then
	mkdir -p "$W/debs"
	(cd "$W/debs" && apt-get download ofono ofono-phonesim)
	mkdir -p "$W/root"
	for d in "$W"/debs/*.deb; do
		dpkg-deb -x "$d" "$W/root"
	done
fi

mkdir -p "$W/ov/etc-upper/ofono" "$W/ov/etc-work" "$W/ov/var-upper/ofono" "$W/ov/var-work" "$W/scripts"
printf '[phonesim]\nAddress=127.0.0.1\nPort=12345\n' > "$W/ov/etc-upper/ofono/phonesim.conf"
# phonesim's GUI is driven by QtScript: pick the demo application and start it
printf 'tabSIM.gbSATK.cbSimApps.currentIndex = 0;\ntabSIM.gbSATK.pbStart.click();\n' > "$W/scripts/start.js"
cat > "$W/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$W/bus.sock</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_type="method_call"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
EOF

# The harness: the daemon's agent code plus a scripted "user"
(cd "$W" && gdbus-codegen --c-namespace OfonoInterface --generate-c-code ofono-interface \
	--interface-prefix org.ofono. "$TOP/files/xml/ofono.xml")
# shellcheck disable=SC2046
${CC:-cc} -Wall -g $(pkg-config --cflags gio-2.0 pbnjson_c) -I"$TOP/src" -I"$TOP/drivers/ofono" \
	-I"$TOP/tests/stubs" -I"$W" -o "$W/stk-harness" "$TOP/tests/phonesim/stk-harness.c" \
	"$TOP/drivers/ofono/ofonostk.c" "$TOP/drivers/ofono/ofonobase.c" "$TOP/src/stkcore.c" \
	"$TOP/src/stkjson.c" "$W/ofono-interface.c" $(pkg-config --libs gio-2.0 pbnjson_c)

STK_PHONESIM_INNER=1 exec unshare -rm "$0" "$W" "$@"
