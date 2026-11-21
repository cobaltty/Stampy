#include "stampy-window-private.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include <unistd.h>

void stampy_show_toast(StampyWindow *self, const char *title) {
    AdwToast *toast;
    if (!self || !title) return;
    if (self->quick_toast_cb) {
        self->quick_toast_cb(self, title, self->quick_cb_data);
        return;
    }
    if (!self->toast_overlay) return;
    toast = adw_toast_new(title);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
}

void stampy_set_progress(StampyWindow *self, double fraction, gboolean visible) {
    if (!self || !self->progress_bar) return;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
    gtk_widget_set_visible(self->progress_bar, visible);
}

static void apply_custom_css(void) {
    static gboolean css_applied = FALSE;
    GdkDisplay *display;
    GtkCssProvider *provider;
    const char *css =
        "progressbar > trough {"
        "  min-height: 4px;"
        "}"
        "progressbar > trough > progress {"
        "  min-height: 4px;"
        "  background-color: @accent_color;"
        "  border-radius: 2px;"
        "  transition: min-width 100ms cubic-bezier(0, 0, 0.2, 1);"
        "}"
        "banner#stampy_banner_light > revealer > widget {"
        "  background-color: #f9f06b;"
        "  color: #1a1a1a;"
        "}"
        "banner#stampy_banner_dark > revealer > widget {"
        "  background-color: #e5a50a;"
        "  color: #1a1a1a;"
        "}"
        "banner#stampy_banner_red > revealer > widget {"
        "  background-color: #c01c28;"
        "  color: #ffffff;"
        "}"
        "banner#stampy_banner_red > revealer > widget button {"
        "  color: #ffffff;"
        "}"
        ".missing-badge, label.missing-badge {"
        "  padding: 2px 10px;"
        "  border-radius: 999px;"
        "  font-size: 0.85em;"
        "  font-weight: 600;"
        "  background-color: alpha(@error_color, 0.18);"
        "  color: @error_color;"
        "}";

    if (css_applied) return;
    display = gdk_display_get_default();
    if (!display) return;

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
    css_applied = TRUE;
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(window);
    (void)user_data;

    if (self->landscape_cancel) {
        g_cancellable_cancel(self->landscape_cancel);
        g_clear_object(&self->landscape_cancel);
    }

    if (self->scan_cancel) {
        g_cancellable_cancel(self->scan_cancel);
        g_clear_object(&self->scan_cancel);
    }

    if (self->scan_pulse_id > 0) {
        g_source_remove(self->scan_pulse_id);
        self->scan_pulse_id = 0;
    }

    if (self->populate_source_id > 0) {
        g_source_remove(self->populate_source_id);
        self->populate_source_id = 0;
    }

    if (self->save_state_timeout_id > 0) {
        g_source_remove(self->save_state_timeout_id);
        self->save_state_timeout_id = 0;
    }

    if (!self->quick_mode) {
        stampy_save_last_state(self);
    }

    if (self->export_active) {
        self->close_after_export = TRUE;
        stampy_cancel_export(self);
        gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
        return TRUE;
    }

    return FALSE;
}

enum {
    PROP_0,
    PROP_QUICK_MODE,
    N_PROPS
};

static GParamSpec *obj_properties[N_PROPS] = { NULL, };

G_DEFINE_TYPE(StampyWindow, stampy_window, ADW_TYPE_APPLICATION_WINDOW)

