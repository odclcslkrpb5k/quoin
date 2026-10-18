# Remote browsing: plan (DRAFT, before red team)

Goal: browse, open and manage files on another machine over SSH without mounting anything,
in the spirit of Konqueror's fish:// (KDE 3). It must survive the laptop sleeping: a dead
connection is an error message in one window, never a frozen desktop.

Decisions already made by the user:
- The remote end is Quoin itself: `ssh host quoin --serve`.
- Opening a remote file in a local app: download a copy, launch the app on it, upload it
  back when the app saves it (as KIO did).

## 1. Why not a mount

sshfs is a kernel (FUSE) mount. When the link dies, every process that touches the mount
point blocks in an uninterruptible syscall: shells completing paths, file dialogs, us.
Moving the connection into the application removes that failure mechanism (Eliminate):
nothing outside Quoin can touch the remote files, and inside Quoin every remote request is
asynchronous and can fail.

## 2. Shape

    laptop                                            workstation
    Quoin (GTK) --pipe-- ssh ==== network ==== sshd -- quoin --serve (no GTK)
      connection thread                                  scanner threads (inotify)
      decodes frames, never blocks the UI                fileops on worker threads

- The remote `quoin --serve` runs the same scanner and file operation code that a local
  window does, against its own disks. Live updates come from the remote's inotify.
  Copy/move/delete within the remote never cross the network.
- One SSH connection per host, shared by every window, tab and job using that host.
  Uses the system `ssh` binary (Substitute: no crypto or auth code of ours; honours
  ~/.ssh/config, agent, known_hosts, ProxyJump, ControlMaster).
- `--serve` is decided in main() before any GTK call; it needs no display or D-Bus.

## 3. Locations: the one dangerous change

Today a location is a `char *` absolute path, and file operations act on whatever path they
get. Both machines have the same home layout, so a remote path that leaks into local code
is a valid LOCAL path: "delete /home/gbk/x" on the workstation would delete the laptop's
/home/gbk/x. That mistake must be impossible to compile (Engineer):

- New type `GbkLoc` (opaque, immutable, refcounted): host (nullptr = local) + absolute path.
- Every UI-level module (browser, window, pathbar, history, view, clipboard, dnd, props,
  jobs) carries `GbkLoc *`, not `char *`.
- Local file code (scanner, fileops, open/launch) keeps `const char *` paths. The only way
  from a `GbkLoc` to a local path is `gbk_loc_local_path(loc)`, which is fatal for a remote
  location. check-source.py gets a rule: no other way to get at a GbkLoc's path string for
  file access.
- A "backend" seam: the dir model, job and props ask the location for its backend (local or
  remote connection), never call scanner/fileops directly.

This refactor ships alone, with no behaviour change, before anything remote exists.

## 4. Protocol

Frames over the ssh pipe: `u32 length | u8 type | u32 channel | payload`. Length capped
(1 MiB); anything malformed closes the connection. Little-endian, versioned.

- Handshake: server writes a magic line first ("QUOIN-SERVE <protocol> <build hash>").
  Anything before it (a noisy .bashrc) or a different protocol number fails the connect
  with a message saying exactly that. Protocol number separate from the git hash; mismatch
  refuses with "laptop speaks N, workstation speaks M: update the older one".
- Channels multiplex: each directory watch, job and transfer is a channel. Large transfers
  are cut into 64 KiB data frames and interleaved, so a big copy can't starve a listing.
- Directory watch: the server sends the same messages a local scanner does (snapshot
  chunks, deltas, error, gone), carrying raw fields (name, kind, flags, size, mtime, mode).
  The LAPTOP builds each GbkEntry with gbk_entry_new() and its own type table, so the
  remote never decides types, and every name passes the existing validation.
- Jobs: the server runs gbk_ops_run(); its hooks (conflict, error, untrashable, progress)
  become frames, and answers come back. The local GbkJob/jobui doesn't know the difference.
- The decoder is pure and gets a fuzz target (the remote is untrusted input: a broken or
  compromised workstation must not be able to crash the laptop).

## 5. Connection lifetime and sleep

