#include "stampy-quick-window.h"
#include "stampy-window-private.h"
#include <string.h>

typedef enum {
    QUICK_BANNER_NONE = 0,
    QUICK_BANNER_CRITICAL_COORDS,
    QUICK_BANNER_CRITICAL_DOCX,
    QUICK_BANNER_MISSING_BOTH,
    QUICK_BANNER_MISSING_DOCX,
    QUICK_BANNER_MISSING_XLSX,
    QUICK_BANNER_MISSING_DATA
} QuickBannerMode;

struct _StampyQuickWindow {
    AdwApplicationWindow parent_instance;

    GtkWidget *btn_cancel;
    GtkWidget *btn_export;
    GtkWidget *progress_bar;
    GtkWidget *banner_warning;
    GtkWidget *toast_overlay;
    GtkWidget *row_source;
    GtkWidget *img_source;
    GtkWidget *spinner_scan;
    GtkWidget *row_options;
    GtkWidget *row_preset;
    GtkWidget *row_auto_stamped;
    GtkWidget *row_compass;
    GtkWidget *btn_pick_compass;
    GtkWidget *row_watermark;
    GtkWidget *btn_pick_watermark;
    GtkWidget *row_geolocation;
    GtkWidget *btn_pick_geolocation;

    StampyWindow *ctrl;
    gboolean scan_finished;
    guint image_count;
    gboolean syncing_preset;
    QuickBannerMode banner_mode;
};

G_DEFINE_TYPE(StampyQuickWindow, stampy_quick_window, ADW_TYPE_APPLICATION_WINDOW)

static void set_banner_theme(StampyQuickWindow *self, gboolean critical) {
    AdwStyleManager *manager;
    gboolean dark;

    if (!self || !self->banner_warning) return;

    if (critical) {
        gtk_widget_set_name(self->banner_warning, "stampy_banner_red");
        return;
    }

    manager = adw_style_manager_get_default();
    dark = manager ? adw_style_manager_get_dark(manager) : FALSE;
    gtk_widget_set_name(self->banner_warning,
                        dark ? "stampy_banner_dark" : "stampy_banner_light");
}

