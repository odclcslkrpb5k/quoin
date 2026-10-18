/* A connection to quoin-serve on another machine, through the system ssh
 * (docs/remote-plan.md, section 5). Main thread API; the I/O runs on a thread of its own.
 *
 * Lifecycle: CONNECTING -> READY -> DEAD, or CONNECTING -> DEAD. DEAD is final: a new
 * connection is a new GbkConn. Each one is one ssh
 * process; nothing outside this process can block on it, and nothing here blocks on it:
 * every request is asynchronous and can fail.
 *
 * Connecting: ssh runs the remote command `.cache/quoin/serve/<build>/quoin-serve` (sshd
 * starts commands in the home folder). If it isn't there, or is another build, the
 * quoin-serve next to this program is sent first (a second ssh run of `exec sh`, fed a
 * fixed script holding the compressed binary and its SHA-256), then the connection is
 * tried once more. So this program always talks to exactly its own build. Remote commands
 * are a fixed alphabet; no path or name is ever part of one.
 *
 * Liveness: a ping every PING_MS; nothing received for DEAD_MS and the connection is DEAD,
 * whatever ssh or TCP think (a laptop that slept leaves a half-open connection). The
 * server stops by itself after the same silence (serve.c).
 *
 * Environment: QUOIN_SSH replaces the ssh command (tests: a fake that runs quoin-serve
 * locally); QUOIN_SERVE names the quoin-serve to send (default: next to this program). */
#pragma once
#include "loc.h"
#include "peer.h"

typedef enum
{
  GBK_CONN_CONNECTING,
  GBK_CONN_READY,
  GBK_CONN_DEAD,
} GbkConnState;

#define GBK_TYPE_CONN (gbk_conn_get_type ())
G_DECLARE_FINAL_TYPE (GbkConn, gbk_conn, GBK, CONN, GObject)

/* Signals:
 *   "state-changed"   gbk_conn_state() changed */

/* Start connecting to `host` (an ssh destination, as typed to ssh). Listings are typed with
 * `types` (referenced). Fails at once (G_IO_ERROR_INVALID_ARGUMENT) for a host
 * gbk_host_valid() refuses. */
[[nodiscard]] GbkConn *gbk_conn_new (const char *host, GbkTypeTable *types, GError **error);
const char *gbk_conn_host (const GbkConn *c);
GbkConnState gbk_conn_state (const GbkConn *c);
/* READY or DEAD after READY: who answered. nullptr before. */
const GbkHello *gbk_conn_hello (const GbkConn *c);
/* READY: the other end is this very machine (same machine-id, e.g. ssh localhost). */
bool gbk_conn_is_this_machine (const GbkConn *c);
/* DEAD: why, in one line for the user. */
const char *gbk_conn_error (const GbkConn *c);
/* DEAD: logging in once in a terminal may fix it (a key or a host key needs a person). */
bool gbk_conn_needs_login (const GbkConn *c);
/* End it now (goes DEAD). The remote end stops when its input closes. */
void gbk_conn_close (GbkConn *c);
/* Since when (g_get_monotonic_time) no watch or request has been open; 0 while one is. */
int64_t gbk_conn_unused_since (const GbkConn *c);

/* A folder watch: `fn` gets every GbkScanMsg (owned) for the folder, on the main thread,
 * while `owner` is alive; then, if the connection dies first, one call with nullptr. */
typedef void (*GbkConnScanFn) (GbkScanMsg *msg, void *data);
typedef struct GbkConnWatch GbkConnWatch;
[[nodiscard]] GbkConnWatch *gbk_conn_watch (GbkConn *c, const char *path, GObject *owner,
                                            GbkConnScanFn fn, void *data);
/* Stop it and free it (also after the connection died). `fn` isn't called again. */
void gbk_conn_unwatch (GbkConnWatch *w);

/* A file's details: `fn` is called exactly once, on the main thread, if `owner` is still
 * alive then: with the details, or with nullptr and why (a lost connection included). */
typedef void (*GbkConnInfoFn) (GbkFileInfo *info, const GError *error, void *data);
void gbk_conn_info (GbkConn *c, const char *path, GObject *owner, GbkConnInfoFn fn, void *data);

/* For a terminal (interactive: the user's own ssh config and prompts apply): a login shell
 * in `path` on `host`, through the installed quoin-serve (the path travels hex-encoded, so
 * no name reaches a shell parser). nullptr for an invalid host. g_strfreev. */
[[nodiscard]] char **gbk_conn_shell_argv (const char *host, const char *path);
/* For a terminal: log in once and do nothing, so a person can unlock a key (it's then
 * added to the agent) or accept a host key. nullptr for an invalid host. g_strfreev. */
[[nodiscard]] char **gbk_conn_login_argv (const char *host);

/* A source for copies (fileops.h) reading from that machine through the connection, for
 * a job's worker thread: it blocks there, never here. Free it on any thread. A connection
 * that dies fails every read in progress at once. `record`: for a move (the source can
 * remove what it copied, if unchanged). */
[[nodiscard]] GbkSource *gbk_conn_source (GbkConn *c, bool record);
/* For jobs that run there (peer.h): asking it to run them, and answering what they ask
 * about this machine's files (only within the sessions such a job opens). Referenced;
 * usable from any thread, and after the connection is gone (everything then fails). */
[[nodiscard]] GbkAsker *gbk_conn_asker (GbkConn *c);
[[nodiscard]] GbkAnswerer *gbk_conn_answerer (GbkConn *c);

/* This machine's /etc/machine-id (hex), or "" without one. g_free. */
[[nodiscard]] char *gbk_conn_local_machine_id (void);

/* Tests: shorter timers for connections made after this (0: the default). */
void gbk_conn_set_timeouts_for_test (int ping_ms, int dead_ms, int connect_ms);
/* Tests: connections (I/O threads) not yet finished. */
int gbk_conn_live (void);

#define GBK_CONN_PING_MS 10000
#define GBK_CONN_DEAD_MS 30000
#define GBK_CONN_CONNECT_MS 30000        /* ssh's own ConnectTimeout is 10 s of this */
