#include "stampy-window-private.h"
#include "stampy-preset-handlers.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static inline int extract_digits_fast(const char *name) {
    int val = -1;
    const char *p;
    if (!name) return -1;
    for (p = name; *p; p++) {
        if (g_ascii_isdigit(*p)) {
            if (val < 0) val = 0;
            if (val < 100000000) val = val * 10 + (*p - '0');
        }
    }
    return val;
}

/* Sorts files by their digit sequence ("2.jpg" before "10.jpg") with zero allocations. */
static gint compare_file_basenames(gconstpointer a, gconstpointer b) {
    GFile *file_a = *(GFile **)a;
    GFile *file_b = *(GFile **)b;
    const char *path_a;
    const char *path_b;
    const char *base_a;
    const char *base_b;
    int id_a, id_b;

    if (!file_a || !file_b) return 0;

    path_a = g_file_peek_path(file_a);
    path_b = g_file_peek_path(file_b);

    if (path_a) {
        base_a = strrchr(path_a, '/');
        base_a = base_a ? base_a + 1 : path_a;
    } else {
        base_a = "";
    }

    if (path_b) {
        base_b = strrchr(path_b, '/');
        base_b = base_b ? base_b + 1 : path_b;
    } else {
        base_b = "";
    }

    id_a = extract_digits_fast(base_a);
    id_b = extract_digits_fast(base_b);

    if (id_a >= 0 && id_b >= 0 && id_a != id_b) {
        return id_a - id_b;
    }

    return strcmp(base_a, base_b);
}

/* Updates the preview when the listbox selection changes. */
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

/* Plain left-click resets the selection to the clicked row. */
static void on_listbox_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkListBoxRow *row;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    (void)n_press; (void)x;
    if (!self || !self->file_listbox) return;

    row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->file_listbox), (int)y);
    if (!row) return;

    /* Without Shift/Ctrl held, a click selects only this row. */
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
    stampy_schedule_save_last_state(self);
}

static void on_metadata_inject_toggled(GObject *object, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkSwitch *btn = GTK_SWITCH(object);
    gboolean active;

    (void)pspec;
    if (!self || !STAMPY_IS_WINDOW(self)) return;
    active = gtk_switch_get_active(btn);
    stampy_config_set_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, active);
    stampy_render_live_preview(self);
    stampy_schedule_save_last_state(self);
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

    if (self->btn_metadata_inject) g_signal_connect(self->btn_metadata_inject, "notify::active", G_CALLBACK(on_metadata_inject_toggled), self);

    if (self->btn_xlsx_time_exif) g_signal_connect(self->btn_xlsx_time_exif, "notify::active", G_CALLBACK(on_xlsx_time_exif_toggled), self);
}

/* Rebuilds the file listbox, sorted naturally. */
static void append_file_row(StampyWindow *self, GFile *file) {
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

static void finish_list_population(StampyWindow *self) {
    GtkListBoxRow *first_row;

    gtk_widget_set_visible(GTK_WIDGET(self->file_listbox), TRUE);
    g_signal_handlers_unblock_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    if (self->left_stack) {
        if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_list")) {
            gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_list");
        }
    }

    first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->file_listbox), 0);
    if (first_row) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->file_listbox), first_row);
    }

    if (!self->export_active && self->scan_pulse_id == 0) {
        if (self->progress_bar) gtk_widget_set_visible(self->progress_bar, FALSE);
        gtk_window_set_title(GTK_WINDOW(self), "Stampy");
    }

    stampy_check_horizontal_photos(self);
    stampy_check_missing_data(self);
}

typedef struct {
    StampyWindow *self;
    guint generation;
    guint index;
    guint total;
} PopulateTask;

static void populate_task_free(gpointer data) {
    PopulateTask *task = data;
    if (task) {
        if (task->self) g_object_unref(task->self);
        g_free(task);
    }
}

