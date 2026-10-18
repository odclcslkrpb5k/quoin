test_env = environment()
# Any GLib warning or critical (e.g. a failed g_return_if_fail) aborts the test.
test_env.set('G_DEBUG', 'fatal-warnings,fatal-criticals,gc-friendly')
test_env.set('G_ENABLE_DIAGNOSTIC', '1')
# No OpenGL: Mesa's GLX setup, when GDK probes it, leaks allocations LSan would report
# (fontconfig stays the only suppression).
test_env.set('GDK_GL', 'disable')
# A callback of ours holding the main loop longer than this aborts (src/lifetime.h). The
# worst seen under ASan was 85 ms (2026-10-17).
test_env.set('GBK_STALL_MS', '250')
test_env.set('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1')
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
test_env.set('LSAN_OPTIONS', 'suppressions=' + meson.current_source_dir() / 'lsan.supp' + ':print_suppressions=0')
# Hermetic: never read the invoking user's real config/data.
test_env.set('XDG_CONFIG_HOME', '/nonexistent/config')
test_env.set('XDG_DATA_HOME', '/nonexistent/data')
test_env.set('GBK_SOURCE_ROOT', meson.project_source_root())
test_env.set('GBK_ARTIFACTS', meson.project_source_root() / 'out' / 'artifacts')
test_env.set('QUOIN_SERVE', serve_exe.full_path())

test_libs = [core_lib, lifetime_lib]
gui_test_libs = [gui_lib, core_lib, lifetime_lib]
gui_tests = ['view', 'window', 'devices', 'remotewin']
# GUI tests drive real X input (XTest) where GTK only listens to the X server: drag and drop.
gui_test_deps = deps + [dependency('x11'), dependency('xtst')]
test_inc = [config_inc, core_inc]

unit_tests = ['lifetime', 'filetype', 'natsort', 'loc', 'dirmodel', 'scanner', 'command', 'config', 'menu', 'open', 'fileops', 'clip', 'job', 'proto', 'peer', 'serve', 'conn', 'remote', 'sshd']
fuzz_targets = ['filetype', 'natsort', 'inotify', 'entry', 'command', 'config', 'clip', 'proto']

if not get_option('fuzzing')
  foreach name : unit_tests
    exe = executable('test-' + name, 'test-' + name + '.c',
      include_directories : test_inc,
      link_with : test_libs,
      dependencies : deps,
    )
    test(name, exe, env : test_env, protocol : 'tap', args : ['--tap'], timeout : 120)
  endforeach
  foreach name : gui_tests
    exe = executable('test-' + name, 'test-' + name + '.c',
      include_directories : test_inc,
      link_with : gui_test_libs,
      dependencies : gui_test_deps,
    )
    # One at a time: they share the Xvfb display.
    test(name, exe, env : test_env, protocol : 'tap', args : ['--tap'], timeout : 120,
         is_parallel : false)
  endforeach
endif

# Fuzz targets. Normal builds replay the seed corpus as a test (under the sanitizers in the
# debug build); fuzzing builds produce real libFuzzer binaries for ./build fuzz.
foreach name : fuzz_targets
  seeds = meson.current_source_dir() / 'fuzz' / 'seeds' / name
  if get_option('fuzzing')
    executable('fuzz-' + name, 'fuzz' / 'fuzz-' + name + '.c',
      include_directories : test_inc + [include_directories('fuzz')],
      link_with : test_libs,
      link_args : '-fsanitize=fuzzer',
      dependencies : deps,
    )
  else
    exe = executable('replay-' + name, 'fuzz' / 'fuzz-' + name + '.c', 'fuzz' / 'driver.c',
      include_directories : test_inc + [include_directories('fuzz')],
      link_with : test_libs,
      dependencies : deps,
    )
    test('fuzz-seeds-' + name, exe, env : test_env, args : [seeds], timeout : 300)
  endif
endforeach

# ./build bench: release build, data on the real host filesystem under out/bench.
bench_dir = meson.project_source_root() / 'out' / 'bench'
bench_exe = executable('bench-scan', 'bench-scan.c',
  include_directories : test_inc,
  link_with : test_libs,
  dependencies : deps,
)
foreach n : ['1000', '10000', '100000']
  benchmark('scan-' + n, bench_exe, args : [bench_dir, n], timeout : 600)
endforeach
