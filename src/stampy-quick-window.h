#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define STAMPY_TYPE_QUICK_WINDOW (stampy_quick_window_get_type())
G_DECLARE_FINAL_TYPE(StampyQuickWindow, stampy_quick_window, STAMPY, QUICK_WINDOW, AdwApplicationWindow)

StampyQuickWindow *stampy_quick_window_new(GtkApplication *app, GSList *files);

G_END_DECLS