static void stampy_window_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    StampyWindow *self = STAMPY_WINDOW(object);

    switch (prop_id) {
    case PROP_QUICK_MODE:
        self->quick_mode = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void stampy_window_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    StampyWindow *self = STAMPY_WINDOW(object);

    switch (prop_id) {
    case PROP_QUICK_MODE:
        g_value_set_boolean(value, self->quick_mode);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void stampy_window_constructed(GObject *object) {
    StampyWindow *self = STAMPY_WINDOW(object);

    G_OBJECT_CLASS(stampy_window_parent_class)->constructed(object);

    /* Load presets and last state after construct-only properties are set;
       saves stay suppressed until done. */
    stampy_scan_and_populate_presets(self);
    stampy_load_last_state(self);
    self->loading_state = FALSE;
}

static void stampy_window_dispose(GObject *object) {
    StampyWindow *self = STAMPY_WINDOW(object);

    if (self->save_state_timeout_id > 0) {
        g_source_remove(self->save_state_timeout_id);
        self->save_state_timeout_id = 0;
        if (!self->quick_mode && !self->disposed) {
            stampy_save_last_state(self);
        }
    }

    self->disposed = TRUE;
    self->scan_generation++;
    self->landscape_generation++;
    self->populate_generation++;

    self->dialog_parent = NULL;
    self->active_hover_popover = NULL;
    self->quick_scan_cb = NULL;
    self->quick_banner_cb = NULL;
    self->quick_progress_cb = NULL;
    self->quick_finish_cb = NULL;
    self->quick_toast_cb = NULL;
    self->quick_cb_data = NULL;

    if (self->scan_cancel) {
        g_cancellable_cancel(self->scan_cancel);
        g_clear_object(&self->scan_cancel);
    }

    if (self->landscape_cancel) {
        g_cancellable_cancel(self->landscape_cancel);
        g_clear_object(&self->landscape_cancel);
    }

    if (self->scan_pulse_id > 0) {
        g_source_remove(self->scan_pulse_id);
        self->scan_pulse_id = 0;
    }

    if (self->populate_source_id > 0) {
        g_source_remove(self->populate_source_id);
        self->populate_source_id = 0;
    }

    stampy_cancel_export(self);

    g_clear_object(&self->file_compass);
    if (!self->export_active) {
        g_clear_object(&self->file_watermark);
        g_clear_object(&self->file_geolocation);
    }
    g_clear_object(&self->preview_cached_pixbuf);
    g_clear_object(&self->default_export_folder);
    g_clear_object(&self->import_root);

    if (self->listbox_popover) {
        gtk_widget_unparent(self->listbox_popover);
        self->listbox_popover = NULL;
    }

    gtk_widget_dispose_template(GTK_WIDGET(self), STAMPY_TYPE_WINDOW);

    G_OBJECT_CLASS(stampy_window_parent_class)->dispose(object);
}

static void stampy_window_finalize(GObject *object) {
    StampyWindow *self = STAMPY_WINDOW(object);

    g_clear_object(&self->file_watermark);
    g_clear_object(&self->file_geolocation);

    g_clear_pointer(&self->geo_table, g_hash_table_destroy);
    g_clear_pointer(&self->image_files, g_ptr_array_unref);
    g_clear_pointer(&self->preview_cached_path, g_free);
    g_clear_pointer(&self->horizontal_photos, g_ptr_array_unref);
    g_clear_pointer(&self->missing_data_items, g_ptr_array_unref);
    g_clear_pointer(&self->critical_coords_details, g_free);
    g_clear_pointer(&self->critical_docx_details, g_free);
    g_clear_pointer(&self->docx_cached_lines, g_strfreev);
    g_clear_pointer(&self->docx_table, g_hash_table_destroy);
    g_clear_pointer(&self->docx_cached_path, g_free);
    g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
    g_clear_pointer(&self->selected_pu_folder, g_free);
    g_clear_pointer(&self->scan_cache_sources, g_ptr_array_unref);
    g_clear_pointer(&self->scan_cache_dirs, g_hash_table_unref);
    g_clear_pointer(&self->quick_meta.meta_strip_text, g_free);

    G_OBJECT_CLASS(stampy_window_parent_class)->finalize(object);
}

static void stampy_window_class_init(StampyWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->set_property = stampy_window_set_property;
    object_class->get_property = stampy_window_get_property;
    object_class->constructed = stampy_window_constructed;
    object_class->dispose = stampy_window_dispose;
    object_class->finalize = stampy_window_finalize;

    obj_properties[PROP_QUICK_MODE] = g_param_spec_boolean(
        "quick-mode",
        "Quick Mode",
        "Run as an unpresented Quick-Stamp controller",
        FALSE,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, N_PROPS, obj_properties);

    gtk_widget_class_set_template_from_resource(widget_class, "/com/cobaltty/stampy/stampy-window.ui");

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, progress_bar);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_missing_data);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_critical_coords);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_critical_docx);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, left_stack);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_area_frame);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, box_pu_selector);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_pu_folder);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, file_listbox);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_compass);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_compass_lock);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_watermark);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_geolocation);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, lbl_compass);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, lbl_watermark);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, lbl_geolocation);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, box_right_panel);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_compass_pos);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_compass_size);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_compass_margin_x);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_compass_margin_y);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_compass_opacity);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_c_shadow);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_c_stroke);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_c_stroke_color);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_text_pos);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_text_font);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_size);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_margin_x);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_margin_y);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_opacity);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_align_left);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_align_center);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_align_right);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_bold);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_italic);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_underline);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_shadow);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_stroke);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_stroke_color);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_t_color);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_presets);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_add_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_remove_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_edit_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_apply_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_import_preset);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_export_preset);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_menu);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_header_clear);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_header_export);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_header_cancel);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, preview_stack);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, img_preview);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, txt_terminal_output);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_spacing);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, img_decoration);
}