/* Adds rows in small chunks so a 20k-photo import stays responsive. */
static gboolean populate_chunk_cb(gpointer user_data) {
    PopulateTask *task = user_data;
    StampyWindow *self = task->self;
    guint end;

    if (task->generation != self->populate_generation ||
        !self->image_files || task->total != self->image_files->len) {
        self->populate_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    end = MIN(task->index + 250, task->total);
    for (; task->index < end; task->index++) {
        GFile *file = g_ptr_array_index(self->image_files, task->index);
        if (file) append_file_row(self, file);
    }

    if (self->progress_bar) {
        double fraction = task->total > 0 ? (double)task->index / (double)task->total : 1.0;
        char *title = g_strdup_printf("Loading list… %d%%", (int)(fraction * 100.0 + 0.5));

        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
        gtk_widget_set_visible(self->progress_bar, TRUE);
        if (!self->export_active) gtk_window_set_title(GTK_WINDOW(self), title);
        g_free(title);
    }

    if (task->index < task->total) return G_SOURCE_CONTINUE;

    self->populate_source_id = 0;
    finish_list_population(self);
    return G_SOURCE_REMOVE;
}

/* Guarantees one list entry per output filename. Duplicate basenames (for
 * example a photo and its previously exported copy) would overwrite each
 * other on export, so only the first occurrence is kept. */
static void dedupe_image_files(StampyWindow *self) {
    GHashTable *seen;
    GPtrArray *unique;
    guint i, j;
    guint removed = 0;

    if (!self || !self->image_files || self->image_files->len < 2) return;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    unique = g_ptr_array_new_with_free_func(g_object_unref);

    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        const char *path;
        const char *base;
        char *key = NULL;
        gboolean duplicate = FALSE;

        if (!file) continue;

        /* Key by basename: exports are written by basename into one folder,
         * so two files with the same name always collide anyway. */
        path = g_file_peek_path(file);
        if (path) {
            base = strrchr(path, '/');
            base = base ? base + 1 : path;
            key = g_utf8_strdown(base, -1);
        }

        if (key) {
            if (g_hash_table_contains(seen, key))
                duplicate = TRUE;
            else
                g_hash_table_add(seen, g_strdup(key));
        } else {
            for (j = 0; j < unique->len; j++) {
                if (g_file_equal(g_ptr_array_index(unique, j), file)) {
                    duplicate = TRUE;
                    break;
                }
            }
        }

        if (duplicate) {
            removed++;
        } else {
            g_ptr_array_add(unique, g_object_ref(file));
        }
        g_free(key);
    }

    g_hash_table_unref(seen);

    if (removed > 0) {
        g_ptr_array_unref(self->image_files);
        self->image_files = unique;
        stampy_append_log(self, "[WARN] Removed duplicate image entries.");
    } else {
        g_ptr_array_unref(unique);
    }
}

static void clear_listbox_safely(GtkListBox *box) {
    GtkWidget *child;

    if (!box) return;
    child = gtk_widget_get_first_child(GTK_WIDGET(box));
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_LIST_BOX_ROW(child)) {
            gtk_list_box_remove(box, child);
        } else {
            gtk_widget_unparent(child);
        }
        child = next;
    }
}

void stampy_populate_file_listbox(StampyWindow *self) {
    guint total, i;

    if (!self || !self->file_listbox) return;

    dedupe_image_files(self);

    /* Cancel any in-flight chunked population. */
    self->populate_generation++;
    if (self->populate_source_id > 0) {
        g_source_remove(self->populate_source_id);
        self->populate_source_id = 0;
    }

    /* Sort before rebuilding rows. */
    if (self->image_files && self->image_files->len > 0) {
        g_ptr_array_sort(self->image_files, compare_file_basenames);
    }

    g_signal_handlers_block_matched(self->file_listbox, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    gtk_widget_set_visible(GTK_WIDGET(self->file_listbox), FALSE);

    /* Invalidate preview cache on full list reload */
    if (self->preview_cached_pixbuf) {
        g_object_unref(self->preview_cached_pixbuf);
        self->preview_cached_pixbuf = NULL;
    }
    g_free(self->preview_cached_path);
    self->preview_cached_path = NULL;

    /* Remove existing rows cleanly. */
    clear_listbox_safely(GTK_LIST_BOX(self->file_listbox));

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
        stampy_check_horizontal_photos(self);
        return;
    }

    total = self->image_files->len;

    if (total <= 200) {
        for (i = 0; i < total; i++) {
            GFile *file = g_ptr_array_index(self->image_files, i);
            if (file) append_file_row(self, file);
        }
        finish_list_population(self);
        return;
    }

    /* Large list: append in chunks on the main loop. */
    {
        PopulateTask *task = g_new0(PopulateTask, 1);

        task->self = g_object_ref(self);
        task->generation = self->populate_generation;
        task->total = total;
        if (self->left_stack) {
            if (gtk_stack_get_child_by_name(GTK_STACK(self->left_stack), "page_list")) {
                gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "page_list");
            }
        }
        self->populate_source_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                   populate_chunk_cb, task,
                                                   populate_task_free);
    }
}

