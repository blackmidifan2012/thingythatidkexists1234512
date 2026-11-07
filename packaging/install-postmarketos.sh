#!/bin/sh
# Builds and installs metrowl on postmarketOS (Alpine based, any architecture:
# aarch64, armv7, x86_64), with either OpenRC or systemd. Installs only the build tools and libraries metrowl needs;
# the build tools can be removed again with --clean-tools after installing.
#
#   sudo sh packaging/install-postmarketos.sh               build + install
#   sudo sh packaging/install-postmarketos.sh --clean-tools remove build tools afterwards
#   sudo sh packaging/install-postmarketos.sh --remove      uninstall metrowl
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$SRC/build-pmos"

if [ "$(id -u)" != 0 ]; then echo "Run as root: sudo sh $0" >&2; exit 1; fi

if [ "$1" = "--remove" ]; then
	rm -rf /usr/bin/metrowl /usr/bin/metrowlctl /usr/bin/metrowl-session \
		/usr/share/metrowl /usr/share/wayland-sessions/metrowl.desktop
	echo "metrowl removed."
	exit 0
fi
if [ "$1" = "--clean-tools" ]; then
	apk del .metrowl-build
	exit 0
fi

# wlroots 0.18 or 0.19: newer postmarketOS/Alpine ships versioned packages
WLR=wlroots-dev
for p in wlroots0.19-dev wlroots0.18-dev; do
	if apk search -q -e "$p" | grep -q .; then WLR=$p; break; fi
done
# postmarketOS can boot with OpenRC (default) or systemd. Notifications need an sd-bus
# library: elogind on OpenRC, libsystemd on systemd; seat access comes from seatd on
# OpenRC and from systemd-logind on systemd.
if [ -d /run/systemd/system ]; then INIT=systemd; else INIT=openrc; fi
if [ "$INIT" = systemd ]; then SDBUS=systemd-dev; RUNTIME="dbus"; else SDBUS=elogind-dev; RUNTIME="seatd dbus"; fi
echo "Init system: $INIT"

# build tools in a virtual group so they can be removed in one go
apk add --virtual .metrowl-build build-base meson samurai pkgconf wayland-dev \
	wayland-protocols "$WLR" libxkbcommon-dev libinput-dev libdrm-dev pixman-dev \
	cairo-dev pango-dev fontconfig-dev glib-dev librsvg-dev "$SDBUS"
# runtime pieces that are not pulled in by the libraries above
apk add $RUNTIME

meson setup "$BUILD" "$SRC" --buildtype=release --prefix=/usr --wipe >/dev/null 2>&1 ||
	meson setup "$BUILD" "$SRC" --buildtype=release --prefix=/usr
ninja -C "$BUILD"
meson install -C "$BUILD"

# the compositor needs seat access
USER_NAME=${SUDO_USER:-$(logname 2>/dev/null || true)}
if [ "$INIT" = systemd ]; then
	# systemd-logind hands out the seat to the logged-in user; just make sure it runs
	systemctl enable --now systemd-logind >/dev/null 2>&1 || true
	if [ -n "$USER_NAME" ] && [ "$USER_NAME" != root ]; then
		for g in video input; do addgroup "$USER_NAME" "$g" 2>/dev/null || true; done
	fi
else
	rc-update add seatd default >/dev/null 2>&1 || true
	rc-service seatd start >/dev/null 2>&1 || true
	if [ -n "$USER_NAME" ] && [ "$USER_NAME" != root ]; then addgroup "$USER_NAME" seat 2>/dev/null || true; fi
fi

echo
echo "metrowl installed. Log in on a text console (TTY) and run: metrowl-session"
echo "(or pick \"Metro (metrowl)\" in a display manager such as tinydm/greetd)."
echo "Remove the build tools with: sudo sh $0 --clean-tools"