static void refresh_quick_ui(StampyQuickWindow *self) {
    const char *preset_name = "Default Preset";
    g_autofree char *compass_base = NULL;
    g_autofree char *compass_path = NULL;
    g_autofree char *wm_base = NULL;
    g_autofree char *geo_base = NULL;
    g_autofree char *opt_sub = NULL;
    gboolean auto_stamped;

    if (!self || !self->ctrl) return;

    auto_stamped = self->ctrl->has_quick_auto_stamped
        ? self->ctrl->quick_auto_stamped
        : stampy_config_get_bool(STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER, TRUE);

    if (self->ctrl->drop_presets) {
        GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->ctrl->drop_presets));
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->ctrl->drop_presets));

        if (model && sel != GTK_INVALID_LIST_POSITION && GTK_IS_STRING_LIST(model)) {
            const char *fetched = gtk_string_list_get_string(GTK_STRING_LIST(model), sel);
            if (fetched && *fetched) preset_name = fetched;
        }

        if (self->row_preset && adw_combo_row_get_selected(ADW_COMBO_ROW(self->row_preset)) != sel) {
            self->syncing_preset = TRUE;
            adw_combo_row_set_selected(ADW_COMBO_ROW(self->row_preset), sel);
            self->syncing_preset = FALSE;
        }
    }

    if (self->ctrl->file_compass) {
        compass_path = g_file_get_path(self->ctrl->file_compass);
        compass_base = g_file_get_basename(self->ctrl->file_compass);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_compass),
                                    compass_path ? compass_path : (compass_base ? compass_base : "Selected"));
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_compass), "Not set");
    }

    opt_sub = g_strdup_printf("%s • %s • %s",
                              preset_name,
                              compass_base ? compass_base : "No compass",
                              auto_stamped ? "_stamped" : "Ask folder");
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(self->row_options), opt_sub);

    if (self->ctrl->file_watermark) {
        wm_base = g_file_get_basename(self->ctrl->file_watermark);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_watermark),
                                    wm_base ? wm_base : "Loaded");
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_watermark), "Missing");
    }

    if (self->ctrl->file_geolocation) {
        geo_base = g_file_get_basename(self->ctrl->file_geolocation);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_geolocation),
                                    geo_base ? geo_base : "Loaded");
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_geolocation), "Missing");
    }

    if (self->scan_finished) {
        gboolean crit_coords = self->ctrl->banner_critical_coords &&
                               adw_banner_get_revealed(ADW_BANNER(self->ctrl->banner_critical_coords));
        gboolean crit_docx = self->ctrl->banner_critical_docx &&
                             adw_banner_get_revealed(ADW_BANNER(self->ctrl->banner_critical_docx));
        gboolean missing_wm = (self->ctrl->file_watermark == NULL);
        gboolean missing_geo = (self->ctrl->file_geolocation == NULL);
        gboolean missing_data = self->ctrl->banner_missing_data &&
                                adw_banner_get_revealed(ADW_BANNER(self->ctrl->banner_missing_data));

        if (crit_coords) {
            self->banner_mode = QUICK_BANNER_CRITICAL_COORDS;
            set_banner_theme(self, TRUE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 adw_banner_get_title(ADW_BANNER(self->ctrl->banner_critical_coords)));
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), "See Details");
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else if (crit_docx) {
            self->banner_mode = QUICK_BANNER_CRITICAL_DOCX;
            set_banner_theme(self, TRUE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 adw_banner_get_title(ADW_BANNER(self->ctrl->banner_critical_docx)));
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), "See Details");
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else if (missing_wm && missing_geo) {
            self->banner_mode = QUICK_BANNER_MISSING_BOTH;
            set_banner_theme(self, FALSE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 "Missing .docx and .xlsx files");
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), "Options");
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else if (missing_wm) {
            self->banner_mode = QUICK_BANNER_MISSING_DOCX;
            set_banner_theme(self, FALSE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 "Missing watermark (.docx) file");
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), "Select DOCX");
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else if (missing_geo) {
            self->banner_mode = QUICK_BANNER_MISSING_XLSX;
            set_banner_theme(self, FALSE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 "Missing geolocation (.xlsx) file");
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), "Select XLSX");
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else if (missing_data) {
            const char *btn_lbl = adw_banner_get_button_label(ADW_BANNER(self->ctrl->banner_missing_data));
            const char *hig_lbl = "See Details";
            if (g_strcmp0(btn_lbl, "See more") == 0) hig_lbl = "See More";
            self->banner_mode = QUICK_BANNER_MISSING_DATA;
            set_banner_theme(self, FALSE);
            adw_banner_set_title(ADW_BANNER(self->banner_warning),
                                 adw_banner_get_title(ADW_BANNER(self->ctrl->banner_missing_data)));
            adw_banner_set_button_label(ADW_BANNER(self->banner_warning), hig_lbl);
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), TRUE);
        } else {
            self->banner_mode = QUICK_BANNER_NONE;
            adw_banner_set_revealed(ADW_BANNER(self->banner_warning), FALSE);
        }
    }

    gtk_widget_set_sensitive(self->btn_export,
                             self->scan_finished && !self->ctrl->export_active && self->image_count > 0);
}

static void on_style_dark_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)object;
    (void)pspec;
    if (!self) return;
    if (self->banner_mode != QUICK_BANNER_CRITICAL_COORDS &&
        self->banner_mode != QUICK_BANNER_CRITICAL_DOCX) {
        set_banner_theme(self, FALSE);
    }
}

static void on_quick_scan(StampyWindow *ctrl, guint image_count, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)ctrl;

    if (!self) return;

    self->scan_finished = TRUE;
    self->image_count = image_count;

    gtk_spinner_stop(GTK_SPINNER(self->spinner_scan));
    gtk_widget_set_visible(self->spinner_scan, FALSE);

    if (image_count == 0) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), "No supported photos found");
    } else {
        g_autofree char *sub = g_strdup_printf("%u photo%s ready to stamp",
                                               image_count,
                                               image_count == 1 ? "" : "s");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), sub);
    }

    refresh_quick_ui(self);
}

static void on_quick_banner(StampyWindow *ctrl, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)ctrl;
    if (!self) return;
    refresh_quick_ui(self);
}

static void on_quick_progress(StampyWindow *ctrl, double fraction, gboolean visible, const char *message, gboolean is_finished, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)ctrl;

    if (!self) return;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
    gtk_widget_set_visible(self->progress_bar, visible);

    if (visible) {
        gtk_widget_set_sensitive(self->btn_export, FALSE);
        gtk_widget_set_sensitive(self->row_options, FALSE);
        if (message && strstr(message, "photos processed") != NULL) {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), message);
        } else {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), "Exporting photos…");
        }
    }

    if (is_finished) {
        gtk_widget_set_visible(self->progress_bar, FALSE);
        gtk_widget_set_sensitive(self->row_options, TRUE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), "Export finished");
    }
}

static void on_quick_finish(StampyWindow *ctrl, AdwDialog *alert, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)ctrl;

    if (!self) return;
    if (alert) {
        g_signal_connect_object(alert, "closed", G_CALLBACK(gtk_window_close), self, G_CONNECT_SWAPPED);
    } else {
        gtk_window_close(GTK_WINDOW(self));
    }
}

