#include "stampy-window-private.h"
#include "stampy-preset-handlers.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Helper untuk mengurutkan file berdasarkan angka murni (Natural Sorting) */
static gint compare_file_basenames(gconstpointer a, gconstpointer b) {
    GFile *file_a = *(GFile **)a;
    GFile *file_b = *(GFile **)b;
    g_autofree char *name_a = NULL;
    g_autofree char *name_b = NULL;
    int id_a = -1, id_b = -1;

    if (!file_a || !file_b) return 0;

    name_a = g_file_get_basename(file_a);
    name_b = g_file_get_basename(file_b);

    /* Ekstrak angka murni dari string A */
    if (name_a) {
        GString *da = g_string_new("");
        for (const char *p = name_a; *p; p++) {
            if (g_ascii_isdigit(*p)) g_string_append_c(da, *p);
        }
        if (da->len > 0) id_a = atoi(da->str);
        g_string_free(da, TRUE);
    }

    /* Ekstrak angka murni dari string B */
    if (name_b) {
        GString *db = g_string_new("");
        for (const char *p = name_b; *p; p++) {
            if (g_ascii_isdigit(*p)) g_string_append_c(db, *p);
        }
        if (db->len > 0) id_b = atoi(db->str);
        g_string_free(db, TRUE);
    }

    /* Jika kedua file memiliki angka, bandingkan nilai matematisnya */
    if (id_a >= 0 && id_b >= 0 && id_a != id_b) {
        return id_a - id_b;
    }

    /* Jika tidak ada angka (atau angkanya persis sama), fallback ke A-Z biasa */
    return g_utf8_collate(name_a ? name_a : "", name_b ? name_b : "");
}

/* Callback ketika seleksi baris listbox berubah */
static void on_listbox_selection_changed(GtkListBox *box, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GList *selected_rows;
    guint count;

    if (!self || !box) return;

    selected_rows = gtk_list_box_get_selected_rows(box);
    count = g_list_length(selected_rows);

    if (count == 1) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(selected_rows->data);
        int index = gtk_list_box_row_get_index(row);

        if (self->preview_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->preview_stack), "single")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->preview_stack), "single");
            }
        }

        if (index >= 0 && self->image_files && (guint)index < self->image_files->len) {
            stampy_render_live_preview(self);
        }
    } else if (count > 1) {
        if (self->preview_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->preview_stack), "multi")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->preview_stack), "multi");
            }
        }
    }

    g_list_free(selected_rows);
}

/* Callback gesture klik kiri untuk memastikan klik normal memilih single item secara bersih */
static void on_listbox_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkListBoxRow *row;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    (void)n_press; (void)x;
    if (!self || !self->file_listbox) return;

    row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->file_listbox), (int)y);
    if (!row) return;

    /* Jika tidak menekan Shift atau Ctrl, klik biasa mereset seleksi ke baris ini saja */
    if (!(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
        GList *selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
        if (g_list_length(selected) != 1 || selected->data != row) {
            gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
            gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), row);
        }
        g_list_free(selected);
    }
}

static void on_panel_setting_changed(GtkWidget *widget, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)widget;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    stampy_render_live_preview(self);
    stampy_save_last_state(self);
}

static void on_dropdown_setting_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)gobject; (void)pspec;
    if (!self || !STAMPY_IS_WINDOW(self)) return;
    on_panel_setting_changed(NULL, self);
}

