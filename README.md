# Quoin
<img src="data/icons/io.github.odclcslkrpb5k.Quoin.svg" width="96" alt="" align="right">

A small, fast file manager for Linux desktops (X11, GTK 3), modelled on
[Thunar](https://docs.xfce.org/xfce/thunar/start)'s compact view. It is written in C for
one person's daily use. The goals are to start instantly, stay instant in huge folders,
use little memory, and never crash.

A *quoin* is a wedge: in a ship's hold it keeps the cargo from shifting, and in a printer's
forme it locks the type in place. (Formerly gbk-file; the installer replaces an old install.)

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/odclcslkrpb5k/quoin/main/install.sh | bash
```

This installs the latest release into `~/.local`. It needs no root and copies three files:

- `~/.local/bin/quoin`
- `~/.local/share/applications/quoin.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/io.github.odclcslkrpb5k.Quoin.svg`

Run the same command again to update. To make Quoin open folders by default:

```sh
xdg-mime default quoin.desktop inode/directory
```

The installer takes these options, set as environment variables before `bash`. For
example, `curl … | QUOIN_VERSION=<hash> bash`.

| Variable | Effect |
| --- | --- |
| `QUOIN_VERSION=<hash>` | Install a specific release instead of the latest. |
| `QUOIN_PREFIX=<dir>` | Install under `<dir>` instead of `~/.local`. |
| `QUOIN_ANY_OS=1` | Skip the Ubuntu 26.04 check (see below). |

To uninstall, run `rm ~/.local/bin/quoin ~/.local/share/applications/quoin.desktop ~/.local/share/icons/hicolor/scalable/apps/io.github.odclcslkrpb5k.Quoin.svg`.

**Supported systems.** Release binaries are built on **Ubuntu 26.04, x86_64**, against
that release's GTK and GLib. The installer refuses other systems rather than leave you a
program that won't start. On anything else, [build from source](#building).

## What it does

- **One view:** compact icon and name columns that scroll sideways, like Thunar's compact
  view. Sort by name, date modified or size, always with folders first.
- **Live updates:** the view follows changes on disk as they happen; there is no polling
  and no refresh button.
- **Big folders:** a folder of 100,000 files shows its first items within milliseconds and
  uses about 150 bytes per entry.
- **No thumbnails.** File types are decided by extension alone, so the program never reads
  a file's contents just to show the folder.
- **Tabs.** Each tab has its own path bar. Right-click a path bar button for that folder's
  menu.
- **Drag and drop.** Drag files out to any application, which decides what to do with
  them. Dropping files *into* Quoin (onto a folder, empty space or a path bar button)
  always asks first: **Move Here**, **Copy Here** or **Cancel**, at the pointer. Nothing is
  moved by accident.
- **Copies are like `cp -a`:** permissions, ownership (where allowed), timestamps,
  extended attributes and ACLs, hard links within the copy, and special files are kept.
- **Create files and folders** from the right-click menu on empty space.
- **Devices** in the ☰ menu: File System (/), then USB sticks, other partitions, phones
  and the like. Open one (mounting it first if needed, with a password prompt if the
  system asks), unmount it, or eject it. Before a device is unmounted, tabs showing it move off it, so Quoin itself
  never keeps it busy. Uses udisks2 through gvfs where installed, as Thunar does.
- **Other machines, over ssh, without mounting anything.** Type `ws1:` (that machine's
  home), `ws1:/srv/music` or `ws1:notes` in the location bar (Ctrl+L), run
  `quoin ws1:/srv`, or pick a machine from **Network** in the ☰ menu (the hosts in your
  `~/.ssh/config`). Folders there list and update live, as here. A laptop going to sleep
  or changing networks never freezes anything: the folder keeps its last listing while
  Quoin reconnects by itself. The first visit sends a small helper program, quoin-serve,
  to `~/.cache/quoin/serve/` on that machine; nothing needs installing there. Logins use
  your ssh keys and agent; when ssh needs a passphrase or a host key accepted, **Log in
  in a terminal** does it once. You can look around, see Properties, open a terminal
  there (F4), copy a path (`ws1:/srv/x`, as scp takes it), and **copy or move** files
  and folders either way, or within that machine (Copy or Cut, then Paste). A move deletes
  an original only once the copy is safely stored, and only if it hasn't changed since.
  **Open** a file there and it is fetched into `~/.cache/quoin/remote/` and opened from
  there; each save is uploaded back, but only over the file as it was when you opened it
  (if someone changed it meanwhile, you choose: Replace, Keep Both, or Don't Upload).
  The status bar says what is happening ("Saving notes.txt to ws1…", "Saved notes.txt to
  ws1 at 14:02"); while Quoin isn't the window in use, a desktop notification says it too
  (briefly for a save, until dismissed for a failure or a question). Quoin keeps running
  after its last window closes while a file is still open in an app, and says so (it can
  tell when an editor closes the file, even if the editor keeps running). Files being
  edited show in bold, with a small emblem, in their folder, and **Files Being Edited** in the ☰ menu
  lists them. A save it couldn't upload is offered the next time Quoin starts. Renaming,
  deleting and the like there, and drag and drop with other machines, come next (see
  [docs/remote-plan.md](docs/remote-plan.md)).
- **Properties** (right-click menu or Alt+Enter): dates (modified, accessed, changed,
  created), size, owner, group, link target, and read / write / execute permissions you can
  change with Apply. For files, the right-click menu also has an **Executable** check
  item that switches execute permission on or off in one click.
- **Copy, cut, paste, trash, delete and rename.** File operations run in the background,
  with a progress window and Skip / Replace / Keep Both choices on conflicts. The clipboard
  works with other file managers.
- **Running files:**
  - Executable scripts run in a terminal that stays open, so you see their output.
  - Other executables run directly.
  - Everything else opens in its default application.
  - The right-click menu also offers Execute, Execute in Terminal and Open With.
- **Your own right-click actions** per file type, such as "Extract Here" for archives
  (see [Configuration](#configuration)).
- **Status bar:** folder and file counts, the selection's size and free space on the left.
  On the right are the sort button (click to cycle) and the `.*` hidden-files toggle.

### Keyboard

| Key | Action |
| --- | --- |
| Arrows, Page Up/Down, Home/End | Move; with Shift, extend the selection; with Ctrl, move without selecting |
| Space | Toggle selection of the item under the cursor |
| Ctrl+A | Select all |
| Typing | Jump to the first name starting with what you typed |
| Enter | Open |
| Alt+Left / Alt+Right / Alt+Up | Back / forward / parent folder |
| Alt+Home | Home folder |
| Mouse back / forward buttons | Back / forward |
| Ctrl+H | Show or hide hidden files |
| Ctrl+L | Type a location |
| F4 | Open a terminal here |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste |
| Delete | Move to trash |
| Shift+Delete | Delete permanently (asks first) |
| F2 | Rename |
| Alt+Enter | Properties (of the selected item, or the folder) |
| Menu key, Shift+F10 | Right-click menu |
| Ctrl+T / Ctrl+W | New tab / close tab |
| Ctrl+Page Up / Ctrl+Page Down | Previous / next tab |
| Ctrl+N / Ctrl+Q | New window / close window |

While you type in the location bar, keys go only to the text. Delete and Ctrl+V edit the
path there; they never act on your files.

## Configuration

The config file is `~/.config/quoin/quoin.conf`, in INI style. Every setting is
optional. [`docs/example.conf`](docs/example.conf) documents them all, and the test suite
loads it, so it always matches what the program accepts. You can set:

- the terminal commands (F4, and running scripts);
- whether hidden files are shown at startup;
- the starting sort;
- script interpreters;
- extra extension → file type mappings;
- right-click actions.

The file is read strictly. One unknown key or bad value rejects the whole file, and
Quoin then reports every problem and uses the defaults, so a typo can't half-apply.

## Building

Everything builds inside a pinned Docker image (`Dockerfile`, `compose.yaml`), so the only
host requirements are Docker with Compose, and bash. Nothing is fetched during a build, and
versions are git hashes.

```sh
./build check     # every check: everything below but bench, then the hardened build's tests
./build debug     # AddressSanitizer + UBSan build, all tests
./build analyze   # GCC static analyzer; any finding fails
./build fuzz 60   # libFuzzer on every parser, 60 s each
./build bench     # scan / sort / memory benchmark
./build release   # check (20 s fuzzing) on a clean tree, marking the binary releasable
./build install   # install the release build into ~/.local (runs release if needed)
./build publish   # upload the release build as a GitHub release (maintainer)
```

`release`, `install` and `publish` refuse to run with uncommitted changes, so a version
hash always names exact source.

The build environment is Ubuntu 26.04 with GCC 15, GTK 3.24, GLib 2.88 and Meson, every
package from one dated snapshot of the Ubuntu archive. `./build check` runs the test suites
under AddressSanitizer and UBSan, GCC's static analyzer (taught GLib's allocators),
clang-tidy and cppcheck behind a baseline that only shrinks, the project's own source rules
(`tools/check-source.py`), clang-format, libFuzzer on every parser, and the hardened build's
tests and binary checks. How it got there, and what each check found:
[docs/paranoid-c-adoption.md](docs/paranoid-c-adoption.md).
Building natively without Docker should work with those or newer versions (Meson ≥ 1.3,
a C23 compiler). That path is not tested:

```sh
mkdir -p out && git rev-parse --short=12 HEAD > out/VERSION
meson setup out/native -Dbuildtype=debugoptimized -Db_ndebug=false -Dhardening=true
meson compile -C out/native && meson test -C out/native
```

### How it avoids crashing

These are checks the build enforces, not guidelines:

- Unsafe C library functions, blocking GTK calls and raw callback registration are
  *poisoned*: using them is a compile error, and the build proves each ban still bites.
- Every signal handler and main-loop callback is tied to an owner object, so no callback
  can fire into a destroyed window. A counter of live registrations must return to zero
  when the program exits.
- All filesystem access for a folder happens on a worker thread, so a hung network mount
  can't freeze the window. In the tests, any callback that holds the main loop for more
  than 250 ms fails the run and names itself.
- File operations never overwrite in place: they write a temp file, fsync it and rename
  it over the target. Creation and renames refuse to replace existing files. Recursive
  delete never follows symlinks out of the tree being deleted.
- Tests run under AddressSanitizer and UBSan with GLib warnings made fatal. The parsers are
  fuzzed, and GCC's static analyzer must report nothing.