static void on_quick_toast(StampyWindow *ctrl, const char *title, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)ctrl;

    if (!self || !self->scan_finished || !self->toast_overlay || !title) return;
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), adw_toast_new(title));
}

static void on_preset_row_selected(GObject *object, GParamSpec *pspec, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    guint sel;

    (void)object;
    (void)pspec;
    if (!self || self->syncing_preset || !self->ctrl || !self->ctrl->drop_presets) return;

    sel = adw_combo_row_get_selected(ADW_COMBO_ROW(self->row_preset));
    if (sel != GTK_INVALID_LIST_POSITION) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->ctrl->drop_presets), sel);
        refresh_quick_ui(self);
    }
}

static void on_auto_stamped_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    gboolean active;

    (void)pspec;
    if (!self || self->syncing_preset || !self->ctrl) return;

    active = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    self->ctrl->has_quick_auto_stamped = TRUE;
    self->ctrl->quick_auto_stamped = active;
    refresh_quick_ui(self);
}

static void on_banner_button_clicked(AdwBanner *banner, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);

    if (!self || !self->ctrl) return;

    switch (self->banner_mode) {
    case QUICK_BANNER_CRITICAL_COORDS:
        stampy_on_critical_coords_banner_clicked(banner, self->ctrl);
        break;
    case QUICK_BANNER_CRITICAL_DOCX:
        stampy_on_critical_docx_banner_clicked(banner, self->ctrl);
        break;
    case QUICK_BANNER_MISSING_BOTH:
        adw_expander_row_set_expanded(ADW_EXPANDER_ROW(self->row_options), TRUE);
        break;
    case QUICK_BANNER_MISSING_DOCX:
        stampy_open_watermark_picker(self->ctrl);
        break;
    case QUICK_BANNER_MISSING_XLSX:
        stampy_open_geolocation_picker(self->ctrl);
        break;
    case QUICK_BANNER_MISSING_DATA:
        stampy_on_missing_data_banner_clicked(banner, self->ctrl);
        break;
    case QUICK_BANNER_NONE:
    default:
        break;
    }
}

static void on_pick_compass_clicked(GtkButton *btn, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)btn;
    if (self && self->ctrl) stampy_open_compass_picker(self->ctrl);
}

static void on_pick_watermark_clicked(GtkButton *btn, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)btn;
    if (self && self->ctrl) stampy_open_watermark_picker(self->ctrl);
}

static void on_pick_geolocation_clicked(GtkButton *btn, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)btn;
    if (self && self->ctrl) stampy_open_geolocation_picker(self->ctrl);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)btn;
    if (self) gtk_window_close(GTK_WINDOW(self));
}

static void on_export_clicked(GtkButton *btn, gpointer user_data) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(user_data);
    (void)btn;

    if (!self || !self->ctrl || !self->scan_finished || self->image_count == 0) return;

    stampy_start_export(self->ctrl);
    if (self->ctrl->export_active) {
        gtk_widget_set_sensitive(self->btn_export, FALSE);
        gtk_widget_set_sensitive(self->row_options, FALSE);
        gtk_widget_set_visible(self->progress_bar, TRUE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->row_source), "Exporting photos…");
    }
}

static void detach_controller(StampyQuickWindow *self) {
    StampyWindow *ctrl;

    if (!self || !self->ctrl) return;

    ctrl = self->ctrl;
    g_object_remove_weak_pointer(G_OBJECT(ctrl), (gpointer *)&self->ctrl);
    self->ctrl = NULL;

    ctrl->dialog_parent = NULL;
    ctrl->quick_scan_cb = NULL;
    ctrl->quick_banner_cb = NULL;
    ctrl->quick_progress_cb = NULL;
    ctrl->quick_finish_cb = NULL;
    ctrl->quick_toast_cb = NULL;
    ctrl->quick_cb_data = NULL;

    if (ctrl->export_active) {
        ctrl->close_after_export = TRUE;
        stampy_cancel_export(ctrl);
    } else {
        gtk_window_destroy(GTK_WINDOW(ctrl));
    }
}

static gboolean on_quick_close_request(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    detach_controller(STAMPY_QUICK_WINDOW(window));
    return FALSE;
}

static gboolean on_escape_shortcut(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)args;
    (void)user_data;
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
}

static void stampy_quick_window_dispose(GObject *object) {
    StampyQuickWindow *self = STAMPY_QUICK_WINDOW(object);

    detach_controller(self);
    gtk_widget_dispose_template(GTK_WIDGET(self), STAMPY_TYPE_QUICK_WINDOW);

    G_OBJECT_CLASS(stampy_quick_window_parent_class)->dispose(object);
}