/* Plays the hidden audio clip through a minimal GStreamer pipeline. */
#include <gst/gst.h>

typedef struct {
    GstElement *pipeline;
    guint bus_watch_id;
    char *tmp_path;
} TetoPlayer;

static gboolean on_teto_bus(GstBus *bus, GstMessage *msg, gpointer user_data) {
    TetoPlayer *player = (TetoPlayer *)user_data;
    (void)bus;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        char *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("[Teto] GStreamer error: %s\n", err ? err->message : "unknown");
        if (dbg) g_printerr("[Teto] debug: %s\n", dbg);
        g_clear_error(&err);
        g_free(dbg);
        gst_element_set_state(player->pipeline, GST_STATE_NULL);
        gst_object_unref(player->pipeline);
        g_free(player->tmp_path);
        g_free(player);
        return FALSE;
    }
    case GST_MESSAGE_EOS:
        gst_element_set_state(player->pipeline, GST_STATE_NULL);
        gst_object_unref(player->pipeline);
        g_free(player->tmp_path);
        g_free(player);
        return FALSE;
    default:
        break;
    }
#pragma GCC diagnostic pop
    return TRUE;
}

static void stampy_play_teto(StampyWindow *self) {
    GBytes *bytes;
    char *tmp_path;
    char *desc;
    int fd;
    gsize size;
    const void *data;
    GError *err = NULL;
    GstBus *bus;
    TetoPlayer *player;

    if (!self) return;

    bytes = g_resources_lookup_data("/com/cobaltty/stampy/teto.mp3", 0, NULL);
    if (!bytes) return;

    data = g_bytes_get_data(bytes, &size);
    tmp_path = g_build_filename(g_get_tmp_dir(), "stampy-teto-XXXXXX.mp3", NULL);
    fd = g_mkstemp_full(tmp_path, O_WRONLY | O_CREAT, 0600);
    if (fd < 0) { g_free(tmp_path); g_bytes_unref(bytes); return; }

    write(fd, data, size);
    close(fd);
    g_bytes_unref(bytes);

    desc = g_strdup_printf(
        "filesrc location=\"%s\" ! queue ! decodebin name=db "
        "db. ! audioconvert ! audioresample ! autoaudiosink",
        tmp_path);

    player = g_new0(TetoPlayer, 1);
    player->pipeline = gst_parse_launch(desc, &err);
    g_free(desc);

    if (err || !player->pipeline) {
        g_printerr("[Teto] Pipeline error: %s\n", err ? err->message : "unknown");
        g_clear_error(&err);
        g_free(player);
        g_free(tmp_path);
        return;
    }

    bus = gst_element_get_bus(player->pipeline);
    player->bus_watch_id = gst_bus_add_watch(bus, on_teto_bus, player);
    gst_object_unref(bus);

    player->tmp_path = tmp_path;
    gst_element_set_state(player->pipeline, GST_STATE_PLAYING);
}

static void stampy_on_compass_lock_toggled(GtkToggleButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    if (!self) return;
    self->compass_locked = gtk_toggle_button_get_active(btn);
    if (btn)
        gtk_button_set_icon_name(GTK_BUTTON(btn), self->compass_locked ? "padlock2-symbolic" : "padlock-open-symbolic");
    /* Persisted, restored on next launch. */
    if (!self->quick_mode && !self->loading_state)
        stampy_config_set_bool(STAMPY_CONFIG_KEY_COMPASS_LOCKED, self->compass_locked);
}

