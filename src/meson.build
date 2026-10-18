deps = [glib_dep, gobject_dep, gtk_dep, cc.find_library('m')]

# lifetime.c is the only file allowed to use the raw signal/source APIs (see prelude.h).
lifetime_lib = static_library('gbk-lifetime', 'lifetime.c',
  c_args : '-DGBK_LIFETIME_IMPL',
  include_directories : config_inc,
  dependencies : deps,
)

# Non-GUI core: everything testable without a display.
core_lib = static_library('gbk-core',
  'filetype.c',
  'natsort.c',
  'loc.c',
  'entry.c',
  'inotify-parse.c',
  'inotify-hub.c',
  'scanner.c',
  'dirmodel.c',
  'command.c',
  'config.c',
  'launch.c',
  'open.c',
  'menu.c',
  'fileops.c',
  'cliptext.c',
  'job.c',
  'devices.c',
  include_directories : config_inc,
  link_with : lifetime_lib,
  dependencies : deps,
)

# GUI: needs a display (tests run under Xvfb).
gui_lib = static_library('gbk-gui',
  'context.c',
  'dnd.c',
  'clipboard.c',
  'jobui.c',
  'view.c',
  'browser.c',
  'pathbar.c',
  'props.c',
  'window.c',
  include_directories : config_inc,
  link_with : [core_lib, lifetime_lib],
  dependencies : deps,
)

core_inc = include_directories('.')

executable('quoin', 'main.c',
  include_directories : config_inc,
  link_with : [gui_lib, core_lib, lifetime_lib],
  dependencies : deps,
)
