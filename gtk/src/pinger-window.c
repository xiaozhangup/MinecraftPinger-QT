#include "pinger-window.h"

#include "minecraft-ping.h"
#include "progress-ring.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <gdk/gdk.h>

#include <stdlib.h>
#include <string.h>

#define REFRESH_PERIOD_MS 10000
#define SPINNER_TICK_MS 50

struct _PingerWindow {
    AdwApplicationWindow parent_instance;

    GtkEntry *entry;
    GtkButton *query_button;

    GtkImage *icon;
    GtkLabel *motd;
    GtkLabel *players;

    ProgressRing *ring;

    guint tick_id;
    gint64 last_success_us;

    GCancellable *cancellable;

    McPingResult *last_result;
};

G_DEFINE_TYPE(PingerWindow, pinger_window, ADW_TYPE_APPLICATION_WINDOW)

static void pinger_window_start_query(PingerWindow *self);

static void pinger_window_set_ring_spinner(PingerWindow *self, gboolean on) {
    progress_ring_set_indeterminate(self->ring, on);
}

static void pinger_window_set_ring_countdown(PingerWindow *self, double progress_0_1) {
    progress_ring_set_progress(self->ring, progress_0_1);
}

static void pinger_window_set_icon_from_png_bytes(PingerWindow *self, GBytes *png_bytes) {
    if (!png_bytes) {
        gtk_image_set_from_icon_name(self->icon, "network-server-symbolic");
        return;
    }

    GInputStream *stream = g_memory_input_stream_new_from_bytes(png_bytes);
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, &error);
    g_object_unref(stream);

    if (!pixbuf) {
        g_clear_error(&error);
        gtk_image_set_from_icon_name(self->icon, "network-server-symbolic");
        return;
    }

    // Scale to 64x64 max.
    const int w = gdk_pixbuf_get_width(pixbuf);
    const int h = gdk_pixbuf_get_height(pixbuf);
    const int side = (w < h) ? w : h;
    const int target = 64;

    GdkPixbuf *scaled = pixbuf;
    if (side > target) {
        const double s = (double)target / (double)side;
        const int nw = (int)(w * s);
        const int nh = (int)(h * s);
        scaled = gdk_pixbuf_scale_simple(pixbuf, MAX(1, nw), MAX(1, nh), GDK_INTERP_BILINEAR);
    }

    GdkTexture *tex = gdk_texture_new_for_pixbuf(scaled);
    gtk_image_set_from_paintable(self->icon, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    if (scaled != pixbuf) {
        g_object_unref(scaled);
    }
    g_object_unref(pixbuf);
}

static void pinger_window_update_size_hint(PingerWindow *self) {
    const char *motd = gtk_label_get_text(self->motd);
    if (!motd || !*motd) {
        return;
    }

    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self->motd), motd);
    int tw = 0;
    int th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    g_object_unref(layout);

    // Rough padding + icon column + margins.
    int target_w = tw + 64 + 48 + 120;
    target_w = CLAMP(target_w, 420, 980);

    gtk_window_set_default_size(GTK_WINDOW(self), target_w, -1);
}

static gboolean pinger_window_tick(gpointer user_data) {
    PingerWindow *self = user_data;

    if (progress_ring_get_indeterminate(self->ring)) {
        progress_ring_advance_spinner(self->ring);
        return G_SOURCE_CONTINUE;
    }

    if (self->last_success_us <= 0) {
        return G_SOURCE_CONTINUE;
    }

    const gint64 now_us = g_get_monotonic_time();
    const gint64 elapsed_ms = (now_us - self->last_success_us) / 1000;
    const gint64 remaining_ms = REFRESH_PERIOD_MS - elapsed_ms;

    if (remaining_ms <= 0) {
        pinger_window_start_query(self);
        return G_SOURCE_CONTINUE;
    }

    const double p = (double)remaining_ms / (double)REFRESH_PERIOD_MS;
    pinger_window_set_ring_countdown(self, p);
    return G_SOURCE_CONTINUE;
}

