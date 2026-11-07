# metrowl

A Windows Phone 8 / 8.1 Metro UI shell for Linux, written as a wlroots Wayland compositor
(C, cairo, pango). Start screen with live tiles, app list, lock screen, action center,
on-screen keyboard, task switcher, and built-in Metro apps (Settings, Alarms & Clock,
Calendar, Calculator, Weather, People, Music, Photos, Files, Battery Saver, ...).
English and Turkish.

## Build

Needs wlroots 0.18 or 0.19, wayland, cairo, pango, xkbcommon, libinput, glib
(optional: librsvg for SVG icons, libsystemd/elogind/basu for notifications).

    meson setup build
    ninja -C build
    meson test -C build

## Install

- **Debian 13 (trixie) or newer, amd64**: `sudo sh packaging/install-debian.sh` installs the
  prebuilt package (no compiling) plus only the runtime libraries it needs.
- **postmarketOS** (any architecture): `sudo sh packaging/install-postmarketos.sh` installs
  the build dependencies, builds and installs metrowl, and sets up seatd. Afterwards
  `--clean-tools` removes the build tools again.

Both scripts accept `--remove` to uninstall.

## Run

    ./build/metrowl                       # from a TTY (DRM) or nested in another Wayland session
    ./build/metrowl --no-lock --demo      # try it on a desktop
    WLR_BACKENDS=headless ./build/metrowl --size 480x800   # no display, control via metrowlctl

`metrowlctl help` lists the control commands (screenshot, tap, swipe, open apps, ...).

## Phones and tablets

The UI scale is picked from the panel's physical size (Windows Phone proportions: the
480 dp page is ~62 mm wide), so phones keep the classic layout and tablets get more tile
columns. Checked from 480x800 up to 2560x1600, portrait and landscape. Override with
`--scale S` or Settings > display.

Example: **Samsung Galaxy Grand Prime** (SM-G530, 540x960, postmarketOS
`samsung-grandprimelte`, aarch64/armv7) runs at 1.125x, i.e. a 480x853 dp page. Start it
from a TTY with `metrowl` (DRM + libinput backends; not yet tested on the real device).

## Linux apps

Any Wayland app runs full screen between the status and navigation bars, at the size apps
are made for on phones: a page 360 px wide, as on GNOME (Phosh) and Plasma Mobile phones,
instead of the denser 480 dp Metro page. That way adaptive GNOME (libadwaita) and KDE
(Kirigami) apps pick their phone layouts and finger sized controls. Settings > display >
"Phone-size Linux apps" turns this off. Apps without a phone layout stay as wide as on the
desktop and are cut off at the right edge.

Tested with GNOME Calculator, Clocks and Text Editor 46, and Firefox 159 (opens from the
Internet tile as the default browser, shows in the task switcher). Desktop Firefox is wider
than a phone page: use a mobile Firefox config such as postmarketOS's, or lower
`layout.css.devPixelsPerPx` in `about:config` (to about `0.6`). Third-party apps keep their
own coloured icons; Settings > start+theme > "Monochrome app icons" draws them as white
silhouettes instead.

## Fonts and icons (for the real Windows Phone look)

The bundled Selawik font is an open replacement for Segoe UI. Icons are drawn as vectors
unless the **Segoe MDL2 Assets** icon font is installed, in which case the original Windows
glyphs are used automatically. These fonts are Microsoft's and cannot be shipped here; if
you have a licensed copy (e.g. `segmdl2.ttf`, `segoeui*.ttf` from a Windows install),
copy them into `~/.local/share/fonts/` (or `data/fonts/`) and restart metrowl.
Settings > start+theme > icon style chooses automatic / font / vector.

## Data

Settings: `~/.config/metrowl/config.json`. App data (alarms, events, contacts, notes):
`~/.local/share/metrowl/`. Screenshots: `~/Pictures/Screenshots`.
