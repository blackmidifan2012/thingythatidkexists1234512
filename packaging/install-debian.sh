#!/bin/sh
# Installs the prebuilt metrowl package on Debian 13 "trixie" (amd64) or newer.
# Nothing is compiled: only the runtime libraries metrowl links against are installed,
# then the prebuilt files are unpacked to /usr. No extra apps are installed.
#
#   sudo sh packaging/install-debian.sh            install
#   sudo sh packaging/install-debian.sh --remove   uninstall
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
PKG="$HERE/metrowl-0.1.0-debian-amd64.tar.gz"
FILES="usr/bin/metrowl usr/bin/metrowlctl usr/bin/metrowl-session usr/share/metrowl usr/share/wayland-sessions/metrowl.desktop"

if [ "$(id -u)" != 0 ]; then echo "Run as root: sudo sh $0" >&2; exit 1; fi

if [ "$1" = "--remove" ]; then
	for f in $FILES; do rm -rf "/$f"; done
	echo "metrowl removed (runtime libraries were left installed)."
	exit 0
fi

arch=$(dpkg --print-architecture)
if [ "$arch" != amd64 ]; then
	echo "The prebuilt package is for amd64 only (this system is $arch)." >&2
	echo "On ARM phones and tablets build from source instead (see README.md)." >&2
	exit 1
fi
if [ ! -f "$PKG" ]; then echo "Missing $PKG" >&2; exit 1; fi

# Runtime libraries only (Debian 13 trixie package names).
# libwlroots-0.18 pulls in libseat, libdrm, libinput and wayland; dbus is used for
# notifications by metrowl-session.
apt-get update
apt-get install -y --no-install-recommends \
	libwlroots-0.18 libwayland-server0 libxkbcommon0 libinput10 libpixman-1-0 \
	libcairo2 libpango-1.0-0 libpangocairo-1.0-0 libfontconfig1 libglib2.0-0t64 \
	librsvg2-2 libsystemd0 dbus

tar -xzf "$PKG" -C /
chmod 755 /usr/bin/metrowl /usr/bin/metrowlctl /usr/bin/metrowl-session
fc-cache -f /usr/share/metrowl/fonts >/dev/null 2>&1 || true

echo
echo "metrowl installed."
echo "  Log out and pick \"Metro (metrowl)\" in your display manager, or run"
echo "  metrowl-session from a text console (TTY)."
