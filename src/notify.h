/* Desktop notifications, through the freedesktop Notifications service (xfce4-notifyd and
 * the like): for news a person may want while Quoin isn't in front. Fire and forget: nothing
 * waits on the service, and without one nothing happens. Each notification has a key; a
 * later one with the same key replaces it on screen instead of piling up. Main thread. */
#pragma once

typedef enum
{
  GBK_NOTIFY_LOW,       /* news: goes away by itself after a few seconds */
  GBK_NOTIFY_NORMAL,    /* something to see to: stays until dismissed */
} GbkNotifyKind;

/* `summary` and `body` are plain text (the body is escaped for servers that read markup). */
void gbk_notify (const char *key, GbkNotifyKind kind, const char *summary, const char *body);

/* At exit: let go of the bus (later notifications are dropped). */
void gbk_notify_shutdown (void);

/* Tests: notifications go to `fn` instead of the service (nullptr: back to the service). */
typedef void (*GbkNotifyFn) (const char *key, GbkNotifyKind kind, const char *summary,
                             const char *body);
void gbk_notify_set_for_test (GbkNotifyFn fn);

#define GBK_NOTIFY_LOW_MS 5000
