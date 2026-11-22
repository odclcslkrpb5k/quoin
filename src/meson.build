deps = [glib_dep, gobject_dep, gtk_dep, cc.find_library('m')]
# What quoin-serve may link: GLib/GIO and libc, never GTK (it runs on machines without a
# display; tools/check-binary.sh proves it). GTK's headers are still compiled in, because
# prelude.h includes them; nothing from them is linked.
serve_deps = [glib_dep, gobject_dep, gio_dep, cc.find_library('m'),
              gtk_dep.partial_dependency(compile_args : true, includes : true)]

# lifetime.c is the only file allowed to use the raw signal/source APIs (see prelude.h).
lifetime_lib = static_library('gbk-lifetime', 'lifetime.c',
  c_args : '-DGBK_LIFETIME_IMPL',
  include_directories : config_inc,
  dependencies : deps,
)

# Shared with quoin-serve: files, folders and the wire protocol, no main loop, no GTK.
serve_lib = static_library('gbk-serve-core',
  'filetype.c',
  'natsort.c',
  'entry.c',
  'inotify-parse.c',
  'inotify-hub.c',
  'scanner.c',
  'fileops.c',
  'proto.c',
  'peer.c',
  'runcmd.c',
  'capture.c',
  'git.c',
  'gitwatch.c',
  include_directories : config_inc,
  dependencies : serve_deps,
)

# Non-GUI core: everything testable without a display.
core_lib = static_library('gbk-core',
  'loc.c',
  'dirmodel.c',
  'command.c',
  'config.c',
  'launch.c',
  'open.c',
  'menu.c',
  'cliptext.c',
  'conn.c',
  'remote.c',
  'job.c',
  'edit-store.c',
  'notify.c',
  'devices.c',
  'viewstate.c',
  'appimage-out.c',
  include_directories : config_inc,
  link_with : [serve_lib, lifetime_lib],
  dependencies : deps,
)

# GUI: needs a display (tests run under Xvfb).
gui_lib = static_library('gbk-gui',
  'context.c',
  'dnd.c',
  'clipboard.c',
  'jobui.c',
  'appchoose.c',
  'edits.c',
  'fetch.c',
  'view.c',
  'appicons.c',
  'browser.c',
  'pathbar.c',
  'props.c',
  'window.c',
  include_directories : config_inc,
  link_with : [core_lib, lifetime_lib],
  dependencies : deps,
)

core_inc = include_directories('.')

# Reading AppImages: squashfs images from wherever the user got them, so only the
# quoin-appimage helper (a process of its own) and the tests and fuzzer link this.
compress_deps = [dependency('zlib'), dependency('libzstd'), dependency('liblzma')]
appimage_lib = static_library('gbk-appimage', 'squashfs.c', 'appimage.c',
  include_directories : config_inc,
  dependencies : serve_deps + compress_deps,
)
appimage_exe = executable('quoin-appimage', 'appimage-main.c',
  include_directories : config_inc,
  link_with : appimage_lib,
  dependencies : serve_deps + compress_deps + [dependency('gdk-pixbuf-2.0')],
)

serve_exe = executable('quoin-serve', 'serve.c',
  include_directories : config_inc,
  link_with : serve_lib,
  dependencies : serve_deps,
)

executable('quoin', 'main.c',
  include_directories : config_inc,
  link_with : [gui_lib, core_lib, lifetime_lib],
  dependencies : deps,
)
