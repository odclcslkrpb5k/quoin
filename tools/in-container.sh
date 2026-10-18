#!/usr/bin/env bash
# Runs inside the build image. Invoked via ./build on the host.
set -euo pipefail
cd /src

setup() {  # setup <dir> <meson options...>
  local dir=$1; shift
  mkdir -p out
  printf '%s\n' "$GBK_VERSION" > out/VERSION    # meson.build reads it as the project version
  rm -f "$dir/released"    # any rebuild voids the release stamp until the gate passes again
  if [[ -d $dir/meson-private ]]; then
    meson setup --reconfigure "$dir" "$@" | tail -n 25
  else
    meson setup "$dir" "$@" | tail -n 25
  fi
}

# GUI tests need a display: a private Xvfb for the life of this script.
start_x() {
  [[ -n ${DISPLAY:-} ]] && return
  Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
  local pid=$!
  for _ in $(seq 100); do
    [[ -e /tmp/.X11-unix/X99 ]] && break
    sleep 0.05
  done
  export DISPLAY=:99 GDK_BACKEND=x11
  # A private session bus, as every desktop has (GTK's mount dialogs ask it for a handler).
  local bus
  bus=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
  export DBUS_SESSION_BUS_ADDRESS=${bus%%$'\n'*}
  trap "kill $pid ${bus##*$'\n'} 2>/dev/null" EXIT
  # No a11y bus or dconf in the container: keep GTK from trying (and warning).
  export NO_AT_BRIDGE=1 GTK_A11Y=none GSETTINGS_BACKEND=memory
  mkdir -p out/artifacts
}

debug() {
  start_x
  echo "== debug (ASan + UBSan) =="
  setup out/debug -Dbuildtype=debug -Db_sanitize=address,undefined
  meson compile -C out/debug
  meson test -C out/debug --print-errorlogs
}

analyze() {
  echo "== analyze (gcc -fanalyzer) =="
  setup out/analyze -Dbuildtype=debug -Danalyzer=true
  meson compile -C out/analyze
}

# fuzz [seconds-per-target]: libFuzzer (clang, ASan+UBSan) on every target. The working
# corpus persists in out/fuzz/corpus; crashing inputs land in out/fuzz/crashes.
# A crash fails the run: minimise it into tests/fuzz/seeds/<target>/ so it becomes a
# permanent regression test.
fuzz() {
  local secs=${1:-60}
  echo "== fuzz (${secs}s per target) =="
  CC=clang setup out/fuzz -Dbuildtype=debug -Dfuzzing=true \
    -Db_sanitize=address,undefined -Db_lundef=false
  meson compile -C out/fuzz
  mkdir -p out/fuzz/crashes
  local status=0
  for bin in out/fuzz/tests/fuzz-*; do
    [[ -f $bin && -x $bin ]] || continue
    local name=${bin##*/fuzz-}
    local corpus=out/fuzz/corpus/$name
    mkdir -p "$corpus"
    echo "-- $name"
    if ! "$bin" -max_total_time="$secs" -timeout=10 -max_len=4096 -print_final_stats=1 \
         -artifact_prefix="out/fuzz/crashes/$name-" "$corpus" "tests/fuzz/seeds/$name" \
         2>"out/fuzz/$name.log"; then
      echo "   CRASH: see out/fuzz/$name.log and out/fuzz/crashes/" >&2
      status=1
    fi
    grep -E "stat::number_of_executed_units|cov:" "out/fuzz/$name.log" | tail -2 | sed 's/^/   /'
  done
  return $status
}

# Every check, in one command: sanitizer tests, the analyzers, fuzzing, then the hardened
# build that ships, tested again.
# The C sources clang-format owns: `check` refuses them unformatted, `format` rewrites them.
format_files() {
  ls src/*.c src/*.h tests/*.c tests/fuzz/*.c
}

format() {
  format_files | xargs clang-format -i
  echo "formatted $(format_files | wc -l) files"
}

check() {
  debug
  analyze
  echo "== desktop entry =="
  sed "s|@BIN@|/usr/bin/quoin|g" data/quoin.desktop.in > out/quoin.desktop
  desktop-file-validate out/quoin.desktop
  echo "== formatting (clang-format) =="
  format_files | xargs clang-format --dry-run --Werror
  echo "== source rules =="
  python3 tools/check-source.py self-test
  python3 tools/check-source.py
  echo "== static analysis (clang-tidy, cppcheck) =="
  python3 tools/analysis.py run
  python3 tools/analysis.py compare
  fuzz 20
  echo "== release (hardened) =="
  start_x
  # b_ndebug=false: assertions stay on in the shipped binary.
  setup out/release -Dbuildtype=debugoptimized -Db_ndebug=false -Dhardening=true
  meson compile -C out/release
  tools/check-binary.sh out/release/src/quoin
  tools/check-binary.sh out/release/src/quoin-serve \
    '^(libglib-2\.0|libgobject-2\.0|libgio-2\.0|libc|libm|libgcc_s)\.so\.[0-9]+$'
  meson test -C out/release --print-errorlogs
  # The GUI suites again, several times: a test that reads state before the main loop has
  # delivered it passes or fails by timing, and the fast release build is where it shows.
  meson test -C out/release --print-errorlogs --repeat 3 window view devices
}

release() {
  check
  # ./build install trusts only a build whose whole gate passed for this exact hash.
  printf '%s\n' "$GBK_VERSION" > out/release/released
  echo
  echo "built: out/release/src/quoin ($(out/release/src/quoin --version))"
}

bench() {
  echo "== bench (release build, data in out/bench) =="
  setup out/release -Dbuildtype=debugoptimized -Db_ndebug=false -Dhardening=true
  meson compile -C out/release
  meson test -C out/release --benchmark --verbose 2>&1 | grep -E "^ *(entries|first|full|per|model|toggle|live|readdir|statx|build)|scan-[0-9]+ " | sed 's/^ *[0-9]*\/[0-9]* //'
}

case "${1:-}" in
  check) check ;;
  debug) debug ;;
  analyze) analyze ;;
  release) release ;;
  fuzz) fuzz "${2:-60}" ;;
  bench) bench ;;
  format) format ;;
  shell) exec bash ;;
  *) echo "unknown command: ${1:-}" >&2; exit 2 ;;
esac
