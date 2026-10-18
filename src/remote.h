/* This process's connections to other machines, one per host, and what makes them go
 * stale (docs/remote-plan.md, section 5):
 *
 * - Sleep. logind's PrepareForSleep(true): every connection is closed before the machine
 *   sleeps, cleanly, so each remote end stops at once instead of waiting out the silence.
 *   Nothing reconnects on resume by itself: the network usually isn't back yet.
 * - Network. NetworkManager reaching full connectivity emits "network-up": what was waiting
 *   for a connection can try again now.
 * - Disuse. A connection without a watch or request for GBK_REMOTE_IDLE_MS is closed.
 *
 * Without a system bus (or without logind or NetworkManager) the monitors hear nothing,
 * and connections still notice a dead link by their own silence timer. Main thread. */
#pragma once
#include "conn.h"

#define GBK_TYPE_REMOTE (gbk_remote_get_type ())
G_DECLARE_FINAL_TYPE (GbkRemote, gbk_remote, GBK, REMOTE, GObject)

/* Signals:
 *   "network-up"   the network (re)gained full connectivity */

/* Listings are typed with `types`. Starts the monitors. */
[[nodiscard]] GbkRemote *gbk_remote_new (GbkTypeTable *types);
/* The connection to `host`, connecting or ready, made if there is none (or only a dead
 * one). New reference. Fails only for a host gbk_host_valid() refuses. */
[[nodiscard]] GbkConn *gbk_remote_conn (GbkRemote *r, const char *host, GError **error);
/* Hosts this process has connected to (char*, sorted). */
[[nodiscard]] GPtrArray *gbk_remote_hosts (GbkRemote *r);
/* The machine names in an ssh config file (char*, in order, once each): every name on a
 * Host line that isn't a pattern (*, ?, !) and that gbk_host_valid() takes. Pure; Include
 * and Match lines are not followed. */
[[nodiscard]] GPtrArray *gbk_ssh_config_hosts (const char *text, size_t len);

/* The connection to `host` if there is one (any state), without making one. Borrowed. */
GbkConn *gbk_remote_peek (GbkRemote *r, const char *host);
/* Close every connection (also what happens before sleep). */
void gbk_remote_close_all (GbkRemote *r);

/* The process's registry, which folder models on other machines use (nullptr: none, and
 * such folders can't be read). Set once by whoever owns it (the GUI context); referenced. */
void gbk_remote_set_default (GbkRemote *r);
GbkRemote *gbk_remote_default (void);

#define GBK_REMOTE_IDLE_MS (5 * 60 * 1000)

/* Tests: follow the session bus instead of the system bus (a private one with a stand-in
 * logind and NetworkManager), and check for idle connections every `ms` with this limit. */
void gbk_remote_use_session_bus_for_test (bool session);
void gbk_remote_set_idle_for_test (int check_ms, int idle_ms);
