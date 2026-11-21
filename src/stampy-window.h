#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define STAMPY_TYPE_WINDOW (stampy_window_get_type())
G_DECLARE_FINAL_TYPE(StampyWindow, stampy_window, STAMPY, WINDOW, AdwApplicationWindow)

StampyWindow *stampy_window_new(GtkApplication *app);
StampyWindow *stampy_window_new_quick(GtkApplication *app);
gboolean stampy_window_is_quick_mode(StampyWindow *self);

G_END_DECLS
