/* Process-wide state shared by every window: the type table, the config, and the
 * connections to other machines.
 * Built once at startup. Atomically refcounted (the type table is also used by scanner
 * threads, through its own reference). */
#pragma once

#include "config.h"
#include "remote.h"

typedef struct
{
  GbkTypeTable *types;
  GbkConfig *config;
  GPtrArray *config_errors;   /* char*; shown to the user once */
  GbkRemote *remote;          /* connections to other machines (also gbk_remote_default) */
} GbkContext;

/* Loads config from `config_path` (nullptr: default path) and the system type table. */
[[nodiscard]] GbkContext *gbk_context_new (const char *config_path);
GbkContext *gbk_context_ref (GbkContext *c);
void gbk_context_unref (GbkContext *c);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkContext, gbk_context_unref)
