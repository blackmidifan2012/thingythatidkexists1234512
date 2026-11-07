project('metrowl', 'c',
  version: '0.1.0',
  meson_version: '>=1.0.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'],
)

cc = meson.get_compiler('c')
add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_GNU_SOURCE',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-sign-compare',
  language: 'c')

# ---------------------------------------------------------------- dependencies
wlroots = dependency('wlroots-0.18', version: '>=0.18.0', required: false)
if not wlroots.found()
  wlroots = dependency('wlroots-0.19', required: true)
endif
wayland_server = dependency('wayland-server')
xkbcommon      = dependency('xkbcommon')
pangocairo     = dependency('pangocairo')
cairo          = dependency('cairo')
fontconfig     = dependency('fontconfig')
glib           = dependency('glib-2.0')
m_dep          = cc.find_library('m', required: false)
threads        = dependency('threads')
libdrm         = dependency('libdrm')
pixman         = dependency('pixman-1')
libinput       = dependency('libinput')

# Optional: SVG icons of .desktop entries.
rsvg = dependency('librsvg-2.0', required: get_option('svg'))
# Optional: org.freedesktop.Notifications daemon (needs an sd-bus implementation).
sdbus = dependency('libsystemd', required: false)
if not sdbus.found()
  sdbus = dependency('libelogind', required: false)
endif
if not sdbus.found()
  sdbus = dependency('basu', required: false)
endif
if get_option('notifications').enabled() and not sdbus.found()
  error('notifications=enabled but no sd-bus implementation (libsystemd/libelogind/basu) found')
endif

conf = configuration_data()
conf.set_quoted('METROWL_VERSION', meson.project_version())
conf.set_quoted('METROWL_SOURCE_DATADIR', meson.project_source_root() / 'data')
conf.set_quoted('METROWL_DATADIR', get_option('prefix') / get_option('datadir') / 'metrowl')
conf.set10('HAVE_RSVG', rsvg.found())
conf.set10('HAVE_SDBUS', sdbus.found() and not get_option('notifications').disabled())
conf.set10('WLR_IS_0_19', wlroots.version().version_compare('>=0.19.0'))
config_h = configure_file(output: 'build_config.h', configuration: conf)

# ------------------------------------------------------------ protocol headers
wayland_scanner = find_program('wayland-scanner', native: true)
wl_protocols = dependency('wayland-protocols', version: '>=1.32')
wl_protocols_dir = wl_protocols.get_variable('pkgdatadir')

protocols = [
  wl_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  wl_protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocols_dir / 'unstable/tablet/tablet-unstable-v2.xml',
]
protocol_headers = []
foreach p : protocols
  protocol_headers += custom_target(
    p.underscorify() + '_h',
    input: p,
    output: '@BASENAME@-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# -------------------------------------------------------------------- sources
third_party = static_library('third_party',
  ['third_party/cJSON.c'],
  c_args: ['-w'],
  include_directories: include_directories('third_party'),
)

shell_src = files(
  'src/main.c',
  'src/server.c',
  'src/output.c',
  'src/input.c',
  'src/views.c',
  'src/textinput.c',
  'src/ipc.c',
  'src/notifyd.c',
  'src/proc.c',
  'src/sys.c',
  'src/config.c',
  'src/desktop.c',
  'src/i18n.c',
  'src/util.c',
  'src/ui/gfx.c',
  'src/ui/icons.c',
  'src/ui/ui.c',
  'src/shell/shell.c',
  'src/shell/lock.c',
  'src/shell/start.c',
  'src/shell/applist.c',
  'src/shell/chrome.c',
  'src/shell/osk.c',
  'src/shell/actioncenter.c',
  'src/shell/switcher.c',
  'src/shell/search.c',
  'src/shell/dialogs.c',
  'src/shell/wallpaper.c',
  'src/apps/apps.c',
  'src/apps/settings.c',
  'src/apps/store.c',
  'src/apps/photos.c',
  'src/apps/calculator.c',
  'src/apps/phone.c',
  'src/apps/clock.c',
  'src/apps/calendar.c',
  'src/apps/weather.c',
  'src/apps/people.c',
  'src/apps/notes.c',
  'src/apps/files.c',
  'src/apps/music.c',
  'src/apps/video.c',
  'src/apps/battery.c',
  'src/apps/storage.c',
  'src/apps/terminal.c',
  'src/apps/minesweeper.c',
  'src/apps/recorder.c',
  'src/apps/player.c',
)

metrowl = executable('metrowl',
  shell_src, protocol_headers, config_h,
  include_directories: [include_directories('src', 'third_party'), include_directories('.')],
  dependencies: [wlroots, wayland_server, xkbcommon, pangocairo, cairo,
                 fontconfig, glib, rsvg, sdbus, m_dep, threads, libdrm, pixman, libinput],
  link_with: third_party,
  install: true,
)

executable('metrowlctl', 'tools/metrowlctl.c', install: true)

install_subdir('data/fonts', install_dir: get_option('datadir') / 'metrowl')
install_data('data/store/catalog.json', install_dir: get_option('datadir') / 'metrowl' / 'store')
install_data('data/metrowl.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
install_data('data/metrowl-session', install_dir: get_option('bindir'),
             install_mode: 'rwxr-xr-x')

# ---------------------------------------------------------------------- tests
test_i18n = executable('test_i18n', 'tests/test_i18n.c', 'src/i18n.c', 'src/util.c',
  include_directories: [include_directories('src', 'third_party'), include_directories('.')],
  dependencies: [glib, m_dep, wlroots, wayland_server], link_with: third_party)
test('i18n', test_i18n)

# Every _("...") string needs a Turkish translation with matching printf specifiers.
python3 = find_program('python3', required: false)
if python3.found()
  test('i18n-coverage', python3, args: [files('tests/check_i18n.py'), meson.project_source_root() / 'src'])
endif
