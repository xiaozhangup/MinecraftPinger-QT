#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PROGRESS_RING_TYPE (progress_ring_get_type())
G_DECLARE_FINAL_TYPE(ProgressRing, progress_ring, PROGRESS, RING, GtkWidget)

GtkWidget *progress_ring_new(void);

void progress_ring_set_indeterminate(ProgressRing *self, gboolean indeterminate);
gboolean progress_ring_get_indeterminate(ProgressRing *self);

void progress_ring_set_progress(ProgressRing *self, double progress_0_1);
double progress_ring_get_progress(ProgressRing *self);

void progress_ring_advance_spinner(ProgressRing *self);

G_END_DECLS
