#include "context.h"

static void
context_free (GbkContext *c)
{
  if (gbk_remote_default () == c->remote)
    gbk_remote_set_default (nullptr);
  g_object_unref (c->remote);
  gbk_type_table_unref (c->types);
  gbk_config_free (c->config);
  g_ptr_array_unref (c->config_errors);
}

GbkContext *
gbk_context_new (const char *config_path)
{
  GbkContext *c = g_atomic_rc_box_new0 (GbkContext);
  c->config_errors = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *default_path = gbk_config_default_path ();
  c->config = gbk_config_load_file (config_path != nullptr ? config_path : default_path,
                                    c->config_errors);

  /* System database, then the config's [Types] overrides on top (they always win). */
  GbkTypeBuilder *b = gbk_type_builder_new_system ();
  gbk_config_apply_types (c->config, b);
  c->types = gbk_type_builder_finish (b);
  c->remote = gbk_remote_new (c->types);
  gbk_remote_set_default (c->remote);
  return c;
}

GbkContext *
gbk_context_ref (GbkContext *c)
{
  return g_atomic_rc_box_acquire (c);
}

void
gbk_context_unref (GbkContext *c)
{
  g_atomic_rc_box_release_full (c, (GDestroyNotify) context_free);
}
