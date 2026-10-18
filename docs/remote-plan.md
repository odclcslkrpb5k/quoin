# Remote browsing: plan (approved 2026-10-17)

Goal: browse, open, edit and manage files on another machine over SSH, without mounting
anything, in the spirit of Konqueror's fish:// (KDE 3). It must survive the laptop sleeping:
a dead connection is a message in one window, never a frozen desktop, and never lost data.

Decided by the user: the remote end is Quoin itself; opening a remote file downloads a copy,
launches the app on it and uploads it back when saved (KIO style).

The draft is in git history (22dc43f). Three independent reviews attacked it: feasibility
against the code, failure modes and security, and usability. Section 12 lists what they
found and what changed.

## 1. Why not a mount

sshfs is a kernel (FUSE) mount. When the link dies, every process touching the mount point
blocks in an uninterruptible syscall. Putting the connection inside Quoin removes that
mechanism (Eliminate): nothing else can touch the remote files, and every remote request is
asynchronous and can fail. For the same reason Quoin never hands an `sftp://` URI to another
app or to GIO: gvfs would quietly mount it under /run/user/*/gvfs and bring the hang back.

## 2. Shape

    laptop                                               workstation
    quoin (GTK) ==socketpair== ssh ===== network ===== sshd -- quoin-serve
      reader + writer threads                               poll loop, no GLib main loop
      main thread only sees decoded messages                scanner threads (inotify)
                                                            fileops on worker threads

- **quoin-serve** is a second, small binary (measured: 68 KB stripped, ~90 KB with the
  protocol; links GLib/GIO and libc only, no GTK). Built and released next to `quoin`.
- **Pushed, not installed** (fish style). The remote command is
  `.cache/quoin/serve/<build hash>/quoin-serve` (sshd starts commands in $HOME). If it isn't
  there, Quoin installs it with a separate ssh run of `exec sh` whose stdin is a fixed
  script of ours with the binary embedded; the script checks its SHA-256, then renames it
  into place. So the laptop always talks to exactly its own build: no PATH problem (Ubuntu
  puts ~/.local/bin on PATH only for login shells), no version skew, no protocol
  compatibility code. The serve binary deletes other builds' directories older than 30 days.
- **Remote commands use a fixed alphabet** (`[A-Za-z0-9._/-]` and spaces): the remote login
  shell (bash, fish, csh...) parses them identically and no filename byte ever reaches it.
  Anything variable (a path for F4) is passed hex-encoded as an argument and decoded by
  quoin-serve (Eliminate).
- **Connections are per process** (Quoin is one process per window launch), shared by the
  windows, tabs and jobs of that process. Uses the system `ssh` (Substitute: no crypto or
  auth of ours; host aliases, agent, known_hosts and ProxyJump work as in a terminal).

## 3. Locations: making a remote/local mix-up impossible to compile

Both machines have the same home layout, so a remote path that leaks into local code is a
valid local path: deleting `/home/gbk/x` "on the workstation" would delete the laptop's.

- `GbkLoc` (opaque, immutable, refcounted): machine (nullptr = local) + absolute path. Parent,
  basename, child, equal, is-under and display (lossy UTF-8) are GbkLoc operations, so UI
  code never holds a raw path string.
- `GbkItem`, `GbkJob` (sources, destination, created names), the browser, history, path
  bar, view, clipboard, drag-and-drop and Properties carry GbkLoc.
- Local file code (scanner, fileops, launch, actions, terminal) keeps `const char *`. The only
  bridge is `[[nodiscard]] bool gbk_loc_as_local (const GbkLoc *, const char **out)`: every
  caller must write the remote branch, and there is no fatal accessor to crash on.
- Text that leaves Quoin names remote items as `host:/path` (Copy Full Path, the text drag
  target), which pastes straight into scp or rsync and can't be mistaken for a local path.
- The terminal template's `%d` is always a local folder.

## 4. Protocol

- Frames: `u32 length | u8 type | u64 channel | payload`, little-endian. Frame cap 1 MiB,
  per-field caps (names 255 bytes, xattr values chunked). Malformed input closes the
  connection. A snapshot chunk is split to fit (8192 entries can exceed 1 MiB).
- **Handshake**: the server's first bytes are a magic line, then its build hash, protocol
  hash, `/etc/machine-id`, uid, and $HOME. Anything before the magic (a .bashrc that prints)
  fails with a message saying exactly that.
- **Identity is the machine-id**, not the alias: two aliases for one machine share one
  connection, and `ssh localhost` is recognised as this machine. That is what stops a
  "cross-host" move between two names of one machine from deleting its own result.
- **Channels** carry each directory watch, job and transfer. Ids come from a 64-bit counter
  and are never reused; every job remembers its connection's generation, so an answer
  clicked after a reconnect can never reach a different job.
- **Flow control**: separate reader and writer threads on both ends (readers never write),
  per-channel credit windows, data in 64 KiB frames interleaved across channels. When a
  watch's backlog passes a cap the server drops its deltas and sends a fresh snapshot (the
  scanner's existing overflow-rescan behaviour).
- **Directory watches** send raw fields (name, kind, flags, size, mtime, mode, free space).
  The laptop decoder rejects empty names, `.`, `..`, names with `/`, duplicate names in a
  snapshot, and invalid names in removals, then builds GbkEntry with its own type table, on
  the connection thread (so the main thread's drain stays under the stall watch).
- **Jobs** run on the server with `gbk_ops_run`; its hooks (conflict, error, untrashable,
  progress) become frames and the answers come back. The laptop's GbkJob and job window
  don't know the difference.
- **Fuzzing**: one target decodes frames into a dir model and checks the model's
  invariants, beyond just decoding. The remote is untrusted input.

## 5. Connection, sleep and failure

- ssh argv (no shell), with options the user's config can't override:
  `ssh -T -e none -o BatchMode=yes -o ControlMaster=no -o ControlPath=none
  -o ClearAllForwardings=yes -o ForwardAgent=no -o ForwardX11=no -o RemoteCommand=none
  -o ConnectTimeout=10 -o StrictHostKeyChecking=yes -o ServerAliveInterval=10 -- HOST CMD`.
  HOST must not start with `-` or contain whitespace or control bytes; the GbkLoc parser
  refuses it, so an option can never be injected through a crafted address.
- stdio is a **socketpair**, written with `send(MSG_NOSIGNAL)`: ssh dying can't SIGPIPE the
  GUI. (Ignoring SIGPIPE process-wide is not an option: it would be inherited by every app we
  launch.) The server ignores SIGPIPE and treats EPIPE as EOF. ssh's stderr is drained
  continuously into a small ring buffer, used for error messages.
- **App-level pings** both ways every 10 s. The laptop declares the link dead after 30 s of
  silence, whatever ssh thinks. The server cancels everything and exits after 30 s of
  silence, with a watchdog thread that removes temp files and `_exit`s even if a write is
  blocked. A half-open TCP connection (laptop asleep, wifi changed) therefore never leaves an
  orphan server holding inotify instances on the workstation.
- **Sleep and network**: logind's PrepareForSleep and NetworkManager's connectivity signal
  (async D-Bus through a new owner-scoped `gbk_dbus_subscribe` in lifetime.c). Going to
  sleep: connections are closed cleanly. On resume: nothing is attempted until the network
  is up.
- **Disconnected** is a new non-final state, distinct from ERROR and GONE (which today end the
  source, and GONE makes the tab walk up towards `/`). The folder keeps its last listing,
  dimmed, "Reconnecting…". Allowed: selecting, scrolling, Copy Full Path, Back/Forward, local
  tabs. Refused with a status-bar reason: entering folders, opening, any change. Reconnect
  happens on network-up, focus, navigation or Reload, with backoff and no dialogs. The new
  snapshot goes into the same model, so selection and scroll position survive.
- **Auth**: on "permission denied" or a host key problem, the message has a button **Log in
  in a terminal**: it runs `ssh -o AddKeysToAgent=yes HOST true` in the configured terminal
  (the user's own config, interactive), and the connection is retried when that closes. A
  locked agent after resume or an unaccepted host key gets the same.
- **Jobs that lose the link** stop on both ends (temp files removed; atomic replace means no
  half-written file). The job window stays up after wake and lists which items finished.
  Changing requests are never queued or retried by themselves. Re-running is safe: finished
  copies come back as conflicts, finished moves as "already moved".
- Idle for 5 minutes: the connection closes.

## 6. Interface

- Addresses: `host:/path` and `host:path` (relative to the remote home), scp's rule: a colon
  before the first `/`. `./name:x` is a local name. Accepted in the location entry and on the
  command line (main parses it before GTK). No `sftp://` form.
- Path bar: a root button with the host name; "home" is the remote $HOME.
- The ☰ menu gets **Network** next to Devices: non-wildcard Host entries from ~/.ssh/config
  plus recently used hosts.
- Status bar shows remote free space, as for local folders. Remote times are shown as the
  remote reports them (no clock-skew correction).
- F4 on a remote folder: the configured TerminalRun runs
  `ssh -t -- HOST .cache/quoin/serve/<hash>/quoin-serve --shell-at <hex path>`.
- Disabled for remote items until the phase that implements them, each with a test that it
  is disabled: Cut/Copy/Paste, drag out, drop in, Open, Execute, custom actions, Properties'
  Apply, and every file operation.
- Remote items are never offered to other apps (drag, clipboard) in v1, apart from the
  `host:/path` text.

## 7. Opening remote files

Phase 5, read-only:
1. One cancellable job downloads all selected files into
   `~/.cache/quoin/remote/<random, 0700>/` with a manifest (machine-id, uid, remote path,
   base token, state). The server checks the file didn't change during the read.
2. The app is launched on the copy, as for a local file. The copy is read-only.

Phase 6 adds editing:
3. The remote path is resolved through symlinks at download; the upload goes to the target,
   keeping the remote file's own mode and xattrs, not the cache copy's.
4. A watch on the copy notices saves (close_write, or a rename over it), waits until the
   size/mtime/ctime stop changing (debounce), then uploads.
5. The upload is one server-side **compare-and-replace**: replace only if the remote file
   still matches the base token (ino, size, mtime_ns, ctime_ns, from the remote's own stat,
   never the laptop's clock). The server returns the new token after each upload, so the next
   save doesn't conflict with our own last one. Mismatch: ask (Replace / Keep both / Don't
   upload). A file with nlink > 1: ask, since replacing breaks the hard link.
6. Offline: the upload waits for reconnect; the manifest says "dirty". On reconnect the
   machine-id and uid must match the manifest, so a different machine answering on a hotel
   network never receives the upload.
7. **Nothing is lost by closing Quoin.** While any copy is being watched, the process stays
   alive after its last window closes, and closing always shows a notice (not optional, no
   "don't show again") naming each file and the app it was opened in: "notes.txt (Mousepad)
   from ws1: Quoin keeps running to upload your saves". Each live watcher holds a
   lock on its cache directory. Cleanup deletes only unlocked directories marked clean and
   older than 7 days. At startup Quoin lists dirty ones: "N edited remote files were never
   uploaded", with Upload / Show / Discard.

## 8. File operations and transfers

- **Within one machine** (remote, or two aliases of the same machine-id): the job runs
  there. Nothing crosses the network.
- **Between laptop and remote**: fileops gets a **source reader** interface (stat, list,
  read, readlink, xattrs) that `copy_into` calls; the destination side drives, so conflicts,
  skips and keep-both prune the walk naturally. A local reader is the existing syscalls; a
  remote reader is pipelined requests (lists and stats fetched ahead). This is a real
  refactor of the most safety-critical file and is budgeted as such.
- Names from a remote reader pass `gbk_ops_valid_name`, and every create under the
  destination uses `openat2` with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, so a hostile stream
  can't write outside it (the classic "symlink, then file through it" attack).
- Across machines, owner, group, `system.posix_acl_*` and `security.*` are never copied
  (numeric ids mean other people elsewhere); the new file belongs to the receiving user, as
  with scp.
- **A cross-machine move deletes a source item only after** the destination has fsynced the
  file and its folder and acknowledged it, and only if the source still matches the token
  taken when reading began (otherwise: a conflict, not a deletion of the newer version).
- Remote A to remote B: later.

## 9. Testing

- `QUOIN_SSH` replaces the ssh command; tests point it at a script running quoin-serve
  locally, so the whole stack runs with no network. quoin-serve becomes a test dependency.
- Fault injection: SIGSTOP the server (sleep), kill it (dead link), freeze the pipe in one
  direction (half-open), junk before the magic (noisy .bashrc), wrong hash, missing binary
  (push path), a server sending `..`, duplicates and oversized frames.
- One real end-to-end test against sshd on localhost in the build container: openssh from
  the same snapshot, its own host keys and `-F` config, and a passwd entry for the
  container uid (ssh finds ~/.ssh through getpwuid, not $HOME).
- Fuzz target: frames into a dir model, with invariant checks.

## 10. Phases

Each phase ships on its own and passes `./build check`.

0. **Fixes needed today, independent of remote** (found by the review):
   - A move between filesystems deletes the source after a copy that was never fsynced
     (fileops.c:315 syncs only when replacing): a crash can lose the file. Sync file and
     folder before deleting the source.
   - Entry validation accepts `.` and `..` (entry.c:7). The local scanner drops them first,
     so it's not reachable today, but nothing enforces that. Reject them in the validator.
1. **GbkLoc**: GbkLoc, GbkItem and GbkJob on GbkLoc, `gbk_loc_as_local`, text exports.
   No behaviour change. About 300 path references in window, browser, pathbar and view.
2. **Protocol and quoin-serve**: codec, frame-to-model fuzz target, serve binary (poll loop,
   watches, info), `--shell-at`, release asset and installer.
3. **Connection**: ssh spawning, socketpair, threads, handshake, push-install, pings, sleep
   and network signals, reconnect, Log in in a terminal, fake-ssh harness with faults, the
   real-sshd test.
4. **Browsing** (read-only): addresses, path bar, Network menu, disconnected state, F4,
   Properties (read-only), Copy Full Path. **First usable release.**
5. **Open read-only and copy remote → laptop**; the fileops source-reader refactor.
6. **Edit with upload, copy laptop → remote, cross-machine moves.**
7. **Operations on the remote**: rename, create, trash, delete, permissions, copy/move within.
8. **Drag and drop** (soon after v1, user's request): remote ↔ local drops inside Quoin,
   between windows of Quoin, and remote items dragged to other apps as a downloaded local
   copy (Konqueror's behaviour), never as sftp://.
9. Later: custom actions run on the remote (argv hex-encoded, no shell), remote A → remote B.

Rough size: 5–6k new lines, the most of it in phases 3, 5 and 6. Biggest risk: the fileops
refactor in phase 5.

## 11. Decisions (user, 2026-10-17)

1. quoin-serve is pushed automatically.
2. "Log in in a terminal" button for keys and host keys.
3. Sleep kills: a long job running entirely on the workstation is cancelled when the laptop
   sleeps or the link dies, and the job window says so. No detached jobs planned.
4. Quoin keeps running windowless while remote edits are open; the notice on close is
   mandatory and names each file and its app.
5. Phase order as in section 10.
6. No drag-and-drop or paste of remote items to other apps in v1 (`host:/path` text only);
   drag and drop is phase 8, soon after.

## 12. Red team: findings and dispositions

Blockers in the draft, all changed above:
- Remote command not on PATH (non-interactive ssh skips ~/.profile) → pushed binary (§2).
- Two aliases for one machine: a "cross-host" move deletes its own copy → machine-id
  identity, delete only on token match (§4, §8).
- Host name could inject ssh options (`-oProxyCommand=`) → `--` and parser refusal (§5).
- F4 built a remote shell command from the filename → hex argument to quoin-serve (§2, §6).
- No SIGPIPE handling (verified: nothing in src/): ssh dying would kill every window →
  socketpair + MSG_NOSIGNAL (§5).
- Remote names `.`/`..` pass entry validation (verified, entry.c:7): deleting a remote `..`
  would remove the grandparent → decoder rejection, phase 0 validator fix (§4, §10).
- Edits silently lost when Quoin exits or cleanup runs → stays alive, locks, manifest (§7).

Major, changed: server never sees EOF on a half-open link (app pings, watchdog); user ssh
config overriding our options (forced options); move deleting before the copy is durable,
including today's local move (fsync + ack, phase 0); stale answers after reconnect (64-bit
channels, generations); a hostile stream writing outside the destination (openat2); uid and
ACLs carried across machines (dropped); upload races, torn saves, self-conflicts, symlinked
targets (compare-and-replace with server tokens, debounce, resolve at download); unbounded
queues and two-way deadlock (separate threads, credit, rescan cap); "the model handles
repeated snapshots" was only half true, and GONE walks the tab up (new DISCONNECTED state);
the fileops "split" is a real refactor (source reader, budgeted); a fatal local-path
accessor would be a crash (bool accessor); phase 1 missed GbkItem and GbkJob (added);
"read-only" phase 4 still leaked paths via copy, drag and open (explicit disabled list with
tests); the first release could not fetch anything (phases reordered); disconnected
behaviour unspecified (specified); BatchMode dead ends (terminal login); one connection per
window process, not per app (stated); sftp:// would bring gvfs mounts back (never handed
out); custom actions (phase 9).

Minor, changed: free space and mode in the protocol; chunks over the frame cap; entries
built off the main thread; async D-Bus wrapper; scp address rule and `./` escape; remote
$HOME for the path bar; sshd test needs a passwd entry and host keys; quoin-serve can't use
g_main_loop_run (poisoned), so it's a poll loop; dropped the sftp:// address form and the
SFTP fallback idea.

Rejected: Quoin's own ControlMaster (`ControlPath=$XDG_RUNTIME_DIR/quoin-%C`) to share one
login between window processes. A dead master is exactly the hang we are removing; with the
agent, a second login costs nothing visible.