void stampy_toggle_compass_lock(StampyWindow *self) {
    if (!self || !self->btn_compass_lock) return;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock)));
}

/* Yellow banner: brighter in light mode, deeper amber in dark mode. */
static void update_banner_theme(StampyWindow *self) {
    AdwStyleManager *manager = adw_style_manager_get_default();
    gboolean dark;

    if (!self || !self->banner_missing_data || !manager) return;

    dark = adw_style_manager_get_dark(manager);
    if (self->banner_missing_data)
        gtk_widget_set_name(self->banner_missing_data,
                            dark ? "stampy_banner_dark" : "stampy_banner_light");
}

static void on_style_dark_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    update_banner_theme(STAMPY_WINDOW(user_data));
}

static void stampy_window_init(StampyWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));

    /* Suppress re-entrant saves for the WHOLE init sequence. This must be
     * set before any setup below: e.g. stampy_populate_system_fonts()
     * replaces a dropdown model which emits notify::selected, and without
     * this guard that would overwrite stampy_last.json with defaults
     * (wiping the saved preset selection) before it is ever loaded. */
    self->loading_state = TRUE;

    apply_custom_css();

    /* GtkBuilder ids are not CSS names; set a name the banner rules match. */
    update_banner_theme(self);
    if (self->banner_critical_coords)
        gtk_widget_set_name(self->banner_critical_coords, "stampy_banner_red");
    if (self->banner_critical_docx)
        gtk_widget_set_name(self->banner_critical_docx, "stampy_banner_red");
    g_signal_connect_object(adw_style_manager_get_default(), "notify::dark",
                            G_CALLBACK(on_style_dark_changed), self, 0);

    self->image_files = g_ptr_array_new_with_free_func(g_object_unref);
    self->geo_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request), NULL);



    if (self->btn_menu) {
        GMenu *menu_model = g_menu_new();
        g_menu_append(menu_model, "What's New", "win.whats-new");
        g_menu_append(menu_model, "Preferences", "win.preferences");
        g_menu_append(menu_model, "Keyboard Shortcuts", "app.shortcuts");
        g_menu_append(menu_model, "About Stampy", "app.about");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_menu), G_MENU_MODEL(menu_model));
        g_object_unref(menu_model);
    }

    stampy_setup_actions_and_menus(self);
    stampy_setup_file_handlers_and_dnd(self);
    stampy_connect_export_logic(self);

    stampy_setup_hover_effect_popovers(self);
    stampy_connect_panel_signals(self);
    stampy_populate_system_fonts(self);

    /* Compass lock button. */
    if (self->btn_compass_lock) {
        self->compass_locked = FALSE;
        gtk_button_set_icon_name(GTK_BUTTON(self->btn_compass_lock), "padlock-open-symbolic");
        g_signal_connect(self->btn_compass_lock, "toggled",
            G_CALLBACK(stampy_on_compass_lock_toggled), self);
    }

    /* Hidden audio clip on decoration click. */
    if (self->img_decoration) {
        GtkGesture *click = gtk_gesture_click_new();
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self->img_decoration), "pointer");
        g_signal_connect_swapped(click, "pressed", G_CALLBACK(stampy_play_teto), self);
        gtk_widget_add_controller(GTK_WIDGET(self->img_decoration), GTK_EVENT_CONTROLLER(click));
    }
}

StampyWindow *stampy_window_new(GtkApplication *app) {
    return g_object_new(STAMPY_TYPE_WINDOW, "application", app, NULL);
}

StampyWindow *stampy_window_new_quick(GtkApplication *app) {
    return g_object_new(STAMPY_TYPE_WINDOW,
                        "application", app,
                        "quick-mode", TRUE,
                        NULL);
}

gboolean stampy_window_is_quick_mode(StampyWindow *self) {
    g_return_val_if_fail(STAMPY_IS_WINDOW(self), FALSE);
    return self->quick_mode;
}