- GbkConn per host: spawns `ssh -o BatchMode=yes -o ServerAliveInterval=10
  -o ServerAliveCountMax=3 host -- quoin --serve`, owns a reader/writer thread, posts
  messages to the main thread through a queue + eventfd (as the scanner does).
- Auth: keys/agent only (BatchMode). A failed connect shows ssh's own stderr line
  ("Permission denied (publickey)", "Host key verification failed") plus a hint.
- Dead link detected by keepalive within ~30 s, or immediately when logind's PrepareForSleep
  says we're going to sleep or have resumed: we drop the connection on resume rather than
  waiting for TCP to notice.
- While disconnected a remote folder keeps its last listing, greyed, "Reconnecting…". It
  reconnects on demand (window focus, navigation, a Reload) with backoff, and a fresh
  snapshot replaces the old listing (the model already handles repeated snapshots).
- Read-only requests (list, info) retry after reconnect. Changing requests never retry by
  themselves: a job whose connection dies ends "connection lost, may be partly done";
  the server cancels it on EOF exactly like a local cancel (temp files removed, atomic
  replace means no half-written file). Re-running it is safe: finished items come back as
  ordinary conflicts.
- No users for 5 minutes: the connection closes.

## 6. User interface

- Address forms: `host:/path`, `host:path` (relative to remote home), `sftp://host/path`.
  The host is an ssh alias as you'd type to `ssh`.
- Path bar: a root button with the host name, then the folders.
- Status bar / window title show the host.
- F4 (Open Terminal Here) on a remote folder: the terminal runs
  `ssh -t host 'cd <path> && exec $SHELL -l'`.
- Execute / Run in Terminal / custom actions: disabled for remote items in v1.
- Sidebar/menu: a "Network" list of recently used hosts.

## 7. Opening remote files (KIO-style)

1. Download to `~/.cache/quoin/remote/<random dir, 0700>/<basename>` (a cancellable job with
   progress; a big file takes a while, there's no way round it without a mount).
2. Launch the default (or chosen) app on the local copy, as for a local file.
3. Watch the copy (inotify on the directory: close_write, and rename-over for editors that
   save via a temp file). On change: upload. Before replacing, check the remote file still
   has the size/mtime we downloaded; if not, ask (Replace / Keep both / Don't upload). The
   remote writes with the existing atomic-replace path.
4. Upload failure (offline): keep the copy, retry on reconnect, and tell the user where the
   copy is.
5. Watches end when Quoin exits; cache dirs older than 7 days are removed at startup.

## 8. File operations

- Within one remote host: the job runs on the server (fast, nothing crosses the network).
- Local -> remote and remote -> local: a stream of items (folder, file data, symlink,
  metadata) produced by a walk on one side and applied on the other by the existing
  destination logic (O_EXCL, temp + rename, cp -a metadata). Conflicts ask through the
  same questions. A cross-host move is copy-then-delete per item, as a cross-device move is
  today.
- Remote A -> remote B: through the laptop. Not in v1.
- Clipboard and drag-and-drop carry remote items as `sftp://host/path` URIs, which other
  file managers with gvfs also understand.

## 9. Testing

- `QUOIN_SSH` overrides the ssh command; tests point it at a script that runs
  `quoin --serve` locally, so the whole stack runs with no network.
- Fault injection: SIGSTOP the server (= sleep), kill it (= dead link), feed garbage
  before the magic (= noisy .bashrc), wrong protocol number.
- One real end-to-end test against sshd on localhost inside the build container.
- Fuzz target for the frame decoder.

## 10. Phases (each ships on its own)

1. GbkLoc + backend seam. No behaviour change.
2. Protocol codec + fuzz target; `--serve` mode.
3. Connection manager: spawn, handshake, keepalive, sleep/resume, reconnect; fake-ssh test
   harness with fault injection.
4. Read-only browsing: location entry, path bar, live updates, Properties, F4.
   First usable release.
5. Open remote files (section 7).
6. File operations on the remote host.
7. Transfers between laptop and remote; clipboard and drag-and-drop.

## 11. Open questions

- Remote install path: is `quoin` on PATH for a non-interactive ssh command?
- Should a long job that runs entirely on the workstation survive the laptop sleeping?
