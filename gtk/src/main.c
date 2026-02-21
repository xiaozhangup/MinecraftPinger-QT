#include <adwaita.h>

#include "pinger-window.h"

static void on_activate(GApplication *app, gpointer user_data) {
    (void)user_data;

    PingerWindow *win = pinger_window_new(ADW_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
    g_autoptr(AdwApplication) app = adw_application_new("cc.happyland.MinecraftPinger", G_APPLICATION_DEFAULT_FLAGS);
    adw_init();

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    return g_application_run(G_APPLICATION(app), argc, argv);
}