void stampy_connect_panel_signals(StampyWindow *self) {
    if (!self) return;

    if (self->val_compass_size) g_signal_connect(self->val_compass_size, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_margin_x) g_signal_connect(self->val_compass_margin_x, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_margin_y) g_signal_connect(self->val_compass_margin_y, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_compass_opacity) g_signal_connect(self->val_compass_opacity, "value-changed", G_CALLBACK(on_panel_setting_changed), self);

    if (self->drop_compass_pos) g_signal_connect(self->drop_compass_pos, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);
    if (self->drop_text_pos) g_signal_connect(self->drop_text_pos, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);
    if (self->drop_text_font) g_signal_connect(self->drop_text_font, "notify::selected", G_CALLBACK(on_dropdown_setting_changed), self);

    if (self->btn_c_shadow) g_signal_connect(self->btn_c_shadow, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_c_stroke) g_signal_connect(self->btn_c_stroke, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_c_stroke_color) g_signal_connect(self->btn_c_stroke_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);

    if (self->val_text_size) g_signal_connect(self->val_text_size, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_margin_x) g_signal_connect(self->val_text_margin_x, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_margin_y) g_signal_connect(self->val_text_margin_y, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_opacity) g_signal_connect(self->val_text_opacity, "value-changed", G_CALLBACK(on_panel_setting_changed), self);

    if (self->btn_t_align_left) g_signal_connect(self->btn_t_align_left, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_align_center) g_signal_connect(self->btn_t_align_center, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_align_right) g_signal_connect(self->btn_t_align_right, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_bold) g_signal_connect(self->btn_t_bold, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_italic) g_signal_connect(self->btn_t_italic, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_underline) g_signal_connect(self->btn_t_underline, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_shadow) g_signal_connect(self->btn_t_shadow, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_stroke) g_signal_connect(self->btn_t_stroke, "toggled", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_stroke_color) g_signal_connect(self->btn_t_stroke_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->btn_t_color) g_signal_connect(self->btn_t_color, "color-set", G_CALLBACK(on_panel_setting_changed), self);
    if (self->val_text_spacing) g_signal_connect(self->val_text_spacing, "value-changed", G_CALLBACK(on_panel_setting_changed), self);
}

/* POPULATE FILE LISTBOX */
void stampy_populate_file_listbox(StampyWindow *self) {
    GtkListBoxRow *first_row;
    guint total, i;
    GtkWidget *child;

    if (!self || !self->file_listbox) return;

    /* Panggil fungsi Natural Sorting kita yang baru */
    if (self->image_files && self->image_files->len > 0) {
        g_ptr_array_sort(self->image_files, compare_file_basenames);
    }

    g_signal_handlers_block_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    /* Hapus elemen UI secara aman */
    child = gtk_widget_get_first_child(GTK_WIDGET(self->file_listbox));
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(self->file_listbox), child);
        child = next;
    }

    if (!self->image_files || self->image_files->len == 0) {
        g_signal_handlers_unblock_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

        if (self->left_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_drop")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_drop");
            }
        }
        if (self->img_preview) {
            gtk_picture_set_paintable(GTK_PICTURE(self->img_preview), NULL);
        }
        return;
    }

    total = self->image_files->len;

    /* Rebuild ListBox rows */
    for (i = 0; i < total; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        if (file) {
            char *basename = g_file_get_basename(file);
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *label = gtk_label_new(basename);

            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_margin_start(label, 12);
            gtk_widget_set_margin_end(label, 12);
            gtk_widget_set_margin_top(label, 8);
            gtk_widget_set_margin_bottom(label, 8);

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_append(GTK_LIST_BOX(self->file_listbox), row);
            g_free(basename);
        }
    }

    g_signal_handlers_unblock_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    /* Switch stack page to display the list */
    if (total > 0) {
        if (self->left_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_list")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_list");
            }
        }

        /* Select first row by default */
        first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->file_listbox), 0);
        if (first_row) {
            gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), first_row);
        }
    }
}

static void on_listbox_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GMenu *context_menu;
    GtkWidget *popover;
    GdkRectangle rect;
    GtkListBoxRow *row;

    (void)gesture; (void)n_press;
    if (!self || !self->file_listbox) return;

    row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->file_listbox), (int)y);
    if (row) {
        GList *selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
        if (!g_list_find(selected, row)) {
            gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
            gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), row);
        }
        g_list_free(selected);
    }

    context_menu = g_menu_new();
    g_menu_append(context_menu, "Add Images", "win.add-images");
    g_menu_append(context_menu, "Select All", "win.select-all");
    g_menu_append(context_menu, "Deselect All", "win.deselect-all");
    g_menu_append(context_menu, "Remove Selected", "win.remove-selected");

    popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(context_menu));
    gtk_widget_set_parent(popover, GTK_WIDGET(self->file_listbox));

    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_popup(GTK_POPOVER(popover));

    g_object_unref(context_menu);
}

static void on_select_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->file_listbox) {
        gtk_list_box_select_all(GTK_LIST_BOX(self->file_listbox));
    }
}

