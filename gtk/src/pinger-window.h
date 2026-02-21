#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PINGER_WINDOW_TYPE (pinger_window_get_type())
G_DECLARE_FINAL_TYPE(PingerWindow, pinger_window, PINGER, WINDOW, AdwApplicationWindow)

PingerWindow *pinger_window_new(AdwApplication *app);

G_END_DECLS
