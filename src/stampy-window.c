#include "stampy-window-private.h"
#include <gtk/gtk.h>

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
        ".custom-rounded-list {"
        "  border-radius: 12px;"
        "}"
        ".custom-rounded-list list,"
        ".custom-rounded-list row:not(:selected) {"
        "  background-color: transparent;"
        "}"
        ".custom-rounded-list row:first-child,"
        ".custom-rounded-list row:first-child:selected {"
        "  border-top-left-radius: 12px;"
        "  border-top-right-radius: 12px;"
        "}"
        ".custom-rounded-list row:last-child,"
        ".custom-rounded-list row:last-child:selected {"
        "  border-bottom-left-radius: 12px;"
        "  border-bottom-right-radius: 12px;"
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

    stampy_save_last_state(self);
    return FALSE;
}

G_DEFINE_TYPE(StampyWindow, stampy_window, ADW_TYPE_APPLICATION_WINDOW)

static void stampy_window_dispose(GObject *object) {
    StampyWindow *self = STAMPY_WINDOW(object);

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

    G_OBJECT_CLASS(stampy_window_parent_class)->dispose(object);
}

static void stampy_window_class_init(StampyWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = stampy_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/com/cobaltty/stampy/stampy-window.ui");

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, progress_bar);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, left_stack);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, drop_area_frame);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, file_listbox);

    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_compass);
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
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, btn_header_cancel); /* <-- BIND TOMBOL CANCEL */
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, preview_stack);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, img_preview);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, txt_terminal_output);
    gtk_widget_class_bind_template_child(widget_class, StampyWindow, val_text_spacing);
}

static void stampy_window_init(StampyWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));

    apply_custom_css();

    self->image_files = g_ptr_array_new_with_free_func(g_object_unref);
    self->geo_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request), NULL);



    if (self->btn_menu) {
        GMenu *menu_model = g_menu_new();
      g_menu_append(menu_model, "What's New", "win.whats-new"); /* <-- BARIS BARU */
        g_menu_append(menu_model, "About Stampy", "app.about");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_menu), G_MENU_MODEL(menu_model));
        g_object_unref(menu_model);
    }

    stampy_setup_actions_and_menus(self);
    stampy_setup_file_handlers_and_dnd(self);
    stampy_connect_export_logic(self);
    stampy_connect_panel_signals(self);
    stampy_populate_system_fonts(self);

    stampy_scan_and_populate_presets(self);
    stampy_load_last_state(self);
}

StampyWindow *stampy_window_new(GtkApplication *app) {
    return g_object_new(STAMPY_TYPE_WINDOW, "application", app, NULL);
}