static void parse_host_port(const char *input, gchar **out_host, guint16 *out_port) {
    *out_host = NULL;
    *out_port = 25565;

    if (!input) {
        input = "";
    }

    g_autofree gchar *trim = g_strdup(input);
    g_strstrip(trim);

    if (!*trim) {
        *out_host = g_strdup("");
        return;
    }

    // Simple host:port parse; ignore IPv6 bracket for now.
    const char *colon = strrchr(trim, ':');
    if (colon && colon != trim && strchr(colon + 1, ':') == NULL) {
        g_autofree gchar *host = g_strndup(trim, colon - trim);
        const char *port_str = colon + 1;
        char *end = NULL;
        long p = strtol(port_str, &end, 10);
        if (end && *end == '\0' && p > 0 && p <= 65535) {
            *out_host = g_strdup(host);
            *out_port = (guint16)p;
            return;
        }
    }

    *out_host = g_strdup(trim);
    *out_port = 25565;
}

static void on_ping_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;

    PingerWindow *self = user_data;

    GError *error = NULL;
    McPingResult *r = mc_ping_finish(res, &error);

    if (!r) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&error);
            return;
        }

        // Keep old info; just show error in MOTD line.
        gtk_label_set_text(self->motd, error ? error->message : "Query failed");
        g_clear_error(&error);
        pinger_window_set_ring_spinner(self, FALSE);
        self->last_success_us = 0;
        pinger_window_set_ring_countdown(self, 0.0);
        return;
    }

    if (self->last_result) {
        mc_ping_result_free(self->last_result);
    }
    self->last_result = r;

    pinger_window_set_icon_from_png_bytes(self, r->favicon_png);

    gtk_label_set_text(self->motd, r->motd_plain ? r->motd_plain : "");

    g_autofree gchar *players = NULL;
    if (r->players_online >= 0 && r->players_max >= 0) {
        players = g_strdup_printf("%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT, r->players_online, r->players_max);
    } else {
        players = g_strdup("-");
    }
    gtk_label_set_text(self->players, players);

    pinger_window_set_ring_spinner(self, FALSE);

    // Start 10s countdown from result arrival.
    self->last_success_us = g_get_monotonic_time();
    pinger_window_set_ring_countdown(self, 1.0);

    pinger_window_update_size_hint(self);
}

static void pinger_window_start_query(PingerWindow *self) {
    // Cancel in-flight request.
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    self->cancellable = g_cancellable_new();

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->entry));
    gchar *host = NULL;
    guint16 port = 25565;
    parse_host_port(text, &host, &port);

    if (!host || !*host) {
        g_free(host);
        return;
    }

    // Keep old info during refresh; just switch ring to spinner.
    pinger_window_set_ring_spinner(self, TRUE);

    mc_ping_async(host, port, self->cancellable, 5000, on_ping_done, self);

    g_free(host);
}

static void on_query_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    PingerWindow *self = user_data;
    pinger_window_start_query(self);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    PingerWindow *self = user_data;
    pinger_window_start_query(self);
}

static gboolean start_initial_query_idle(gpointer user_data) {
    PingerWindow *self = user_data;
    pinger_window_start_query(self);
    return G_SOURCE_REMOVE;
}

static void pinger_window_dispose(GObject *object) {
    PingerWindow *self = PINGER_WINDOW(object);

    if (self->tick_id) {
        g_source_remove(self->tick_id);
        self->tick_id = 0;
    }

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }

    if (self->last_result) {
        mc_ping_result_free(self->last_result);
        self->last_result = NULL;
    }

    G_OBJECT_CLASS(pinger_window_parent_class)->dispose(object);
}

static void pinger_window_class_init(PingerWindowClass *klass) {
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose = pinger_window_dispose;
}

