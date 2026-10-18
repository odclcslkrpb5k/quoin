# claude-notes

NAME: **Quoin** (user, 2026-10-14; was gbk-file). Sister project gbk-markdown -> "Galley".
Binary `quoin`, quoin.desktop, app id io.github.odclcslkrpb5k.Quoin (= icon name, like
Galley's io.github.odclcslkrpb5k.Galley), config ~/.config/quoin/quoin.conf
(old gbk-file/gbk-file.conf moved across once by gbk_config_path_in), repo
github.com/odclcslkrpb5k/quoin, installer env QUOIN_*; install.sh removes an old gbk-file
install under the same prefix and moves the inode/directory default (default prefix only).
Code prefix gbk_/Gbk stays. ICON (2026-10-14): data/icons/io.github.odclcslkrpb5k.Quoin.svg,
Galley's sibling (48 grid, tile #e9eff7 + navy #1f3b63 border, mid #3f6aa6): folder +
wedge. Installed to hicolor/scalable/apps; release asset; window default icon name. Local checkout dir is still ~/Projects/gbk-file.

Durable context for Claude. Read this first every session. Keep it current.

## Ground rules (from user, 2026-10-10)
- Filesystem writes: ONLY inside this repo. Nothing in ~/.claude memory, /tmp, etc.
- Durable context lives here (docs/claude-notes.md), not in Claude's memory system.
- Claude handles git. Push to main whenever appropriate. No branches/PRs (solo project).
- No CI. Builds are entirely local.
- Versions are tied to git hashes (no semver tags).

## Project
gbk-file: from-scratch Thunar clone for the user's own daily use. Linux / X11 / GTK / Xfce.
User has run Thunar ~10 years with an unchanged config and uses few of its features.

Primary goal: maximum performance in every scenario + minimal memory footprint.

## Decided
- C + GTK3 (decided 2026-10-10).
- Quality bar: "no crashes, ever". Use the Hierarchy of Defect Controls ladder
  (hierarchy-of-defect-controls plugin); every safety measure must be Engineer rung or
  better (Eliminate > Substitute > Engineer). Name the rung when proposing a control.
- Event-driven fs change updates (files created by other apps appear without refresh).
- No thumbnails, ever.
- File type by filename extension only. No content sniffing.
  No extension -> default "unknown" type. All of these configurable later:
  - dotfiles (.bashrc) = no extension -> unknown
  - longest glob match (.tar.gz beats .gz), from shared-mime-info globs2
  - case-insensitive
  - no-extension + exec bit -> "executable" type (mode bits free from statx)
- Double-click executable: runs it. Script (by extension) -> runs in configured terminal so
  stdout/stderr visible. Extensionless executables / AppImage -> run directly, no terminal.
  Right-click menu: "Execute" and "Execute in Terminal" to override per-invocation.
- Non-executable script: double-click -> default app. Right-click still offers "Execute" and
  "Execute in Terminal" (run via per-extension interpreter map in config, e.g. sh->sh,
  py->python3; never chmod the file).
- Config: two terminal entries. Terminal= (F4, open in dir) and TerminalRun= (run a script,
  e.g. xfce4-terminal --hold --working-directory=%d -x %f).
- Sort by modified: newest first, folders still first.
- Delete/trash/paste-conflict behavior: follow Thunar (Shift+Del confirms; untrashable ->
  offer permanent delete; paste conflicts -> replace/skip/rename/apply-to-all dialog).
- Config format: GLib GKeyFile (.desktop-style). Decided 2026-10-10.
- Clipboard/edit keys: Ctrl+C/X/V, Delete -> trash (freedesktop spec), Shift+Delete -> permanent. F2 rename.
- VIEW: Thunar "Compact view" at lowest zoom ONLY. No other view modes, ever.
  (names flow top->bottom in columns, columns left->right, horizontal scroll)
- Sort default: name, case-insensitive, folders first. Also sort by last-modified,
  switchable from the empty-space right-click menu.
- Selection: click, Shift+click, Ctrl+click, arrows, Shift+arrows, Ctrl+arrows (move focus
  without selecting), Space (toggle focused). NO rubber-band/drag-select.
- Type-ahead: typing jumps to first match. Used frequently.

## Open questions

## Feature inventory (user, 2026-10-10; "not exhaustive, muscle memory")
- F4: open terminal in cwd. Terminal command configurable (user uses xfce4-terminal).
- Ctrl+H: toggle hidden files.
- Alt+Left / Alt+Right: history back/forward. Alt+Up: parent dir.
- Arrow keys: move selection.
- Ctrl+C "and friends": copy/cut/paste etc. (clipboard must interop with other apps:
  x-special/gnome-copied-files + text/uri-list).
- Double-click: execute, or open with default handler (mimeapps.list via GAppInfo).
- Right-click item: action menu. Right-click empty space = menu for cwd as a folder.
- Configurable filetype-based menu actions (e.g. archive extraction). Uses this a lot.
- Plus from screenshot: tabs, button path bar w/ edit-location toggle, status bar, hamburger menu.
- Command execution rule (Eliminate shell injection): never pass through a shell; parse the
  template to argv once, substitute placeholders per-argument.

## Architecture notes (proposed, not final)
- Bypass GIO for listing: getdents64 + statx on worker thread, compact arrays.
- Custom virtualized view widget (cairo/Pango), not GtkTreeView/GtkIconView.
- Raw inotify fd on GLib main loop (g_unix_fd_add), batch events per frame; not GFileMonitor.
- Icons: per-type, rendered once per size, cached.
- Precomputed natural/casefold sort keys.
- Type table built from /usr/share/mime/globs2 at startup.

## User's current Thunar setup (docs/thunar.png)
- No menubar (hamburger button instead), no toolbar, no side pane.
- Path bar as buttons (home/gbk > Projects > gbk-file), pencil = edit location, computer icon at right.
- Tabs.
- Status bar: "1 folder | Free space: 40.9 GiB".
- Retro-ish theme (salmon/tan, bold monospace-ish labels) — theme fidelity likely matters.

## Build environment (decided 2026-10-10)
- Everything (build, tests, analyzers, fuzzers) runs in a Docker image defined in-repo.
  Workstation installs must not matter. Host has Docker 29.8 (rootless). Host = Ubuntu 26.04.1.
- Base image ubuntu:26.04 pinned by digest so the binary links against the same
  glibc/GTK as the host. Rebuild image when host distro upgrades.
- Build system: meson + ninja.
- User is not a C programmer; relies on me for C good practice. Correctness/safety > convenience.

## Local toolchain (2026-10-10, host — only for reference now)
gcc 15.2 (default -std=gnu23, has -fanalyzer, -fhardened, -fzero-init-padding-bits),
glibc 2.43, GTK 3.24.52, GLib 2.88, meson + ninja.
NOT installed: clang, clang-tidy, valgrind, cppcheck, AFL++. (User must install; I can't write outside repo.)

## GTK3 status
3.24.52 (Mar 2026); upstream moved to one release/year, crash/important fixes only.
Stable target, no new features coming. Fine for us.

## Safety plan (proposed 2026-10-10, awaiting user OK)
Eliminate:
- No thumbnails/sniffing -> no decoder code of ours. No plugins of our own. (GTK itself still
  dlopens theme engines/IM modules. -z nodlopen is in the OpenSSF set but ld emits no NOOPEN
  flag for our PIE executable — effectively a no-op; harmless.)
- File entries live in per-directory-load storage, referenced by (generation,index) handles,
  never raw pointers held across main-loop iterations -> stale-entry UAF can't exist.
- GTK touched only from main thread; workers send self-contained messages (owned copies).
- Filenames are opaque bytes; display strings made once via g_filename_display_name.
Substitute:
- gnu23: nullptr, constexpr, [[nodiscard]], {} init, <stdckdint.h> ckd_* for size math.
- GLib containers/strings; g_autofree/g_autoptr/g_steal_pointer/g_clear_*; G_DECLARE_FINAL_TYPE.
- GSlice is just malloc since GLib 2.76: use g_new/g_free, not g_slice.
Engineer:
- -Werror always (solo, never distributed). Big warning set incl. -Wconversion,
  -Wjump-misses-init (catches goto past g_auto vars), -Wshadow, -Wvla, -Wnull-dereference,
  -Wswitch-enum, -Wmissing-prototypes, -Wstrict-prototypes, -Wcast-qual, -Wformat=2.
- poison.h: #pragma GCC poison strcpy strcat sprintf vsprintf strncpy gets strtok atoi...
- Build variants: debug(ASan+UBSan, no FORTIFY), analyze(-fanalyzer, must be clean),
  release(OpenSSF 2026-08-20 TL;DR set + -fhardened-ish, FORTIFY=3, auto-var-init=zero).
- Tests run with G_DEBUG=fatal-warnings,fatal-criticals: any g_return_if_fail = failure.
- Stress tests: scanner vs. concurrent create/delete/rename storms; inotify overflow (IN_Q_OVERFLOW).
- Signal/source lifetime (user said close this gap, 2026-10-10):
  Eliminate: all signal connections + idle/timeout sources go through our wrapper module
  (owner-scoped; auto-disconnect/remove when owner OR instance is disposed, via weak refs).
  Engineer: lint script (runs in docker check stage) fails if raw g_signal_connect*,
  g_idle_add*, g_timeout_add*, g_source_attach appear outside the wrapper file.
  Engineer: debug builds keep a live-registration counter; tests assert zero at teardown
  and that callbacks never fire after owner dispose.
- NO git hooks / commit gating (user, 2026-10-10): repo contents don't matter, the BUILT
  binary does. Gate lives in the release build instead: it runs tests + analyzer + sanitizer
  suite and refuses to emit a binary unless all pass. Release builds refuse a dirty tree, so
  the embedded git hash fully identifies the source.
Protect (deliberate, flagged):
- -ftrivial-auto-var-init=zero, FORTIFY, stack protector: absorb residual bugs in release.
- Unavoidable exits: OOM (g_malloc aborts), X server connection loss. Not our code.

## Current state of the code (update as it changes)
`./build {debug|analyze|fuzz [s]|release|shell|image}`. Release = debug tests + analyzer +
20s fuzz per target + hardened build + tests. Tree must be clean for release.
- out/ is build output (NOT build/: the ./build script occupies that name).
- Docker here is NOT rootless: container runs with --user $(id -u):$(id -g), HOME=/tmp.
  If out/ ever ends up root-owned, delete it via the container (rm as root inside).
- meson: get_option() can't be used inside project(); option named 'version' is rejected
  -> ours is 'build_id'. Toolchain-pinned flags are required, not probed. GCC-only warnings
  are gated on cc.get_id() (clang is used only for libFuzzer builds).
- src/prelude.h is force-included (-include) in every TU: all system headers, then
  #pragma GCC poison bans. New system header? add it to prelude ABOVE the poison block.
  Macros must be #undef'd before poisoning. meson.build has configure-time probes: each
  ban must fail to compile (and a baseline must compile). Add a probe with every new ban.
- g_error is banned (its for(;;) trips -fanalyzer). Use gbk_fatal() (src/fatal.h).
- Never write "type/*" (slash-star) in comments: -Wcomment error.
- g_assert_* in tests can return (non-fatal mode): don't deref after g_assert_nonnull;
  use if (!x) gbk_fatal(...). The analyzer catches this.
- memcmp/memcpy with an empty GArray: data may be nullptr -> UB even with len 0. Guard.
- pthread_cancel/pthread_exit/g_thread_exit are banned. -fexceptions is on for every build
  EXCEPT analyze (with it, -fanalyzer reports leaks on impossible unwind paths).
- LSan: tests/lsan.supp suppresses ONLY libfontconfig (its cache is never freed). Never add
  broad suppressions (libgtk, libpango): they'd hide our own GObject leaks.
- Screenshots: GUI tests write PNGs to out/artifacts/. The Read tool shows them downscaled —
  zoom/crop with PIL before judging colours (I once "fixed" white text that was fine).

Modules (src/, all in libgbk-core except lifetime):
- lifetime: gbk_connect[_after], gbk_idle_add, gbk_timeout_add, gbk_fd_watch,
  gbk_source_cancel, gbk_lifetime_live. Only lifetime.c may use raw signal/source APIs.
  fd watches abort on G_IO_NVAL. GLib notifies weak refs in GObject's own dispose, so a
  subclass dispose that closes a watched fd must gbk_source_cancel the watch first.
- filetype: immutable atomically-refcounted ext->MIME table (GbkTypeId uint16, builtins
  0..7). gbk_type_table_new_system() reads XDG globs2 + generic-icons.
- natsort: sort keys (NFD + casefold; ASCII fast path) + natural compare (total preorder,
  fuzz-checked). Final tiebreak on raw name lives in dirmodel.
- entry: GbkEntry = one malloc (struct + name\0[display\0]key\0). statx flags
  AT_NO_AUTOMOUNT|AT_STATX_DONT_SYNC. Hidden = leading '.' or trailing '~'.
- inotify-parse: pure parser (fuzzed).
- scanner: detached worker thread per dir; owns ALL fs syscalls. Messages via GAsyncQueue +
  eventfd. CHUNK(first..last)=snapshot, DELTA, ERROR, GONE. Coalesce 10ms quiet/50ms max.
  >PENDING_MAX names or IN_Q_OVERFLOW -> rescan. Rescans must openat(dirfd,".") — NOT
  dup() (shares dir offset -> empty rescan; real bug, caught by storm test).
  Deleted/moved dir: while ANY fd holds it (ours always does) IN_DELETE_SELF is deferred,
  and plain rmdir sends NO self IN_ATTRIB (an earlier fix wrongly relied on that; a test
  helper's chmod masked it). So we also watch the PARENT for IN_DELETE|IN_MOVED_FROM of our
  basename, then confirm with still_here() (fstat nlink==0, or path no longer same inode).
  Test hooks: set_pending_max_for_test, rescans, live_workers.
  Free space: fstatvfs on the worker, carried in CHUNK(last)/DELTA messages.
- inotify-hub: ONE inotify instance per process (max_user_instances is 128/user, shared
  with the desktop). Hub thread reads it, routes by wd to GbkInotifySub buffers (mutex +
  eventfd, cap GBK_INOTIFY_MAX_PENDING -> synthetic IN_Q_OVERFLOW -> rescan). Same inode
  => same wd for several subs: add with IN_MASK_ADD (else the kernel REPLACES the mask),
  rm_watch only when the last sub leaves. Both properties mutation-tested (tests fail
  without them). Lock order: hub_lock -> sub->lock, never the reverse.
- dirmodel: GObject, signals changed/state-changed. Generational slot handles (GbkHandle:
  gen<<32|slot). Rescan = mark (epoch) and sweep at last chunk. Incremental re-sort for
  small batches, full sort otherwise; gbk_dir_model_check_order() is the oracle.
  Drains <=16 messages per wakeup then re-arms.
- command: argv templates via g_shell_parse_argv; %d %f %F %c %%; %F/%c whole-arg only;
  program literal; files always absolute (no "-rf" option injection). No shell, ever.
- config: GKeyFile at $XDG_CONFIG_HOME/gbk-file/gbk-file.conf. Strict all-or-nothing:
  any error -> defaults + all errors reported. Sections: General(Terminal, TerminalRun,
  ShowHidden, SortBy), Interpreters (ext=argv; replaces defaults; marks scripts),
  Types (ext=mime overrides, weight 1000), Action <id> (Name, Types, Directories, Command).
  docs/example.conf is loaded by a test (can't rot).

Tests: tests/test-<name>.c listed in tests/meson.build unit_tests. Fuzz targets
tests/fuzz/fuzz-<name>.c + seeds in tests/fuzz/seeds/<name>/ (replayed as tests in normal
builds; fuzz_targets list). A fuzz crash -> minimise into seeds as a regression test.
Tests run hermetic (XDG dirs point nowhere) with fatal warnings/criticals, ASan/UBSan.

Benchmarks (./build bench, release, warm cache, host fs, 2026-10-10):
  100k entries: first paint data 5.7ms, full listing 390ms, ~153 B/entry heap,
  full re-sort 66ms, live create latency ~11ms (10ms coalescing quiet period).
  Per entry: readdir 0.4us, statx 2.4us (kernel, dominant), build 0.35us, model 0.9us.
  Optimization candidate: parallel statx across a small thread pool (helps cold cache /
  network fs most). Loading re-sorts are throttled to every 100ms (order_stale).

GUI (libgbk-gui; tests under Xvfb started by in-container.sh, is_parallel:false):
- view: GbkView (css name "iconview", class "view"), GtkScrollable (hadjustment only).
  Columns flow top->bottom, left->right; per-column width = widest item, capped at
  max(240, 2/3 width), ellipsize middle. Text width: ASCII advance table + 3px slack,
  Pango only for non-ASCII; cached per slot+gen. Icons per type id via
  gtk_icon_theme_choose_icon_for_scale (specific, generic, text-x-generic).
  Selection = GHashTable of handles; pruned on model change. Signals: activate,
  selection-changed, context-menu(GdkEvent*), search-changed. Type-ahead: prefix match on
  sort keys, 1s timeout, shown in status bar.
- window: GbkWindow (GtkApplicationWindow). Actions win.back/forward/up/toggle-hidden/
  terminal/close; accels set in main.c. Up/back-to-parent focuses the child we came from.
  GONE / ENOENT / ENOTDIR -> walk up to nearest ancestor (async, one level per attempt).
- dirmodel announces state-changed only AFTER the order is updated (finish_batch).
- context: refcounted {types (system + config [Types]), config, config_errors}.
- launch (core): g_spawn_async argv, SEARCH_PATH, stdin /dev/null; GLib reaps children.
- open (core): GbkItem = owned snapshot of an entry (abs path, mime, dir/exec/script).
  gbk_open_plan: dir->ENTER, exec+script->RUN_TERMINAL, exec->RUN, else DEFAULT.
  Execute/Execute in Terminal for exec files and scripts (non-exec scripts via
  [Interpreters]). Default app via GIO, grouped per app (one launch for N files).
  Multi-selection activation skips folders until tabs exist.
- menu (core, pure): gbk_menu_entries(config, items, background). Item menu: Open, Open
  With (same-type files; window lists g_app_info_get_recommended_for_type), Execute(+Term)
  for one runnable file, Open Terminal Here for one folder, then actions matching ALL
  items. Background: Terminal Here, dir actions, Sort by Name/Date Modified, Show Hidden.
  Copy Full Path (user, 2026-10-11) on items (after Copy/Paste Into) and background (after
  Paste): plain text only, one path per line, NO trailing newline (terminal paste would run
  it); replaces our files clipboard. Window builds GtkMenu: items' data owned by menu (KEY_ITEMS snapshot), activations
  connected with window as owner; menu destroyed on idle after deactivate AND when the view
  is destroyed (GtkMenu is a toplevel: attach-widget destroy does NOT destroy it — leaked
  once, caught by the lifetime counter). Keyboard popups use gtk_get_current_event() as the
  trigger (GTK requires one).
- fileops (core, blocking, worker-only): see header for safety rules (temp+fsync+rename
  replace, O_EXCL/RENAME_NOREPLACE, dirfd/O_NOFOLLOW delete, into-itself guard, depth cap
  GBK_OPS_MAX_DEPTH, EXDEV move deletes source only after a complete copy, folder REPLACE
  = merge, paste where it came from = automatic "name (1).ext").
- job (core): GObject; worker thread + queue/eventfd; questions block the worker on a GCond
  until gbk_job_answer/cancel. Running job holds a self-ref (outlives windows).
- jobui (gui): progress window after 500ms, async question dialogs (Cancel/Skip/Keep
  Both/Replace|Merge + Apply to all; untrashable -> Delete Permanently), error dialog.
- clipboard (gui) + cliptext (core, fuzzed): x-special/gnome-copied-files, text/uri-list,
  text. Non-local file:// hosts rejected. Cut-paste clears the clipboard if ours.
- lifetime: gbk_clipboard_request(owner, cb, target, fn, data, destroy) — weak-ref owner.
  BANNED: gtk_clipboard_request_*/wait_*, gtk_dialog_run, gtk_main, g_main_loop_run.
- browser (gui): one tab = GbkBrowser (GtkBox): path, model, history, focus-on-load,
  pending select, transient message, status text. Signals path-changed/status-changed.
- pathbar (gui): toggle buttons from $HOME (else "File System" /), current pressed, deeper
  buttons kept after going up. ONE PATH BAR PER TAB, inside the browser's toolbar row
  (user, 2026-10-11: tabs ABOVE the path bar — deliberate break from Thunar, since the path
  bar's contents belong to the tab). The window puts its ☰ menu button at the start of each
  tab's toolbar row.
  Pencil / Ctrl+L -> GtkEntry (~ and relative paths), Enter navigates, Escape returns.
- window: GtkNotebook of browsers at the top (tab bar hidden with one tab),
  hamburger GMenu (new tab/window, show hidden, sort, terminal, location, close), status.
  show-hidden (bool) and sort ('name'|'modified') are stateful window-wide actions.
  Shortcut table lives in window.c: gbk_window_install_accels() — used by main AND tests
  (tests once silently had no shortcuts at all). Ctrl+T/W/N/Q/L, Ctrl+PgUp/PgDn, F2, F4,
  Ctrl+C/X/V, Delete, Shift+Delete, Ctrl+H, Alt+arrows.
  KEY ROUTING: while a GtkEditable has focus, keys go to it and window shortcuts DON'T run
  (otherwise Delete/Ctrl+V/F2 in the location bar act on files). Tested both ways.
  Several folders activated -> new tabs. Trash: no confirmation; Shift+Delete confirms.
- Mouse side buttons (user, 2026-10-12): buttons 8/9 = back/forward via two
  GtkGestureMultiPress on the window in CAPTURE phase (children like path bar buttons can't
  swallow them). GTK3 gestures aren't owned by the widget: unref'd in window dispose.
  Test sends real press/release to the view and to a path bar button (mutation-checked).
- Title bar = full path of the active folder, nothing else (user, 2026-10-11).
- Status bar (user mockup, 2026-10-12): left "4 folders · 6 files   13.1 GiB free",
  "name  28.6 KiB   … free", "3 selected  41.2 KiB   … free". Right: small buttons, never
  focusable: sort button "↑Name"/"↓Modified"/"↓Size", click cycles (table `sorts[]` in
  window.c is the single list: state string, label, tooltip); ".*" GtkToggleButton bound to
  win.show-hidden via GtkActionable (state syncs itself). win.sort ignores unknown values
  (actions are reachable over D-Bus).
- Sort by size: folders first BY NAME (dir size meaningless), files largest first, ties by
  name. Config SortBy=name|modified|size.
- Menus: gbk_menu_entries takes GbkMenuTarget (ITEMS/BACKGROUND/PATHBAR). Open in New
  Tab/Window when all items are folders (tab: switch to the first; window: one per folder).
  Path bar right-click (real button-press, tests synthesize one via
  gtk_button_get_event_window): Open, New Tab/Window, Terminal Here, Copy, Paste Into, Copy
  Full Path, actions — NO cut/trash/delete/rename (the tab is inside that folder). Menu is
  attached to the path bar (dies with it). on_menu_item holds its own ref on the items.
- User decisions 2026-10-11: no far-right terminal toolbar icon; hamburger menu contents OK
  for now; no shortcuts while the location entry has focus is correct; no middle-click
  features; hidden/sort window-wide is right.
- Synthetic key events in tests need hardware_keycode (GTK bindings use it); pick the
  keymap entry with level 0/group 0.
- Tests: test-open uses a private XDG env with a fake default app (.desktop + mimeapps.list)
  that touches marker files. Variadic nullptr terminators must be cast (C23 nullptr_t).
- main: GtkApplication NON_UNIQUE (process per invocation), argv parsed by us.

- Version: tools/in-container.sh writes $GBK_VERSION (git hash, "-dirty" suffix) to
  out/VERSION before every setup; meson.build reads it via project(version: files(...)),
  GBK_VERSION = meson.project_version(). (Was a separate build_id option -> meson summary
  printed "gbk-file undefined"; user spotted it.)
- Repo is PUBLIC (user, 2026-10-12): github.com/odclcslkrpb5k/quoin. This notes file is
  public too — keep it free of anything private.
- Distribution: prebuilt binaries (user's choice). install.sh (repo root) is THE installer:
  curl|bash downloads releases/latest (or GBK_VERSION) assets gbk-file, gbk-file.desktop.in,
  SHA256SUMS; checks Ubuntu 26.04 x86_64 (GBK_ANY_OS=1 overrides); ./build install runs the
  same script with GBK_FROM=out/dist/<hash>. ./build publish = gh release create <hash>
  --latest (needs HEAD on origin/main, refuses existing). Other distros: deferred by user.
- ./build install [prefix] (HOST side, default ~/.local — the one command writing outside
  the repo; only the USER runs it with the default prefix; I test with a prefix under out/).
  Refuses dirty tree; needs out/release/released == HEAD hash (written only after the full
  release gate passes; setup() deletes it on any rebuild, e.g. bench), else runs release.
  Checks binary --version == HEAD, then cp+rename (atomic) binary and data/gbk-file.desktop.in.
- View scroll position is CONTENT-anchored (anchor_index/offset/end), restored by every
  update_adjustment; only real scrolling (value-changed outside `adjusting`) records it.
  Why: GtkScrolledWindow allocates the child once WITHOUT the hscrollbar, then with it, and
  the transient layout (more rows, shorter content) clamped the pixel value -> couldn't
  reach the end (user bug 2026-10-12). Overlay scrolling OFF for the view's scroller (an
  overlay bar covered the bottom row). Width cache keyed by slot+gen is per MODEL: cleared
  in gbk_view_set_model (stale widths truncated "movies" to "m…s"). All mutation-checked.
- Alt+Home = home folder (win.home).
- Status bar right-click (user, 2026-10-12) = the active tab's background menu. Status bar
  is wrapped in a GtkEventBox (visible window, so tests can target its GdkWindow). Every
  context menu is now destroyed with its TAB too (KEY_BROWSER is borrowed; the status bar
  outlives tabs) — mutation-checked by closing the tab with the menu open.
- BUILT 2026-10-12 (user's choices: Apply button, no Ctrl+Shift+N, Executable on every
  regular file):
  * fileops: gbk_ops_create (mkdirat / openat O_EXCL|O_NOFOLLOW, never replaces),
    gbk_ops_set_executable (+x where r / clear all x; regular files only, follows links),
    gbk_ops_set_permissions (rwx only, keeps 07000), gbk_ops_info (statx incl. btime,
    symlink: target's details + link_target; dangling: the link's), gbk_ops_numbered_name.
  * job: GbkJobKind (COPY..INFO) separate from GbkOpKind; worker switch; info result via
    gbk_job_info. Executable job on several files asks Skip/Cancel per failure.
  * Background menu starts "Create Folder… | Create File…"; name suggestion from the MODEL
    (no syscalls on main thread), "(n)" if taken; shared name_dialog (rename/create).
  * props.c: GbkProps window (not an app window: no accels), info job on open and after
    Apply; perms editable only by owner/root; Escape/Close; destroy_with_parent.
    Alt+Enter = win.properties (selection of 1, or the folder). Properties also in item
    (single), background and path bar menus.
- DRAG AND DROP (user, 2026-10-13: "careful"; real failure mode = accidental moves into a
  subfolder). dnd.{h,c} shared helpers. OUT: uri-list + text, actions COPY|MOVE|LINK|ASK,
  NO drag_data_delete ever (never delete because a target asked). Press on a selected item
  defers select-only to release (multi-drag). IN (view + path bar buttons): flags 0 (all
  ours); drop target = folder item under pointer else view's folder; from same view: not
  into a dragged item, not onto own folder (status 0). drag_data_received: finish(success,
  del=FALSE) at once, then emit "drop"; window ALWAYS asks: Move Here / Copy Here / - /
  Cancel, popup_at_pointer with the current event (SelectionNotify) as trigger. Dropped
  back into their own folder: "Already in this folder", no menu. Edge autoscroll.
- COPY = cp -a (user): keep_owner (EPERM -> group only -> nothing), keep_xattrs (fd or l*
  variants; ACLs ride along), chmod AFTER chown, times incl. symlinks (AT_SYMLINK_NOFOLLOW),
  dirs' metadata after contents, hard links via Ctx.links "dev:ino"->first copy, mknod for
  FIFO/socket/dev. Sparse not preserved.
- TESTING DnD: in-GTK synthetic events can START a drag but never drive it (GTK grabs the
  pointer at the X server). Real test uses XTest (libxtst-dev in image, gui tests link
  x11+xtst): fake motion/buttons at root coords. Synthetic drag + gtk_drag_cancel leaves
  icon popups mapped forever -> don't. GTK keeps empty 1x1 popups at -99,-99 for DnD IPC:
  "no popups" checks must ignore childless popups. Xvfb has no WM: windows stack at 0,0.
- DEVICES (user, 2026-10-15: Thunar's sidebar Devices list, in the ☰ menu; user chose ONE
  "Devices" submenu, each device a submenu Open / Unmount / Eject|Safely Remove). devices.{h,c}
  (core, GIO only): GbkDevices per window wraps GVolumeMonitor (gvfs udisks2 if installed,
  else unix mount table); listing = drives' volumes, driveless volumes, volumeless unshadowed
  mounts; skipped if nothing can be done. "File System" (/) is ALWAYS first, Open only, even before the
  monitor starts (user 2026-10-15: "No devices" looked wrong; Thunar lists it). No placeholder. Menu ids are a never-reused counter; unknown ids do
  nothing (actions reachable over D-Bus). Labels double "_" (mnemonics). Started
  GBK_WINDOW_DEVICES_DELAY_MS (300) after the window opens: g_volume_monitor_get costs ~20 ms
  of sync D-Bus with gvfs. "pre-unmount"(root) signal (ours AND gvfs mount-pre-unmount): window
  moves tabs under root to dirname(root). Unmount/eject then WAIT until
  gbk_scanner_busy_under(roots) is false (scanner keeps a live-worker registry; up to
  GBK_DEVICES_RELEASE_MS) — our own dirfd would make it EBUSY. Mutation-checked with
  gbk_scanner_set_release_delay_for_test (without the delay the race always won and the
  mutation survived). Async ops hold a GWeakRef on the window. FAILED_HANDLED/CANCELLED
  errors are not shown (GtkMountOperation already did). gtk_mount_operation_new does a SYNC
  session-bus call and g_criticals without a bus -> the container now runs a private
  dbus-daemon (Dockerfile dbus-daemon; in-container.sh start_x). tests/test-devices.c fakes
  GVolumeMonitor/GVolume/GMount (folders as mounts). Fake fail code 0 == G_IO_ERROR_FAILED:
  0 means "succeed" there (bit me once).
- PARANOID C (2026-10-16/17): the user (via the paranoid-typescript session) asked to adopt the
  standard's draft C/GTK appendix and report back. Full record: docs/paranoid-c-adoption.md.
  What now exists: ./build check (compose.yaml; release = check + stamp); meson.build warning
  flags each with a fixture (a flag that does nothing fails configure); prelude refuses
  G_DISABLE_*; tools/check-binary.sh (readelf + DW_AT_producer per unit); src/analyzer-glib.h
  (analyzer build only: GLib allocators declared to -fanalyzer; leak warnings off, GCC 15.2
  FP with g_autofree); tools/analysis.py = clang-tidy + cppcheck vs tools/analysis-baseline.txt
  (compared to origin/main's copy, which ./build writes; may only shrink; 10 permanent
  entries); tools/check-source.py rules (nodiscard on GError ** functions, unsequenced &x/x
  args, NULL GError with discarded result, no suppressions/(void) casts except VOID_CASTS,
  no paths in src logs, no ->message comparisons, no hidden chars), self-test first;
  stall watch (lifetime.c, GBK_STALL_MS=250 in tests; registration functions are macros
  passing #callback); clang-format (.clang-format, whitespace commit in
  .git-blame-ignore-revs); Dockerfile packages from snapshot.ubuntu.com (SNAPSHOT arg;
  move the date to take updates). The peer standard wants no CI; the user's rule wins.
- MY RECURRING SLIP: filtering build output (grep) hid failures/compile errors twice.
  Always: set -o pipefail; ./build X > out/build.log; echo exit; grep error: too.
  Third time (2026-10-16): a background `./build check > log; echo exit $?` always "exits
  0", I read the notification, not the echoed line, and pushed a commit whose tests failed
  (69eded6). Fix: run `./build check > log 2>&1` as the background command ITSELF, nothing
  after it, so the task's exit code is the check's.
- Not built yet (proposed 2026-10-11): New Folder/New File, rubber-band selection, drag and
  drop (ask user if they use it), remember window size, parallel statx.
- REMOTE BROWSING (2026-10-17): plan in docs/remote-plan.md, red-teamed, APPROVED by
  the user with the decisions in its section 11. Execute phase by phase, in order.
  Done: phase 0 (18aa15c), phase 1 GbkLoc (src/loc.{h,c}): every UI path is a GbkLoc; file
  code gets paths only via gbk_loc_as_local (remote -> NOT_SUPPORTED / EREMOTE, tested with
  gbk_loc_new_remote_for_test). check-source rule `locpath` limits gbk_loc_path() callers.
  Phase 2: src/proto.{h,c} (codec, fuzzed: fuzz-proto), src/serve.c = quoin-serve (poll
  loop, non-blocking stdio, scanner watches, info on threads, --shell-at HEX). serve_lib =
  the GTK-free part of core (filetype natsort entry inotify scanner fileops proto);
  check-binary.sh allowlists quoin-serve's NEEDED libs. Shipped in releases + install.sh
  next to quoin. Fuzzing found a real OOB read (type byte before length check) in 20 s.
  Phase 3: src/conn.{h,c} GbkConn (I/O thread, socketpair + MSG_NOSIGNAL, forced ssh
  options, push-install via `exec sh` script, magic/HELLO/build check, ping/dead timers),
  src/remote.{h,c} GbkRemote registry (one conn per host, PrepareForSleep closes all,
  NM StateChanged -> "network-up", idle close), lifetime gbk_dbus_signal (raw
  g_dbus_connection_signal_subscribe poisoned elsewhere). Tests: tests/fake-ssh (modes via
  a file), test-conn, test-remote (GTestDBus), test-sshd (real sshd as non-root +
  nss_wrapper; image has openssh-client/server, libnss-wrapper).
  Phase 4: GbkAddress (loc.h, scp rule) + gbk_browser_open_address (pending until the
  remote home is known); dirmodel remote source (gbk_remote_default set by GbkContext),
  GBK_DIR_DISCONNECTED (keeps entries, retry backoff 1..30 s, network-up, window
  focus-in, info bar Retry / Log in in a terminal); gbk_menu_works_remote = the one list of
  what works remotely (menu + shortcuts + drag/drop); pathbar host button; props via
  gbk_conn_info; F4 remote = TerminalRun ssh -t ... --shell-at HEX; Network ☰ submenu
  (~/.ssh/config Host names + used hosts). GtkMountOperation parent-ref leak worked around
  in window.c device_action (found because the registry made the leaked context visible).
  Phase 5: protocol LIST/LSTAT/READ(+CREDIT/CANCEL)/LISTING/STATED/DATA/END; serve.c
  general worker Tasks (credit cond var for READ); fileops GbkSource (local: fds +
  copy_file_range; remote in conn.c via worker-thread Calls routed by channel in the I/O
  thread, dead connection fails every Call); gbk_ops_run_from; GbkStatEntry lives in
  fileops.h; jobs COPY remote->local; clipboard x-special/quoin-locations (cliptext,
  fuzzed); fetch.c = Open/Open With remote files (cache, read-only, 7-day cleanup).
  Found: window teardown crash (two tabs, selection, first tab current) - fixed, tested.
  check-source `widening` rule after the 6th implicit-widening finding.
  Phase 6a: src/peer.{h,c} (serve_lib) = both directions: GbkOutbox (frames from any
  thread; replaced conn's out buffer and serve's done queue), GbkAsker (calls + a copy's
  source + gbk_asker_run_job), GbkAnswerer (LIST/LSTAT/READ/REMOVE/GET_INFO tasks in
  SESSIONS; laptop's is restricted: only its own sessions' roots, openat2
  RESOLVE_BENEATH|NO_SYMLINKS; tests/test-peer.c), GbkJobRunner (server runs JOBs with
  gbk_ops_run_from, QUESTION/ANSWER/PROGRESS/CREATED/DONE). Server channels have bit 63.
  Moves delete originals via GbkSourceOps.remove = REMOVE in a recording session:
  gbk_ops_remove_unchanged keeps anything not as FIRST listed. job.c plan_transfer picks
  here / read-from-there / run-there. Cut/Paste/Paste Into now work remotely; drops onto
  remote stay refused (phase 8) in ask_drop + view.
- ANALYZER + REFCOUNTS: -fanalyzer can't follow refcounts: a task/job referenced by a
  table AND its worker gets "use after free" on error paths. Give such things ONE owner
  (the worker) and let tables borrow them under a lock (peer.c tasks and jobs).
- SCRIPTED EDITS: Python str.replace replaces EVERY match (hit a second poll loop in
  conn.c, 2026-10-18; UBSan caught the OOB). Use a match-exactly-once helper (out/edit.py,
  `once(path, old, new)`: recreate it if out/ was cleaned) or the Edit tool.
- UI rule (user, 2026-10-18): an action says what it will do before you click it. The Open
  entry names the application ("Open With Mousepad") whenever one app will be used.
- CHAIN with && : a check run after a failing edit script tests unchanged code (did it
  2026-10-17). Edits then `./build format && ./build check`. RECURRED 2026-10-18 (the
  rule was written down and still skipped), so now Engineer: out/edit.py leaves
  out/EDIT-FAILED on a failed edit and ./build refuses to run while it exists.
- FORMAT before CHECK: `./build format` rewrites C with clang-format; run it before every
  `./build check` (check refuses unformatted C; I tripped it twice in one session). Phase 0 there
  (fsync before deleting a moved source; reject "."/".." in entry validation) is a real
  defect fix independent of remote work.
