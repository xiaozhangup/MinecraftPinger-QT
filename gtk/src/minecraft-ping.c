#include "minecraft-ping.h"

#include <json-glib/json-glib.h>

#include <string.h>

#define MC_DEFAULT_PORT 25565
#define MC_TIMEOUT_FALLBACK_MS 5000

static void gbytes_unref0(GBytes *b) {
    if (b) {
        g_bytes_unref(b);
    }
}

void mc_ping_result_free(McPingResult *r) {
    if (!r) {
        return;
    }
    g_free(r->resolved_host);
    g_free(r->version_name);
    g_free(r->motd_plain);
    gbytes_unref0(r->favicon_png);
    g_free(r);
}

// --- VarInt helpers (Minecraft protocol) ---

static gboolean write_varint(GByteArray *out, guint32 value) {
    while (TRUE) {
        guint8 temp = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            temp |= 0x80;
        }
        g_byte_array_append(out, &temp, 1);
        if (value == 0) {
            return TRUE;
        }
    }
}

static gboolean read_varint(const guint8 *buf, gsize len, gsize *io_off, guint32 *out_value) {
    guint32 num_read = 0;
    guint32 result = 0;

    while (TRUE) {
        if (*io_off >= len) {
            return FALSE;
        }
        guint8 byte = buf[(*io_off)++];
        result |= (guint32)(byte & 0x7F) << (7 * num_read);

        num_read++;
        if (num_read > 5) {
            return FALSE;
        }

        if ((byte & 0x80) == 0) {
            *out_value = result;
            return TRUE;
        }
    }
}

// --- DNS SRV resolve (best-effort) ---

typedef struct {
    gchar *host;
    guint16 port;
} SrvTarget;

static void srv_target_clear(SrvTarget *t) {
    g_clear_pointer(&t->host, g_free);
    t->port = 0;
}

static SrvTarget resolve_srv_best_effort(const char *host) {
    SrvTarget t = {0};

    if (!host || !*host) {
        return t;
    }

    // If already an IP literal, SRV doesn't apply.
    if (g_hostname_is_ip_address(host)) {
        return t;
    }

    GResolver *resolver = g_resolver_get_default();
    GError *error = NULL;
    GList *records = g_resolver_lookup_service(resolver,
                                              "minecraft",
                                              "tcp",
                                              host,
                                              NULL,
                                              &error);

    if (error) {
        g_clear_error(&error);
        if (records) {
            g_resolver_free_targets(records);
        }
        return t;
    }

    if (records) {
        // Pick first target (resolver usually sorts by priority/weight).
        GSrvTarget *rt = records->data;
        const gchar *h = g_srv_target_get_hostname(rt);
        const guint16 p = g_srv_target_get_port(rt);
        if (h && *h && p > 0) {
            t.host = g_strdup(h);
            t.port = p;
        }
        g_resolver_free_targets(records);
    }

    g_object_unref(resolver);

    return t;
}

// --- JSON parsing helpers ---

static gchar *extract_text_component(JsonNode *node) {
    if (!node) {
        return g_strdup("");
    }

    if (JSON_NODE_HOLDS_VALUE(node)) {
        if (json_node_get_value_type(node) == G_TYPE_STRING) {
            return g_strdup(json_node_get_string(node));
        }
        return g_strdup("");
    }

    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        return g_strdup("");
    }

    JsonObject *obj = json_node_get_object(node);
    if (json_object_has_member(obj, "text")) {
        const char *text = json_object_get_string_member(obj, "text");
        return g_strdup(text ? text : "");
    }

    // Some servers put legacy string in "extra" array.
    if (json_object_has_member(obj, "extra")) {
        JsonNode *extra = json_object_get_member(obj, "extra");
        if (extra && JSON_NODE_HOLDS_ARRAY(extra)) {
            JsonArray *arr = json_node_get_array(extra);
            GString *out = g_string_new(NULL);
            const guint n = json_array_get_length(arr);
            for (guint i = 0; i < n; i++) {
                JsonNode *child = json_array_get_element(arr, i);
                gchar *piece = extract_text_component(child);
                g_string_append(out, piece);
                g_free(piece);
            }
            return g_string_free(out, FALSE);
        }
    }

    return g_strdup("");
}