static void on_deselect_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->file_listbox) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(self->file_listbox));
    }
}

static gint compare_ints_desc(gconstpointer a, gconstpointer b) {
    int val_a = *(const int*)a;
    int val_b = *(const int*)b;
    if (val_b > val_a) return 1;
    if (val_b < val_a) return -1;
    return 0;
}

static void on_remove_selected_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GArray *indices = NULL;
    GList *selected_rows, *l;

    (void)action; (void)parameter;
    if (!self || !self->file_listbox || !self->image_files) return;

    selected_rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(self->file_listbox));
    if (!selected_rows) return;

    indices = g_array_new(FALSE, FALSE, sizeof(int));

    for (l = selected_rows; l != NULL; l = l->next) {
        int idx = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(l->data));
        if (idx >= 0) g_array_append_val(indices, idx);
    }
    g_list_free(selected_rows);

    if (indices->len > 0) {
        guint i;
        g_array_sort(indices, compare_ints_desc);

        for (i = 0; i < indices->len; i++) {
            int remove_idx = g_array_index(indices, int, i);
            if (remove_idx >= 0 && (guint)remove_idx < self->image_files->len) {
                g_ptr_array_remove_index(self->image_files, remove_idx);
            }
        }
    }

    g_array_free(indices, TRUE);

    stampy_populate_file_listbox(self);

    if (self->image_files && self->image_files->len > 0) {
        stampy_render_live_preview(self);
    }

    stampy_update_clear_button_visibility(self);

    stampy_show_toast(self, "Selected images removed!");
    stampy_append_log(self, "[INFO] Selected images removed.");
}

static void on_drop_area_frame_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)btn;
    if (self) stampy_open_image_picker(self);
}

void stampy_update_clear_button_visibility(StampyWindow *self) {
    gboolean has_asset;

    if (!self) return;

    has_asset = (self->file_compass || self->file_watermark || self->file_geolocation ||
                 (self->image_files && self->image_files->len > 0));

    /* Compass lock button stays visible; lock state is indicated by its icon */
    if (self->btn_compass_lock)
        gtk_widget_set_visible(self->btn_compass_lock, TRUE);
    if (self->btn_header_clear)
        gtk_widget_set_visible(self->btn_header_clear, has_asset);
}

static void on_clear_all_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)btn;
    if (!self) return;

    if (self->image_files) {
        g_ptr_array_remove_range(self->image_files, 0, self->image_files->len);
    }

    if (self->geo_table) {
        g_hash_table_remove_all(self->geo_table);
    }

    if (!self->compass_locked) {
        g_clear_object(&self->file_compass);
        if (self->lbl_compass) gtk_label_set_text(GTK_LABEL(self->lbl_compass), "Add Compass");
        if (self->btn_compass_lock) {
            self->compass_locked = FALSE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock), FALSE);
        }
        /* Forget the remembered folder too (it was not locked). */
        stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER, "");
    }

    g_clear_object(&self->file_watermark);
    g_clear_object(&self->file_geolocation);

    if (self->lbl_watermark) gtk_label_set_text(GTK_LABEL(self->lbl_watermark), "Add Watermark(.docx)");
    if (self->lbl_geolocation) gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), "Add Geolocation(.xlsx)");

    stampy_populate_file_listbox(self);

    if (self->img_preview) {
        gtk_picture_set_paintable(GTK_PICTURE(self->img_preview), NULL);
    }

    stampy_show_toast(self, "All files removed!");
    stampy_append_log(self, "[INFO] Removed all images, compass, watermark, and geolocation data.");
    stampy_update_clear_button_visibility(self);
}

/* ==========================================================
   WHAT'S NEW DIALOG
   ========================================================== */