static void pinger_window_init(PingerWindow *self) {
    gtk_window_set_title(GTK_WINDOW(self), "MinecraftPinger");
    gtk_window_set_resizable(GTK_WINDOW(self), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(self), 520, 260);

    // Header bar with entry + query button
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    gtk_window_set_titlebar(GTK_WINDOW(self), GTK_WIDGET(hb));

    GtkBox *title_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    gtk_widget_set_halign(GTK_WIDGET(title_box), GTK_ALIGN_CENTER);

    self->entry = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(self->entry), "happylandmc.cc");
    gtk_entry_set_placeholder_text(self->entry, "server:port");
    gtk_entry_set_width_chars(self->entry, 22);

    self->query_button = GTK_BUTTON(gtk_button_new_with_label("查询"));
    gtk_widget_add_css_class(GTK_WIDGET(self->query_button), "suggested-action");

    gtk_box_append(title_box, GTK_WIDGET(self->entry));
    gtk_box_append(title_box, GTK_WIDGET(self->query_button));

    adw_header_bar_set_title_widget(hb, GTK_WIDGET(title_box));

    g_signal_connect(self->query_button, "clicked", G_CALLBACK(on_query_clicked), self);
    g_signal_connect(self->entry, "activate", G_CALLBACK(on_entry_activate), self);

    // Content
    GtkBox *root = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 16));
    gtk_widget_set_margin_top(GTK_WIDGET(root), 16);
    gtk_widget_set_margin_bottom(GTK_WIDGET(root), 16);
    gtk_widget_set_margin_start(GTK_WIDGET(root), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(root), 16);

    GtkOverlay *overlay = GTK_OVERLAY(gtk_overlay_new());

    GtkBox *card = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_add_css_class(GTK_WIDGET(card), "card");
    gtk_widget_set_margin_top(GTK_WIDGET(card), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(card), 8);
    gtk_widget_set_margin_start(GTK_WIDGET(card), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(card), 8);

    self->icon = GTK_IMAGE(gtk_image_new_from_icon_name("network-server-symbolic"));
    gtk_image_set_pixel_size(self->icon, 64);

    GtkBox *text_col = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));

    self->motd = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(self->motd, 0.0f);
    gtk_label_set_wrap(self->motd, FALSE);
    gtk_label_set_ellipsize(self->motd, PANGO_ELLIPSIZE_END);

    self->players = GTK_LABEL(gtk_label_new("-"));
    gtk_label_set_xalign(self->players, 0.5f);
    gtk_widget_add_css_class(GTK_WIDGET(self->players), "title-2");

    gtk_box_append(text_col, GTK_WIDGET(self->motd));
    gtk_box_append(text_col, GTK_WIDGET(self->players));

    gtk_box_append(card, GTK_WIDGET(self->icon));
    gtk_box_append(card, GTK_WIDGET(text_col));

    gtk_overlay_set_child(overlay, GTK_WIDGET(card));

    self->ring = PROGRESS_RING(progress_ring_new());
    gtk_widget_set_halign(GTK_WIDGET(self->ring), GTK_ALIGN_END);
    gtk_widget_set_valign(GTK_WIDGET(self->ring), GTK_ALIGN_END);
    gtk_widget_set_margin_end(GTK_WIDGET(self->ring), 10);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->ring), 10);

    gtk_overlay_add_overlay(overlay, GTK_WIDGET(self->ring));

    gtk_box_append(root, GTK_WIDGET(overlay));

    gtk_window_set_child(GTK_WINDOW(self), GTK_WIDGET(root));

    self->tick_id = g_timeout_add(SPINNER_TICK_MS, pinger_window_tick, self);

    // Auto query on startup.
    g_idle_add(start_initial_query_idle, self);
}

PingerWindow *pinger_window_new(AdwApplication *app) {
    return g_object_new(PINGER_WINDOW_TYPE, "application", app, NULL);
}
