/* A file operation running on a worker thread, observed from the main thread.
 *
 * The worker never touches GTK. It sends progress, questions (conflict / error /
 * untrashable) and completion through a queue + eventfd. When it asks a question it waits
 * until the main thread calls gbk_job_answer() or gbk_job_cancel(). A running job keeps
 * itself alive (closing the window that started it doesn't stop it). */
#pragma once

#include "fileops.h"
#include "loc.h"

typedef enum
{
  GBK_QUESTION_CONFLICT,     /* `path` (source) vs `other` (existing destination) */
  GBK_QUESTION_ERROR,        /* `other` is the error message; answer SKIP or CANCEL */
  GBK_QUESTION_UNTRASHABLE,  /* answer REPLACE to delete permanently, SKIP to keep */
  GBK_QUESTION_UPLOAD,       /* `path` (there) changed since it was opened, or
                        * (`other_is_dir`) has other hard links: REPLACE, KEEP_BOTH, SKIP */
} GbkQuestionKind;

typedef struct
{
  GbkQuestionKind kind;
  char *path;
  char *other;
  bool other_is_dir;
} GbkQuestion;

typedef enum
{
  GBK_JOB_COPY,
  GBK_JOB_MOVE,
  GBK_JOB_TRASH,
  GBK_JOB_DELETE,
  GBK_JOB_RENAME,
  GBK_JOB_CREATE_FILE,
  GBK_JOB_CREATE_FOLDER,
  GBK_JOB_SET_EXECUTABLE,
  GBK_JOB_CLEAR_EXECUTABLE,
  GBK_JOB_PERMISSIONS,
  GBK_JOB_INFO,
  GBK_JOB_FETCH,             /* files from another machine, to open (gbk_job_new_fetch) */
  GBK_JOB_UPLOAD,            /* an edited copy back to where it came from */
} GbkJobKind;

/* What a FETCH brought. */
typedef struct
{
  guint index;               /* which of the job's sources */
  char *name;                /* the copy's name: the file's, as it was asked for */
  char *remote;              /* where it came from there, links resolved */
  GbkFileToken token;        /* that file as it was read */
} GbkFetched;

void gbk_fetched_free (GbkFetched *f);

#define GBK_TYPE_JOB (gbk_job_get_type ())
G_DECLARE_FINAL_TYPE (GbkJob, gbk_job, GBK, JOB, GObject)

/* Signals:
 *   "progress"  ()
 *   "question"  (GbkQuestion *q)  answer with gbk_job_answer (now or later)
 *   "finished"  (GError *error)   nullptr on success (cancelled: G_IO_ERROR_CANCELLED) */

/* Locations are GbkLoc (arrays hold GbkLoc*, referenced). A job on a location that isn't on
 * this machine finishes at once with G_IO_ERROR_NOT_SUPPORTED (remote jobs: a later phase of
 * docs/remote-plan.md). `dest_dir` for COPY/MOVE. */
[[nodiscard]] GbkJob *gbk_job_new (GbkOpKind kind, const GPtrArray *sources, GbkLoc *dest_dir);
[[nodiscard]] GbkJob *gbk_job_new_rename (GbkLoc *loc, const char *new_name);
/* Create an empty file or folder `name` in `dir` (never replacing anything). */
[[nodiscard]] GbkJob *gbk_job_new_create (GbkLoc *dir, const char *name, bool folder);
/* Execute permission on/off for regular files (see gbk_ops_set_executable). */
[[nodiscard]] GbkJob *gbk_job_new_executable (const GPtrArray *locs, bool executable);
/* Set the rwx bits of `loc` (see gbk_ops_set_permissions). */
[[nodiscard]] GbkJob *gbk_job_new_permissions (GbkLoc *loc, uint32_t rwx);
/* Read `loc`'s details (gbk_ops_info); the result is gbk_job_info() once finished. */
[[nodiscard]] GbkJob *gbk_job_new_info (GbkLoc *loc);
/* Download `locs` (files on one other machine, each followed through links there) into
 * `dirs` here (one folder each, char*), as new files under their own names.
 * gbk_job_fetched() says what came, from where, as it was. */
[[nodiscard]] GbkJob *gbk_job_new_fetch (const GPtrArray *locs, const GPtrArray *dirs);
/* Save `local` (here) over `target` (another machine) if it is still as `base` says, else
 * ask (GBK_QUESTION_UPLOAD). gbk_job_created(): the name written there (none: not uploaded,
 * as answered); gbk_job_uploaded(): the new file's state. */
[[nodiscard]] GbkJob *gbk_job_new_upload (const char *local, const GbkLoc *target,
                                          const GbkFileToken *base);

void gbk_job_start (GbkJob *job);
void gbk_job_cancel (GbkJob *job);
void gbk_job_answer (GbkJob *job, GbkChoice choice, bool apply_all);

bool gbk_job_running (const GbkJob *job);
GbkJobKind gbk_job_kind (GbkJob *job);
const GbkLoc *gbk_job_dest (const GbkJob *job); /* where things are created, or nullptr */
size_t gbk_job_n_sources (GbkJob *job);
double gbk_job_fraction (const GbkJob *job);         /* 0..1, or -1 if unknown yet */
const char *gbk_job_current (GbkJob *job);     /* item being processed (display name) */
GPtrArray *gbk_job_created (const GbkJob *job);     /* names created in dest (after finishing) */
const GbkFileInfo *gbk_job_info (const GbkJob *job); /* INFO jobs, after success; else nullptr */
/* FETCH, once finished: GbkFetched*, those that came (others failed, and said so). */
const GPtrArray *gbk_job_fetched (const GbkJob *job);
/* UPLOAD, once finished: true with the new file's state if one was written. */
bool gbk_job_uploaded (const GbkJob *job, GbkFileToken *token);

/* Jobs still running (tests wait for 0). */
int gbk_job_live (void);
