#!/bin/sh
# Builds and installs metrowl on postmarketOS (Alpine based, any architecture:
# aarch64, armv7, x86_64). Installs only the build tools and libraries metrowl needs;
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
# notifications need an sd-bus implementation: elogind on OpenRC, systemd if used
SDBUS=elogind-dev
if apk info -e systemd >/dev/null 2>&1; then SDBUS=systemd-dev; fi

# build tools in a virtual group so they can be removed in one go
apk add --virtual .metrowl-build build-base meson samurai pkgconf wayland-dev \
	wayland-protocols "$WLR" libxkbcommon-dev libinput-dev libdrm-dev pixman-dev \
	cairo-dev pango-dev fontconfig-dev glib-dev librsvg-dev "$SDBUS"
# runtime pieces that are not pulled in by the libraries above
apk add seatd dbus

meson setup "$BUILD" "$SRC" --buildtype=release --prefix=/usr --wipe >/dev/null 2>&1 ||
	meson setup "$BUILD" "$SRC" --buildtype=release --prefix=/usr
ninja -C "$BUILD"
meson install -C "$BUILD"

# the compositor needs seat access: run seatd and add the user to its group
rc-update add seatd default >/dev/null 2>&1 || true
rc-service seatd start >/dev/null 2>&1 || true
USER_NAME=${SUDO_USER:-$(logname 2>/dev/null || true)}
if [ -n "$USER_NAME" ] && [ "$USER_NAME" != root ]; then addgroup "$USER_NAME" seat 2>/dev/null || true; fi

echo
echo "metrowl installed. Log in on a text console (TTY) and run: metrowl-session"
echo "(or pick \"Metro (metrowl)\" in a display manager such as tinydm/greetd)."
echo "Remove the build tools with: sudo sh $0 --clean-tools"
