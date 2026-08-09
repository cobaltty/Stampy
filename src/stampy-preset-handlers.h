#pragma once

#include "stampy-window-private.h"

G_BEGIN_DECLS

void on_preset_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_btn_apply_preset_clicked(GtkButton *btn, gpointer user_data); /* <--- ADDED */
void on_btn_edit_preset_clicked(GtkButton *btn, gpointer user_data);
void on_btn_add_preset_clicked(GtkButton *btn, gpointer user_data);
void on_btn_remove_preset_clicked(GtkButton *btn, gpointer user_data);
void on_btn_import_preset_clicked(GtkButton *btn, gpointer user_data);
void on_btn_export_preset_clicked(GtkButton *btn, gpointer user_data);

G_END_DECLS
