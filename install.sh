#!/usr/bin/env bash
# Install Quoin into ~/.local (no root needed):
#
#   curl -fsSL https://raw.githubusercontent.com/odclcslkrpb5k/quoin/main/install.sh | bash
#
# Environment (all optional):
#   QUOIN_VERSION=<hash>   install that release instead of the latest
#   QUOIN_PREFIX=<dir>     install under <dir> instead of ~/.local
#   QUOIN_FROM=<dir>       install files from <dir> instead of downloading (./build install)
#   QUOIN_ANY_OS=1         skip the Ubuntu 26.04 check
#
# Replaces an install from before the rename (gbk-file) under the same prefix.
#
# Everything is inside main(), called on the last line: if the download of this script is
# cut short, bash has nothing complete to run.
set -euo pipefail

main() {
  local repo=odclcslkrpb5k/quoin
  local prefix=${QUOIN_PREFIX:-$HOME/.local}
  local version=${QUOIN_VERSION:-}

  say() { printf '%s\n' "$*" >&2; }
  die() { say "quoin install: $*"; exit 1; }

  # The prefix goes verbatim into the .desktop Exec line and a sed replacement: allow only
  # characters that need no quoting in either.
  [[ $prefix == /* && ! $prefix =~ [^A-Za-z0-9/._+-] ]] \
    || die "install location must be an absolute path of [A-Za-z0-9/._+-]: $prefix"
  [[ -z $version || $version =~ ^[0-9a-f]{12}$ ]] \
    || die "QUOIN_VERSION must be a 12-character build hash, got: $version"

  local icon=io.github.odclcslkrpb5k.Quoin.svg
  local src
  if [[ -n ${QUOIN_FROM:-} ]]; then
    src=$QUOIN_FROM
  else
    # Release binaries are built on Ubuntu 26.04 (x86_64) against its GTK and GLib.
    [[ $(uname -m) == x86_64 ]] || die "release binaries are x86_64 only (this is $(uname -m))"
    if [[ -z ${QUOIN_ANY_OS:-} ]]; then
      local id="" ver=""
      if [[ -r /etc/os-release ]]; then
        id=$(. /etc/os-release && printf '%s' "${ID:-}")
        ver=$(. /etc/os-release && printf '%s' "${VERSION_ID:-}")
      fi
      [[ $id == ubuntu && $ver == 26.04 ]] || die "release binaries are built for Ubuntu 26.04;
this is ${id:-unknown} ${ver:-}. They may not start here. To try anyway: QUOIN_ANY_OS=1.
Or build from source: see https://github.com/$repo#building"
    fi
    command -v curl >/dev/null || die "curl is required"
    command -v sha256sum >/dev/null || die "sha256sum is required"

    local base
    if [[ -n $version ]]; then
      base=https://github.com/$repo/releases/download/$version
    else
      base=https://github.com/$repo/releases/latest/download
    fi
    src=$(mktemp -d)
    # Expanded now: `src` is local and gone by the time the EXIT trap runs.
    trap "rm -rf '$src'" EXIT
    local f
    for f in quoin quoin-serve quoin.desktop.in "$icon" SHA256SUMS; do
      curl -fsSL --proto '=https' --tlsv1.2 -o "$src/$f" "$base/$f" \
        || die "download failed: $base/$f"
    done
    # Catches corrupted or truncated downloads. (The sums come from the same release, so
    # this is not a defence against a compromised GitHub account.)
    (cd "$src" && sha256sum --quiet --strict -c SHA256SUMS) || die "checksum mismatch"
  fi

  [[ -f $src/quoin && -f $src/quoin-serve && -f $src/quoin.desktop.in && -f $src/$icon ]] \
    || die "missing files in $src"
  chmod 755 "$src/quoin" "$src/quoin-serve"
  local got
  got=$("$src/quoin" --version) || die "the binary doesn't run here (missing libraries?)"
  "$src/quoin-serve" --version >/dev/null || die "quoin-serve doesn't run here"

  local bindir=$prefix/bin appdir=$prefix/share/applications
  local icondir=$prefix/share/icons/hicolor/scalable/apps
  mkdir -p "$bindir" "$appdir" "$icondir"
  # Copy beside the target, then rename over it: atomic, and running instances keep the old
  # file instead of seeing a half-written one.
  cp "$src/quoin" "$bindir/.quoin.new"
  chmod 755 "$bindir/.quoin.new"
  mv -f "$bindir/.quoin.new" "$bindir/quoin"
  # What Quoin sends to another machine to browse it (docs/remote-plan.md); found next to
  # the quoin binary.
  cp "$src/quoin-serve" "$bindir/.quoin-serve.new"
  chmod 755 "$bindir/.quoin-serve.new"
  mv -f "$bindir/.quoin-serve.new" "$bindir/quoin-serve"
  sed "s|@BIN@|$bindir/quoin|g" "$src/quoin.desktop.in" > "$appdir/.quoin.desktop.new"
  mv -f "$appdir/.quoin.desktop.new" "$appdir/quoin.desktop"
  cp "$src/$icon" "$icondir/.$icon.new"
  mv -f "$icondir/.$icon.new" "$icondir/$icon"
  touch "$prefix/share/icons/hicolor"     # tells icon caches to look again
  say "installed $got"
  say "  $bindir/quoin"
  say "  $bindir/quoin-serve"
  say "  $appdir/quoin.desktop"
  say "  $icondir/$icon"

  # The same program under its old name, installed by this script before the rename.
  local old_default=false
  if [[ $prefix == "$HOME/.local" ]] && command -v xdg-mime >/dev/null \
     && [[ $(xdg-mime query default inode/directory 2>/dev/null) == gbk-file.desktop ]]; then
    old_default=true
  fi
  if [[ -e $bindir/gbk-file || -e $appdir/gbk-file.desktop ]]; then
    rm -f "$bindir/gbk-file" "$appdir/gbk-file.desktop"
    say "removed the old gbk-file install (renamed to Quoin)"
  fi
  if $old_default; then
    xdg-mime default quoin.desktop inode/directory
    say "folders now open with Quoin (they opened with gbk-file)"
  elif [[ $(xdg-mime query default inode/directory 2>/dev/null) != quoin.desktop ]]; then
    say "To make it the default for folders:"
    say "  xdg-mime default quoin.desktop inode/directory"
  fi
}

main "$@"