static void on_whats_new_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    GtkWidget *box, *heading, *content_label;

    (void)action; (void)parameter;
    if (!self) return;
    /* Buat dialog Adwaita */
    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "What's New in Stampy");
    adw_dialog_set_content_width(dialog, 450);
    adw_dialog_set_content_height(dialog, 400);

    /* Kontainer utama dialog */
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);

    /* Judul Dialog */
    heading = gtk_label_new("What's New in 1.4.001.beta?");
    gtk_widget_add_css_class(heading, "title-2");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);

    /* What's New Log */
    content_label = gtk_label_new(NULL);
    content_label = gtk_label_new(
        "TETO RELEASE (0401)\n"
        "MAJOR UPDATE!! IS OUT OF ALPHA PHASE 🎊\n"
        "• Keyboard Shortcuts has been implemented.\n"
        "• Compass lock has been implemented.\n"
        "• Intterupt has been implemented.\n"
        "• Notification has been added when export is done.\n"
        "• New Easteregg regarding for 0401 release:3\n"
        "• Major performance improvement.\n"
        " Is possible by enabling all cores to do all the job\n"
        " the performance boost can be from 30% up to 50%.\n\n"
        "1.3.021.alpha\n"
        "• Minor bug fix.\n\n"
        "1.3.020.alpha\n"
        "• Redesign Core\n"
        "  Rethinking logic of exif.\n\n"
        "1.2.041.alpha\n"
        "• ExifTool Engine Integration\n"
        "  Preserves original Date Taken & EXIF metadata with 100% accuracy (hopefully).\n"
        "• Interrupt / Cancel Export\n"
        "  Instantly stop batch processing with the red Cancel button.\n"
        "• Smart Auto-Swap Coordinates\n"
        "  Automatically detects and fixes inverted Latitude/Longitude columns.\n"
        "• Decimal Locale Resolver\n"
        "  Seamlessly supports comma (,) and dot (.) decimal formats from Excel.\n"
        "• New accompany!\n"
        "  Prof. Teto is here to watching you and motivates you during work!\n"
    );
    gtk_label_set_use_markup(GTK_LABEL(content_label), TRUE); /* Aktifkan agar tag <b> terbaca */
    gtk_label_set_wrap(GTK_LABEL(content_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(content_label), 0.0);

    /* Masukkan ke dalam kotak dan tampilkan ke layar */
    gtk_box_append(GTK_BOX(box), heading);
    gtk_box_append(GTK_BOX(box), content_label);

    adw_dialog_set_child(dialog, box);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ==========================================================
   KEYBOARD SHORTCUT ACTION CALLBACKS
   ========================================================== */
static void on_watermark_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_watermark_picker(self);
}

static void on_geolocation_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_geolocation_picker(self);
}

static void on_compass_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_compass_picker(self);
}

static void on_compass_lock_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_toggle_compass_lock(self);
}

static void on_export_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_start_export(self);
}

static void on_cancel_export_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_cancel_export(self);
}

static void on_remove_all_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self) on_clear_all_clicked(NULL, self);
}

static void on_select_preset_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self && self->drop_presets) {
        gtk_widget_activate(GTK_WIDGET(self->drop_presets));
    }
}

static void on_preferences_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    if (self) stampy_show_preferences(self);
}