static void on_listbox_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
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

    if (!self->listbox_popover) {
        GtkWidget *parent_widget = gtk_widget_get_parent(GTK_WIDGET(self->file_listbox));
        GMenu *context_menu = g_menu_new();

        g_menu_append(context_menu, "Add Images", "win.add-images");
        g_menu_append(context_menu, "Select All", "win.select-all");
        g_menu_append(context_menu, "Deselect All", "win.deselect-all");
        g_menu_append(context_menu, "Remove Selected", "win.remove-selected");

        self->listbox_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(context_menu));
        gtk_widget_set_parent(self->listbox_popover, parent_widget ? parent_widget : GTK_WIDGET(self));
        g_object_unref(context_menu);
    }

    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(self->listbox_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(self->listbox_popover));
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

    /* The loaded set no longer matches the cached folder tree. */
    g_clear_pointer(&self->scan_cache_sources, g_ptr_array_unref);
    g_clear_pointer(&self->scan_cache_dirs, g_hash_table_unref);

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

    /* Lock state is shown by the button icon. */
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

    g_clear_object(&self->default_export_folder);
    g_clear_object(&self->import_root);
    g_clear_pointer(&self->scan_cache_sources, g_ptr_array_unref);
    g_clear_pointer(&self->scan_cache_dirs, g_hash_table_unref);
    if (self->btn_header_export)
        gtk_widget_set_tooltip_text(self->btn_header_export, "Export Final Output");

    if (!self->compass_locked) {
        g_clear_object(&self->file_compass);
        if (self->lbl_compass) gtk_label_set_text(GTK_LABEL(self->lbl_compass), "Add Compass");
        if (self->btn_compass_lock) {
            self->compass_locked = FALSE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock), FALSE);
        }
        /* Also forget the remembered folder (it was not locked). */
        stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER, "");
    }

    g_clear_object(&self->file_watermark);
    g_clear_object(&self->file_geolocation);
    stampy_invalidate_docx_cache(self);

    if (self->banner_critical_coords) {
        adw_banner_set_revealed(ADW_BANNER(self->banner_critical_coords), FALSE);
    }
    g_clear_pointer(&self->critical_coords_details, g_free);

    if (self->lbl_watermark) gtk_label_set_text(GTK_LABEL(self->lbl_watermark), "Add Watermark(.docx)");
    if (self->lbl_geolocation) gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), "Add Geolocation(.xlsx)");

    stampy_populate_file_listbox(self);
    stampy_check_missing_data(self);

    if (self->img_preview) {
        gtk_picture_set_paintable(GTK_PICTURE(self->img_preview), NULL);
    }

    stampy_show_toast(self, "All files removed!");
    stampy_append_log(self, "[INFO] Removed all images, compass, watermark, and geolocation data.");
    stampy_update_clear_button_visibility(self);
}

/* Shows the "What's New" dialog. */
#define STAMPY_RELEASE_DATE "2026-10-20"

static GtkWidget *whats_new_bullet(const char *text) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *dot = gtk_label_new("•");
    GtkWidget *body = gtk_label_new(text);

    gtk_widget_set_valign(dot, GTK_ALIGN_START);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0);
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(row), dot);
    gtk_box_append(GTK_BOX(row), body);
    return row;
}

static GtkWidget *whats_new_heading(const char *text) {
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

static void on_whats_new_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    adw_dialog_close(ADW_DIALOG(user_data));
}

static void on_whats_new_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    GtkWidget *box, *topbar, *title, *subtitle, *scroll, *content, *close_btn;
    g_autofree char *title_text = NULL;

    (void)action; (void)parameter;
    if (!self) return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "What's New");
    adw_dialog_set_content_width(dialog, 480);
    adw_dialog_set_content_height(dialog, 560);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    adw_dialog_set_child(dialog, box);

    topbar = gtk_center_box_new();
    gtk_box_append(GTK_BOX(box), topbar);

    title_text = g_strdup_printf("What's New in %s?", PACKAGE_VERSION);
    title = gtk_label_new(title_text);
    gtk_widget_add_css_class(title, "title-2");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(topbar), title);

    close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_add_css_class(close_btn, "circular");
    gtk_widget_set_tooltip_text(close_btn, "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_whats_new_close_clicked), dialog);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(topbar), close_btn);

    subtitle = gtk_label_new("Released on " STAMPY_RELEASE_DATE);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.5);
    gtk_box_append(GTK_BOX(box), subtitle);

    scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(box), scroll);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    gtk_box_append(GTK_BOX(content), whats_new_heading("Increments support"));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Added support for increments."));
    gtk_box_append(GTK_BOX(content), whats_new_heading("Quality of life"));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Quality-of-life improvements across the app."));
    gtk_box_append(GTK_BOX(content), whats_new_heading("Fixes"));
    gtk_box_append(GTK_BOX(content), whats_new_bullet("Various bug fixes."));

    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* Keyboard shortcut action callbacks. */
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

    /* Register the "What's New" action. */
    act_whats_new = g_simple_action_new("whats-new", NULL);
    g_signal_connect(act_whats_new, "activate", G_CALLBACK(on_whats_new_activated), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(act_whats_new));

    /* Keyboard shortcut actions. */
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
