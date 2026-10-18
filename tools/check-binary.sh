#!/usr/bin/env bash
# Proves the hardening reached the binary: a flag misspelled or dropped by a build change
# fails here instead of shipping quietly without its protection.
#
# Two kinds of evidence. readelf shows the properties (PIE, BIND_NOW, ...), but Ubuntu's GCC
# turns most of them on by default, so they pass without our flags too. The flags
# themselves are in the debug info: GCC records each compilation unit's options in
# DW_AT_producer (-grecord-gcc-switches, on by default), so every unit must carry each one.
#
#   tools/check-binary.sh <elf> [needed]
#
# `needed`, if given, is an extended regex every NEEDED library must match: the libraries
# the binary may load, and no others (quoin-serve: GLib and libc, never GTK).
set -uo pipefail
bin=${1:?usage: check-binary.sh <elf> [needed]}
allowed=${2:-}
fail=0
check() {  # check <what> <command...>: the command must succeed
  local what=$1; shift
  if "$@" >/dev/null 2>&1; then
    printf '  ok    %s\n' "$what"
  else
    printf '  FAIL  %s\n' "$what"
    fail=1
  fi
}

header=$(readelf -hW "$bin") || exit 1
dynamic=$(readelf -dW "$bin")
segments=$(readelf -lW "$bin")
notes=$(readelf -nW "$bin")
imports=$(readelf --dyn-syms -W "$bin" | awk '$7 == "UND" { print $8 }')

echo "binary checks: $bin"
check "position independent (-pie)" \
  grep -qE 'Type: +DYN' <<<"$header"
check "PIE flag (-pie)" \
  grep -qE 'FLAGS_1.*\bPIE\b' <<<"$dynamic"
check "immediate binding (-z now)" \
  grep -qE '\(FLAGS\).*\bBIND_NOW\b|FLAGS_1.*\bNOW\b' <<<"$dynamic"
check "read-only relocations (-z relro)" \
  grep -qE '^ *GNU_RELRO ' <<<"$segments"
check "stack not executable (-z noexecstack)" \
  grep -qE '^ *GNU_STACK .* RW +0x' <<<"$segments"
if [[ -n $allowed ]]; then
  needed=$(sed -n 's/.*(NEEDED) *Shared library: \[\(.*\)\]/\1/p' <<<"$dynamic")
  check "loads only $allowed ($(wc -w <<<"$needed") libraries)" \
    bash -c '[[ -n $1 ]] && ! grep -vqE "$2" <<<"$1"' _ "$needed" "$allowed"
fi
check "no text relocations" \
  bash -c '! grep -q TEXTREL <<<"$1"' _ "$dynamic"
check "stack protector (-fstack-protector-strong)" \
  grep -qE '^__stack_chk_fail(@|$)' <<<"$imports"
check "fortified calls (-D_FORTIFY_SOURCE=3)" \
  grep -qE '^__[a-z_]+_chk(@|$)' <<<"$imports"
check "indirect branch tracking (-fcf-protection=full)" \
  grep -qE 'x86 feature:.*\bIBT\b' <<<"$notes"
check "shadow stack (-fcf-protection=full)" \
  grep -qE 'x86 feature:.*\bSHSTK\b' <<<"$notes"
# Code-generation flags, per compilation unit (debug info: the release build keeps -g).
producers=$(readelf --debug-dump=info "$bin" 2>/dev/null \
  | sed -n 's/.*DW_AT_producer *: *(indirect string[^)]*): //p; s/.*DW_AT_producer *: *//p' \
  | grep '^GNU C' | sed 's/$/ /')
units=$(grep -c . <<<"$producers")
check "debug info names the compilation units ($units)" test "$units" -gt 0
for flag in '-D _FORTIFY_SOURCE=3' -O2 -fstack-protector-strong -fstack-clash-protection \
            -fcf-protection=full -fzero-call-used-regs=used-gpr -ftrivial-auto-var-init=zero \
            -fzero-init-padding-bits=all -fno-strict-aliasing -fno-strict-overflow \
            -fno-delete-null-pointer-checks -fstrict-flex-arrays=3; do
  missing=$(grep -vcF -- " $flag " <<<"$producers")
  check "every unit compiled with $flag" test "$missing" -eq 0
done
exit $fail
