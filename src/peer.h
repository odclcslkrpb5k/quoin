/* Requests either machine makes of the other (docs/remote-plan.md, sections 4 and 8), shared
 * by Quoin (conn.c) and quoin-serve (serve.c). Both ends ask and both answer: the laptop
 * asks the server for its files when copying from there, and a job running on the server
 * asks the laptop for the laptop's files when copying to there.
 *
 * GbkOutbox: frames waiting for the other machine, added from any thread; the connection's
 * thread sends them.
 *
 * GbkAsker: requests made from worker threads, each waiting for its own answers, which the
 * connection's thread routes to it by channel. A copy's source (GbkSource) and running a
 * whole job on the other machine are built on it.
 *
 * GbkAnswerer: LIST, LSTAT, READ and REMOVE (and GET_INFO on the server), each on a thread
 * of its own, answering when ready. They come in sessions. On the laptop the answerer is
 * RESTRICTED: the other machine may only ask inside sessions the laptop opened itself for
 * its own jobs, and only about the files given to that job and what is inside them. Paths
 * are resolved with openat2 RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, so no request can climb
 * out through "..", or through a symlink, to read anything else on the laptop.
 *
 * GbkJobRunner (the server): a JOB is gbk_ops_run_from on a thread of its own; its
 * questions and progress go to the laptop, and its sources are read from the laptop when
 * they are there. */
#pragma once
#include "proto.h"

/* A channel number never used before in this process; the server's have
 * GBK_PROTO_SERVER_CHANNEL set. Any thread. */
uint64_t gbk_peer_channel (bool server);

/* ---- the outbox ---- */

typedef struct GbkOutbox GbkOutbox;

[[nodiscard]] GbkOutbox *gbk_outbox_new (void);
GbkOutbox *gbk_outbox_ref (GbkOutbox *o);
void gbk_outbox_unref (GbkOutbox *o);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkOutbox, gbk_outbox_unref)
/* Add frames (any thread) and wake the sender. */
void gbk_outbox_put (GbkOutbox *o, const GByteArray *frames);
/* The waiting bytes, locked: write some, remove what was written, then end. Writers in
 * between are held up, so keep it short. gbk_proto_put_* may append directly too. */
GByteArray *gbk_outbox_begin (GbkOutbox *o);
void gbk_outbox_end (GbkOutbox *o);
size_t gbk_outbox_len (GbkOutbox *o);
/* Readable when something was added: poll it, then gbk_outbox_woken. */
int gbk_outbox_wake_fd (const GbkOutbox *o);
void gbk_outbox_woken (const GbkOutbox *o);

/* ---- asking ---- */

typedef struct GbkAsker GbkAsker;

[[nodiscard]] GbkAsker *gbk_asker_new (bool server, GbkOutbox *out);
GbkAsker *gbk_asker_ref (GbkAsker *a);
void gbk_asker_unref (GbkAsker *a);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkAsker, gbk_asker_unref)
/* On the connection's thread: if `f` is on one of our requests' channels, it goes to that
 * request, and *valid says whether it is an answer at all. False: not ours. */
bool gbk_asker_route (GbkAsker *a, const GbkFrame *f, bool *valid);
/* Is `type` an answer to a request (LISTING ... DONE)? Answers to requests that have ended
 * arrive late and are dropped. */
bool gbk_asker_answer_type (GbkMsgType type);
/* The connection ended: everything waiting fails with `error`, and so does everything later. */
void gbk_asker_dead (GbkAsker *a, const GError *error);
/* Since when (g_get_monotonic_time) nothing has used it (at first: since it was made); 0
 * while a source or a job uses it. */
int64_t gbk_asker_unused_since (GbkAsker *a);

/* A copy's source reading the other machine's files, in a session of its own (`record`:
 * for a move, so the other end can tell later what changed since it was listed). */
[[nodiscard]] GbkSource *gbk_asker_source (GbkAsker *a, bool record);
/* The same in `session`, which the other machine opened (the server reading the laptop). */
[[nodiscard]] GbkSource *gbk_asker_source_in (GbkAsker *a, uint64_t session);

/* Run `r` on the other machine, from a worker thread, which it blocks: questions go to
 * `hooks` and their answers back, progress to hooks->progress, and the names made in the
 * destination into `created` (may be nullptr). Cancelling (hooks->cancelled) stops it there
 * too. UPLOAD: *uploaded (may be nullptr) gets the new file's state, when one was written. */
[[nodiscard]] bool gbk_asker_run_job (GbkAsker *a, const GbkJobRequest *r, const GPtrArray *sources,
                                      const GbkOpsHooks *hooks, GPtrArray *created,
                                      GbkFileToken *uploaded, GError **error);

/* ---- answering ---- */

typedef struct GbkAnswerer GbkAnswerer;

[[nodiscard]] GbkAnswerer *gbk_answerer_new (bool restricted, GbkOutbox *out);
GbkAnswerer *gbk_answerer_ref (GbkAnswerer *a);
void gbk_answerer_unref (GbkAnswerer *a);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkAnswerer, gbk_answerer_unref)
/* Is `type` a request for it (LIST LSTAT READ CREDIT CANCEL REMOVE SESSION FORGET
 * GET_INFO)? CANCEL may also be for a job (GbkJobRunner): both take it. */
bool gbk_answerer_takes (GbkMsgType type);
/* Start answering one (on the connection's thread). False: malformed, or a request this
 * end never takes from the other (a restricted one: SESSION, FORGET, GET_INFO); the
 * connection should end. A request it may not answer (outside its session) fails. */
[[nodiscard]] bool gbk_answerer_handle (GbkAnswerer *a, const GbkFrame *f);
/* Restricted: open a session for a job of ours that lets the other machine read `roots`
 * (absolute paths, char*) and what's inside them; `record` for a move (REMOVE). */
uint64_t gbk_answerer_open (GbkAnswerer *a, const GPtrArray *roots, bool record);
void gbk_answerer_close (GbkAnswerer *a, uint64_t session);
/* End everything in progress (reads stop) and refuse whatever comes later. */
void gbk_answerer_stop (GbkAnswerer *a);
/* Tests: requests being answered. */
int gbk_answerer_busy (GbkAnswerer *a);
/* Tests: each READ's chunk waits this long first (0: none), so a copy is still running
 * when something happens to it. */
void gbk_answerer_set_read_delay_for_test (int ms);

/* ---- running jobs (the server) ---- */

typedef struct GbkJobRunner GbkJobRunner;

/* Sources on the other machine are read through `asker`. */
[[nodiscard]] GbkJobRunner *gbk_job_runner_new (GbkAsker *asker, GbkOutbox *out);
/* SOURCES JOB ANSWER CANCEL */
bool gbk_job_runner_takes (GbkMsgType type);
[[nodiscard]] bool gbk_job_runner_handle (GbkJobRunner *r, const GbkFrame *f);
/* Cancel every job and wait up to `ms` for them to finish (removing their temp files). */
void gbk_job_runner_stop (GbkJobRunner *r, int ms);
void gbk_job_runner_unref (GbkJobRunner *r);