static void stampy_quick_window_class_init(StampyQuickWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = stampy_quick_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/com/cobaltty/stampy/stampy-quick-window.ui");

    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, btn_cancel);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, btn_export);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, progress_bar);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, banner_warning);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_source);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, img_source);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, spinner_scan);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_options);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_auto_stamped);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_compass);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, btn_pick_compass);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_watermark);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, btn_pick_watermark);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, row_geolocation);
    gtk_widget_class_bind_template_child(widget_class, StampyQuickWindow, btn_pick_geolocation);
}

static void stampy_quick_window_init(StampyQuickWindow *self) {
    GtkShortcutController *sc;
    GtkShortcut *esc;

    gtk_widget_init_template(GTK_WIDGET(self));

    g_signal_connect(self, "close-request", G_CALLBACK(on_quick_close_request), NULL);
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
    g_signal_connect(self->btn_export, "clicked", G_CALLBACK(on_export_clicked), self);
    g_signal_connect(self->banner_warning, "button-clicked", G_CALLBACK(on_banner_button_clicked), self);
    g_signal_connect(self->row_preset, "notify::selected", G_CALLBACK(on_preset_row_selected), self);
    g_signal_connect(self->row_auto_stamped, "notify::active", G_CALLBACK(on_auto_stamped_changed), self);
    g_signal_connect(self->btn_pick_compass, "clicked", G_CALLBACK(on_pick_compass_clicked), self);
    g_signal_connect(self->btn_pick_watermark, "clicked", G_CALLBACK(on_pick_watermark_clicked), self);
    g_signal_connect(self->btn_pick_geolocation, "clicked", G_CALLBACK(on_pick_geolocation_clicked), self);

    g_signal_connect_object(adw_style_manager_get_default(), "notify::dark",
                            G_CALLBACK(on_style_dark_changed), self, 0);

    sc = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    esc = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                           gtk_callback_action_new(on_escape_shortcut, NULL, NULL));
    gtk_shortcut_controller_add_shortcut(sc, esc);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(sc));
}

StampyQuickWindow *stampy_quick_window_new(GtkApplication *app, GSList *files) {
    StampyQuickWindow *self;
    guint n_files;

    g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);

    self = g_object_new(STAMPY_TYPE_QUICK_WINDOW,
                        "application", app,
                        NULL);

    self->ctrl = stampy_window_new_quick(app);
    g_object_add_weak_pointer(G_OBJECT(self->ctrl), (gpointer *)&self->ctrl);
    self->ctrl->dialog_parent = GTK_WINDOW(self);
    self->ctrl->quick_scan_cb = on_quick_scan;
    self->ctrl->quick_banner_cb = on_quick_banner;
    self->ctrl->quick_progress_cb = on_quick_progress;
    self->ctrl->quick_finish_cb = on_quick_finish;
    self->ctrl->quick_toast_cb = on_quick_toast;
    self->ctrl->quick_cb_data = self;

    if (self->ctrl->drop_presets) {
        GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->ctrl->drop_presets));
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->ctrl->drop_presets));

        self->syncing_preset = TRUE;
        adw_combo_row_set_model(ADW_COMBO_ROW(self->row_preset), model);
        if (sel != GTK_INVALID_LIST_POSITION) {
            adw_combo_row_set_selected(ADW_COMBO_ROW(self->row_preset), sel);
        }
        self->syncing_preset = FALSE;
    }

    self->syncing_preset = TRUE;
    adw_switch_row_set_active(ADW_SWITCH_ROW(self->row_auto_stamped),
                              stampy_config_get_bool(STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER, TRUE));
    self->syncing_preset = FALSE;

    n_files = g_slist_length(files);
    if (n_files == 1 && files->data) {
        GFile *f = G_FILE(files->data);
        GFileType ftype = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
        g_autofree char *base = g_file_get_basename(f);

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_source),
                                      base ? base : "Selected Item");
        gtk_image_set_from_icon_name(GTK_IMAGE(self->img_source),
                                     ftype == G_FILE_TYPE_DIRECTORY ? "folder-symbolic" : "image-x-generic-symbolic");
    } else if (n_files > 1) {
        g_autofree char *title = g_strdup_printf("%u selected items", n_files);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_source), title);
        gtk_image_set_from_icon_name(GTK_IMAGE(self->img_source), "image-x-generic-symbolic");
    }

    refresh_quick_ui(self);

    if (files) {
        stampy_add_image_files(self->ctrl, files);
    } else {
        on_quick_scan(self->ctrl, 0, self);
    }

    return self;
}