void stampy_setup_actions_and_menus(StampyWindow *self) {
    GSimpleAction *act_add;
    GSimpleAction *act_sel_all;
    GSimpleAction *act_desel_all;
    GSimpleAction *act_rem_sel;
    GSimpleAction *act_whats_new;
    GSimpleAction *act_watermark;
    GSimpleAction *act_geolocation;
    GSimpleAction *act_compass;
    GSimpleAction *act_compass_lock;
    GSimpleAction *act_export;
    GSimpleAction *act_cancel_export;
    GSimpleAction *act_remove_all;
    GSimpleAction *act_select_preset;
    GSimpleAction *act_preferences;

    if (!self) return;

    if (self->drop_area_frame) {
        g_signal_connect(self->drop_area_frame, "clicked", G_CALLBACK(on_drop_area_frame_clicked), self);
    }

    if (self->btn_header_clear) {
        g_signal_connect(self->btn_header_clear, "clicked", G_CALLBACK(on_clear_all_clicked), self);
        gtk_widget_set_visible(self->btn_header_clear, FALSE);
    }

    if (self->file_listbox) {
        GtkGesture *left_click;
        GtkGesture *right_click;

        gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->file_listbox), GTK_SELECTION_MULTIPLE);
        g_signal_connect(self->file_listbox, "selected-rows-changed", G_CALLBACK(on_listbox_selection_changed), self);

        left_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(left_click, "pressed", G_CALLBACK(on_listbox_left_click), self);
        gtk_widget_add_controller(GTK_WIDGET(self->file_listbox), GTK_EVENT_CONTROLLER(left_click));

        right_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(right_click, "pressed", G_CALLBACK(on_listbox_right_click), self);
        gtk_widget_add_controller(GTK_WIDGET(self->file_listbox), GTK_EVENT_CONTROLLER(right_click));
    }

    if (self->drop_presets) {
        g_signal_connect(self->drop_presets, "notify::selected", G_CALLBACK(on_preset_dropdown_changed), self);
    }
    if (self->btn_add_preset) {
        g_signal_connect(self->btn_add_preset, "clicked", G_CALLBACK(on_btn_add_preset_clicked), self);
    }
    if (self->btn_remove_preset) {
        g_signal_connect(self->btn_remove_preset, "clicked", G_CALLBACK(on_btn_remove_preset_clicked), self);
    }
    if (self->btn_edit_preset) {
        g_signal_connect(self->btn_edit_preset, "clicked", G_CALLBACK(on_btn_edit_preset_clicked), self);
    }
    if (self->btn_apply_preset) {
        g_signal_connect(self->btn_apply_preset, "clicked", G_CALLBACK(on_btn_apply_preset_clicked), self);
    }
    if (self->btn_import_preset) {
        g_signal_connect(self->btn_import_preset, "clicked", G_CALLBACK(on_btn_import_preset_clicked), self);
    }
    if (self->btn_export_preset) {
        g_signal_connect(self->btn_export_preset, "clicked", G_CALLBACK(on_btn_export_preset_clicked), self);
    }

    act_add = g_simple_action_new("add-images", NULL);
    g_signal_connect(act_add, "activate", G_CALLBACK(on_add_images_action_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_add));

    act_sel_all = g_simple_action_new("select-all", NULL);
    g_signal_connect(act_sel_all, "activate", G_CALLBACK(on_select_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_sel_all));

    act_desel_all = g_simple_action_new("deselect-all", NULL);
    g_signal_connect(act_desel_all, "activate", G_CALLBACK(on_deselect_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_desel_all));

    act_rem_sel = g_simple_action_new("remove-selected", NULL);
    g_signal_connect(act_rem_sel, "activate", G_CALLBACK(on_remove_selected_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_rem_sel));

    /* PENDAFTARAN ACTION "WHAT'S NEW" */
    act_whats_new = g_simple_action_new("whats-new", NULL);
    g_signal_connect(act_whats_new, "activate", G_CALLBACK(on_whats_new_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_whats_new));

    /* --- KEYBOARD SHORTCUT ACTIONS --- */
    act_watermark = g_simple_action_new("watermark", NULL);
    g_signal_connect(act_watermark, "activate", G_CALLBACK(on_watermark_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_watermark));

    act_geolocation = g_simple_action_new("geolocation", NULL);
    g_signal_connect(act_geolocation, "activate", G_CALLBACK(on_geolocation_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_geolocation));

    act_compass = g_simple_action_new("compass", NULL);
    g_signal_connect(act_compass, "activate", G_CALLBACK(on_compass_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_compass));

    act_compass_lock = g_simple_action_new("compass-lock", NULL);
    g_signal_connect(act_compass_lock, "activate", G_CALLBACK(on_compass_lock_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_compass_lock));

    act_export = g_simple_action_new("export", NULL);
    g_signal_connect(act_export, "activate", G_CALLBACK(on_export_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_export));

    act_cancel_export = g_simple_action_new("cancel-export", NULL);
    g_signal_connect(act_cancel_export, "activate", G_CALLBACK(on_cancel_export_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_cancel_export));

    act_remove_all = g_simple_action_new("remove-all", NULL);
    g_signal_connect(act_remove_all, "activate", G_CALLBACK(on_remove_all_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_remove_all));

    act_select_preset = g_simple_action_new("select-preset", NULL);
    g_signal_connect(act_select_preset, "activate", G_CALLBACK(on_select_preset_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_select_preset));

    act_preferences = g_simple_action_new("preferences", NULL);
    g_signal_connect(act_preferences, "activate", G_CALLBACK(on_preferences_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_preferences));
}