static gboolean parse_status_json(const guint8 *json_bytes,
                                 gsize json_len,
                                 McPingResult **out_result,
                                 GError **error) {
    g_autofree gchar *json_text = g_strndup((const char *)json_bytes, (gssize)json_len);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, error)) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid JSON root");
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *obj = json_node_get_object(root);

    McPingResult *r = g_new0(McPingResult, 1);
    r->protocol = -1;
    r->players_online = -1;
    r->players_max = -1;

    if (json_object_has_member(obj, "version")) {
        JsonObject *ver = json_object_get_object_member(obj, "version");
        if (ver) {
            if (json_object_has_member(ver, "name")) {
                r->version_name = g_strdup(json_object_get_string_member(ver, "name"));
            }
            if (json_object_has_member(ver, "protocol")) {
                r->protocol = json_object_get_int_member(ver, "protocol");
            }
        }
    }

    if (json_object_has_member(obj, "players")) {
        JsonObject *pl = json_object_get_object_member(obj, "players");
        if (pl) {
            if (json_object_has_member(pl, "online")) {
                r->players_online = json_object_get_int_member(pl, "online");
            }
            if (json_object_has_member(pl, "max")) {
                r->players_max = json_object_get_int_member(pl, "max");
            }
        }
    }

    if (json_object_has_member(obj, "description")) {
        JsonNode *desc = json_object_get_member(obj, "description");
        r->motd_plain = extract_text_component(desc);
    } else {
        r->motd_plain = g_strdup("");
    }

    if (json_object_has_member(obj, "favicon")) {
        const char *fav = json_object_get_string_member(obj, "favicon");
        if (fav && g_str_has_prefix(fav, "data:image/png;base64,")) {
            const char *b64 = fav + strlen("data:image/png;base64,");
            gsize out_len = 0;
            g_autofree guint8 *decoded = g_base64_decode(b64, &out_len);
            if (decoded && out_len > 8) {
                r->favicon_png = g_bytes_new_take(g_steal_pointer(&decoded), out_len);
            }
        }
    }

    g_object_unref(parser);

    *out_result = r;
    return TRUE;
}

// --- Core ping (sync helper used inside async) ---

