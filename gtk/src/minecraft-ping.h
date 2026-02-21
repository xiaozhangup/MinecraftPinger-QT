#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _McPingResult {
    gchar *resolved_host;
    guint16 resolved_port;

    gchar *version_name;
    gint64 protocol;

    gchar *motd_plain;

    gint64 players_online;
    gint64 players_max;

    GBytes *favicon_png; // PNG bytes, or NULL
} McPingResult;

void mc_ping_result_free(McPingResult *r);

void mc_ping_async(const char *host_or_ip,
                   guint16 port,
                   GCancellable *cancellable,
                   int timeout_ms,
                   GAsyncReadyCallback callback,
                   gpointer user_data);

McPingResult *mc_ping_finish(GAsyncResult *res, GError **error);

G_END_DECLS
