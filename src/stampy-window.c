#include "stampy-window-private.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include <unistd.h>

void stampy_show_toast(StampyWindow *self, const char *title) {
    AdwToast *toast;
    if (!self || !self->toast_overlay) return;
    toast = adw_toast_new(title);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
}

void stampy_set_progress(StampyWindow *self, double fraction, gboolean visible) {
    if (!self || !self->progress_bar) return;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
    gtk_widget_set_visible(self->progress_bar, visible);
}

static void apply_custom_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
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
        "}";

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(window);
    (void)user_data;

    stampy_cancel_export(self);

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

    stampy_save_last_state(self);
    return FALSE;
}

G_DEFINE_TYPE(StampyWindow, stampy_window, ADW_TYPE_APPLICATION_WINDOW)

static void stampy_window_dispose(GObject *object) {
    StampyWindow *self = STAMPY_WINDOW(object);

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

    if (self->geo_table) {
        g_hash_table_destroy(self->geo_table);
        self->geo_table = NULL;
    }

    if (self->image_files) {
        g_ptr_array_unref(self->image_files);
        self->image_files = NULL;
    }

    if (self->file_compass) {
        g_object_unref(self->file_compass);
        self->file_compass = NULL;
    }

    if (self->file_watermark) {
        g_object_unref(self->file_watermark);
        self->file_watermark = NULL;
    }

    if (self->file_geolocation) {
        g_object_unref(self->file_geolocation);
        self->file_geolocation = NULL;
    }

    if (self->save_state_timeout_id > 0) {
        g_source_remove(self->save_state_timeout_id);
        self->save_state_timeout_id = 0;
        stampy_save_last_state(self);
    }

    if (self->preview_cached_pixbuf) {
        g_object_unref(self->preview_cached_pixbuf);
        self->preview_cached_pixbuf = NULL;
    }

    if (self->preview_cached_path) {
        g_free(self->preview_cached_path);
        self->preview_cached_path = NULL;
    }

    if (self->landscape_cancel) {
        g_cancellable_cancel(self->landscape_cancel);
        g_clear_object(&self->landscape_cancel);
    }

    if (self->horizontal_photos) {
        g_ptr_array_unref(self->horizontal_photos);
        self->horizontal_photos = NULL;
    }

    if (self->missing_data_items) {
        g_ptr_array_unref(self->missing_data_items);
        self->missing_data_items = NULL;
    }

    if (self->critical_coords_details) {
        g_free(self->critical_coords_details);
        self->critical_coords_details = NULL;
    }

    if (self->docx_cached_lines) {
        g_strfreev(self->docx_cached_lines);
        self->docx_cached_lines = NULL;
    }

    if (self->docx_table) {
        g_hash_table_destroy(self->docx_table);
        self->docx_table = NULL;
    }

    if (self->docx_cached_path) {
        g_free(self->docx_cached_path);
        self->docx_cached_path = NULL;
    }

    if (self->default_export_folder) {
        g_object_unref(self->default_export_folder);
        self->default_export_folder = NULL;
    }

    g_clear_pointer(&self->pu_folders, g_ptr_array_unref);
    g_clear_pointer(&self->selected_pu_folder, g_free);

    if (self->import_root) {
        g_object_unref(self->import_root);
        self->import_root = NULL;
    }

    if (self->scan_cache_sources) {
        g_ptr_array_unref(self->scan_cache_sources);
        self->scan_cache_sources = NULL;
    }

    if (self->scan_cache_dirs) {
        g_hash_table_unref(self->scan_cache_dirs);
        self->scan_cache_dirs = NULL;
    }

    if (self->listbox_popover) {
        gtk_widget_unparent(self->listbox_popover);
        self->listbox_popover = NULL;
    }

    stampy_cancel_export(self);

    G_OBJECT_CLASS(stampy_window_parent_class)->dispose(object);
}

static void stampy_window_class_init(StampyWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = stampy_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/com/cobaltty/stampy/stampy-window.ui");

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, progress_bar);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_landscape);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_missing_data);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, banner_critical_coords);
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

    if (!self || !self->banner_landscape || !manager) return;

    dark = adw_style_manager_get_dark(manager);
    if (self->banner_landscape)
        gtk_widget_set_name(self->banner_landscape,
                            dark ? "stampy_banner_dark" : "stampy_banner_light");
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

    /* Load presets and last state; saves stay suppressed until done. */
    stampy_scan_and_populate_presets(self);
    stampy_load_last_state(self);
    self->loading_state = FALSE;
}

StampyWindow *stampy_window_new(GtkApplication *app) {
    return g_object_new(STAMPY_TYPE_WINDOW, "application", app, NULL);
}
