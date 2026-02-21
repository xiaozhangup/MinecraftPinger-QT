#include "progress-ring.h"

#include <math.h>

struct _ProgressRing {
    GtkWidget parent_instance;

    gboolean indeterminate;
    double progress;
    double phase_deg;
};

G_DEFINE_TYPE(ProgressRing, progress_ring, GTK_TYPE_WIDGET)

static void progress_ring_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ProgressRing *self = PROGRESS_RING(widget);

    const int w = gtk_widget_get_width(widget);
    const int h = gtk_widget_get_height(widget);
    const int side = (w < h) ? w : h;

    const float pad = 2.0f;
    const float cx = w / 2.0f;
    const float cy = h / 2.0f;
    const float radius = side / 2.0f - pad;

    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0, 0, w, h);

    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    // Background track
    cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    if (self->indeterminate) {
        // A single white dot rotating.
        const double angle = (self->phase_deg - 90.0) * (G_PI / 180.0);
        const double px = cx + radius * cos(angle);
        const double py = cy + radius * sin(angle);

        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_arc(cr, px, py, 2.3, 0, 2 * G_PI);
        cairo_fill(cr);

        cairo_destroy(cr);
        return;
    }

    // Foreground arc
    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    const double p = CLAMP(self->progress, 0.0, 1.0);
    const double start = -G_PI / 2.0;
    const double end = start + (2 * G_PI * p);
    cairo_arc(cr, cx, cy, radius, start, end);
    cairo_stroke(cr);

    cairo_destroy(cr);
}

static void progress_ring_measure(GtkWidget *widget,
                                 GtkOrientation orientation,
                                 int for_size,
                                 int *minimum,
                                 int *natural,
                                 int *minimum_baseline,
                                 int *natural_baseline) {
    (void)for_size;
    (void)minimum_baseline;
    (void)natural_baseline;

    // Small, like the Qt version.
    if (minimum) {
        *minimum = 22;
    }
    if (natural) {
        *natural = 22;
    }
}

static void progress_ring_class_init(ProgressRingClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = progress_ring_snapshot;
    widget_class->measure = progress_ring_measure;
}

static void progress_ring_init(ProgressRing *self) {
    self->indeterminate = FALSE;
    self->progress = 0.0;
    self->phase_deg = 0.0;

    gtk_widget_set_can_focus(GTK_WIDGET(self), FALSE);
}

GtkWidget *progress_ring_new(void) {
    return g_object_new(PROGRESS_RING_TYPE, NULL);
}

void progress_ring_set_indeterminate(ProgressRing *self, gboolean indeterminate) {
    g_return_if_fail(PROGRESS_IS_RING(self));
    self->indeterminate = indeterminate ? TRUE : FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean progress_ring_get_indeterminate(ProgressRing *self) {
    g_return_val_if_fail(PROGRESS_IS_RING(self), FALSE);
    return self->indeterminate;
}

void progress_ring_set_progress(ProgressRing *self, double progress_0_1) {
    g_return_if_fail(PROGRESS_IS_RING(self));
    self->indeterminate = FALSE;
    self->progress = CLAMP(progress_0_1, 0.0, 1.0);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

double progress_ring_get_progress(ProgressRing *self) {
    g_return_val_if_fail(PROGRESS_IS_RING(self), 0.0);
    return self->progress;
}

void progress_ring_advance_spinner(ProgressRing *self) {
    g_return_if_fail(PROGRESS_IS_RING(self));

    // Roughly one rotation per ~1s when tick is 50ms.
    self->phase_deg += 18.0;
    if (self->phase_deg >= 360.0) {
        self->phase_deg -= 360.0;
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