static gboolean do_ping_sync(const char *host,
                            guint16 port,
                            int timeout_ms,
                            McPingResult **out_result,
                            GError **error) {
    if (timeout_ms <= 0) {
        timeout_ms = MC_TIMEOUT_FALLBACK_MS;
    }

    // Resolve SRV first (best effort).
    SrvTarget srv = resolve_srv_best_effort(host);
    const char *connect_host = srv.host ? srv.host : host;
    const guint16 connect_port = srv.port ? srv.port : port;

    const guint32 protocol_attempts[] = {760, 47};
    GError *last_error = NULL;

    for (guint i = 0; i < G_N_ELEMENTS(protocol_attempts); i++) {
        const guint32 protocol_version = protocol_attempts[i];

        GSocketClient *client = g_socket_client_new();
        g_socket_client_set_timeout(client, (guint)MAX(1, timeout_ms / 1000));
        g_socket_client_set_enable_proxy(client, FALSE);

        GError *attempt_error = NULL;
        GSocketConnection *conn = g_socket_client_connect_to_host(client,
                                                                 connect_host,
                                                                 connect_port,
                                                                 NULL,
                                                                 &attempt_error);
        g_object_unref(client);

        if (!conn) {
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));

        // Build handshake packet (id 0) for status state.
        GByteArray *handshake = g_byte_array_new();
        write_varint(handshake, 0x00);
        write_varint(handshake, protocol_version);

        // host string
        const gsize host_len = strlen(host);
        write_varint(handshake, (guint32)host_len);
        g_byte_array_append(handshake, (const guint8 *)host, host_len);

        // port (big endian)
        guint8 port_be[2] = {(guint8)((connect_port >> 8) & 0xFF), (guint8)(connect_port & 0xFF)};
        g_byte_array_append(handshake, port_be, 2);

        // next state: status (1)
        write_varint(handshake, 0x01);

        // Prepend length
        GByteArray *frame = g_byte_array_new();
        write_varint(frame, (guint32)handshake->len);
        g_byte_array_append(frame, handshake->data, handshake->len);

        g_byte_array_unref(handshake);

        gsize written = 0;
        if (!g_output_stream_write_all(out, frame->data, frame->len, &written, NULL, &attempt_error)) {
            g_byte_array_unref(frame);
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }
        g_byte_array_unref(frame);

        // Status request packet (length 1, id 0)
        guint8 req_packet[2] = {0x01, 0x00};
        if (!g_output_stream_write_all(out, req_packet, sizeof(req_packet), &written, NULL, &attempt_error)) {
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }
        g_output_stream_flush(out, NULL, NULL);

        // Read response: length VarInt, then packet data.
        guint8 buf[65536];
        gssize nread = g_input_stream_read(in, buf, sizeof(buf), NULL, &attempt_error);
        if (nread <= 0) {
            if (!attempt_error) {
                g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_FAILED, "No data received");
            }
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        gsize off = 0;
        guint32 packet_len = 0;
        if (!read_varint(buf, (gsize)nread, &off, &packet_len)) {
            g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bad packet length");
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        // Ensure buffer has at least packet_len bytes available.
        // If initial read didn't get all bytes, read the remainder.
        while ((gsize)nread - off < packet_len) {
            const gsize have = (gsize)nread;
            const gsize need_more = packet_len - (have - off);
            if (have + need_more > sizeof(buf)) {
                g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Packet too large");
                break;
            }
            gssize nr = g_input_stream_read(in, buf + have, (gsize)need_more, NULL, &attempt_error);
            if (nr <= 0) {
                if (!attempt_error) {
                    g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_FAILED, "Truncated packet");
                }
                break;
            }
            nread += nr;
        }
        if (attempt_error) {
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        guint32 packet_id = 0;
        if (!read_varint(buf, (gsize)nread, &off, &packet_id) || packet_id != 0x00) {
            g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected packet id");
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        guint32 json_len = 0;
        if (!read_varint(buf, (gsize)nread, &off, &json_len)) {
            g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bad JSON length");
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        if (off + json_len > (gsize)nread) {
            g_set_error(&attempt_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JSON truncated");
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        McPingResult *r = NULL;
        if (!parse_status_json(buf + off, json_len, &r, &attempt_error)) {
            g_object_unref(conn);
            g_clear_error(&last_error);
            last_error = attempt_error;
            continue;
        }

        r->resolved_host = g_strdup(connect_host);
        r->resolved_port = connect_port;

        g_object_unref(conn);
        g_clear_error(&last_error);
        srv_target_clear(&srv);

        *out_result = r;
        return TRUE;
    }

    srv_target_clear(&srv);
    if (last_error) {
        g_propagate_error(error, last_error);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Ping failed");
    }
    return FALSE;
}

// --- Async wrapper ---

typedef struct {
    gchar *host;
    guint16 port;
    int timeout_ms;
} PingTaskData;

static void ping_task_data_free(PingTaskData *d) {
    if (!d) {
        return;
    }
    g_free(d->host);
    g_free(d);
}

static void ping_task_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    PingTaskData *d = task_data;
    McPingResult *result = NULL;
    GError *error = NULL;

    if (!do_ping_sync(d->host, d->port, d->timeout_ms, &result, &error)) {
        g_task_return_error(task, error);
        return;
    }

    g_task_return_pointer(task, result, (GDestroyNotify)mc_ping_result_free);
}

void mc_ping_async(const char *host_or_ip,
                   guint16 port,
                   GCancellable *cancellable,
                   int timeout_ms,
                   GAsyncReadyCallback callback,
                   gpointer user_data) {
    if (!host_or_ip || !*host_or_ip) {
        host_or_ip = "";
    }
    if (port == 0) {
        port = MC_DEFAULT_PORT;
    }

    PingTaskData *d = g_new0(PingTaskData, 1);
    d->host = g_strdup(host_or_ip);
    d->port = port;
    d->timeout_ms = timeout_ms;

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, d, (GDestroyNotify)ping_task_data_free);
    g_task_run_in_thread(task, ping_task_thread);
    g_object_unref(task);
}

McPingResult *mc_ping_finish(GAsyncResult *res, GError **error) {
    g_return_val_if_fail(G_IS_TASK(res), NULL);
    return g_task_propagate_pointer(G_TASK(res), error);
}
