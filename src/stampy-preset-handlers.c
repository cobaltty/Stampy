#include "stampy-preset-handlers.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>

typedef struct {
    StampyWindow *self;
    GtkWidget *entry;
} PresetDialogData;

static const char *pos_index_to_string(guint idx) {
    switch (idx) {
        case 0: return "UpLeft";
        case 1: return "UpRight";
        case 2: return "DownLeft";
        case 3: return "DownRight";
        default: return "UpLeft";
    }
}

static guint pos_string_to_index(const char *str) {
    if (!str) return 0;
    if (strstr(str, "UpRight") || strstr(str, "top_right")) return 1;
    if (strstr(str, "DownLeft") || strstr(str, "bottom_left")) return 2;
    if (strstr(str, "DownRight") || strstr(str, "bottom_right")) return 3;
    return 0;
}

/* Creates the destination's parent directory if missing. */
static void ensure_parent_directory_exists(const char *filepath) {
    if (filepath) {
        g_autofree char *dirname = g_path_get_dirname(filepath);
        if (dirname) {
            g_mkdir_with_parents(dirname, 0755);
        }
    }
}

/* Preset discovery and file I/O. */
void stampy_scan_and_populate_presets(StampyWindow *self) {
    g_autofree char *config_dir = NULL;
    GDir *dir = NULL;
    const char *filename = NULL;
    GListModel *model = NULL;

    if (!self || !self->drop_presets) return;

    config_dir = g_build_filename(g_get_user_config_dir(), "stampy_presets", NULL);
    g_mkdir_with_parents(config_dir, 0755);

    model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_presets));
    if (!GTK_IS_STRING_LIST(model)) return;

    dir = g_dir_open(config_dir, 0, NULL);
    if (!dir) return;

    while ((filename = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(filename, ".json")) {
            g_autofree char *preset_name = g_strndup(filename, strlen(filename) - 5);
            gboolean exists = FALSE;
            guint n = g_list_model_get_n_items(model);
            guint i;

            for (i = 0; i < n; i++) {
                if (g_strcmp0(gtk_string_list_get_string(GTK_STRING_LIST(model), i), preset_name) == 0) {
                    exists = TRUE;
                    break;
                }
            }
            if (!exists) {
                gtk_string_list_append(GTK_STRING_LIST(model), preset_name);
            }
        }
    }
    g_dir_close(dir);
}

void stampy_save_preset_to_file(StampyWindow *self, const char *filepath) {
    const char *c_pos, *t_pos, *t_font = "Arial", *t_align;
    g_autofree char *json_data = NULL;
    g_autofree char *color_hex_c_stroke = NULL;
    g_autofree char *color_hex_c_shadow = NULL;
    g_autofree char *color_hex_t_stroke = NULL;
    g_autofree char *color_hex_t_shadow = NULL;
    g_autofree char *color_hex_t_color = NULL;
    GError *error = NULL;
    GdkRGBA rgba;
    guint c_idx, t_idx;
    double t_spacing = 12.0;
    double t_stroke_w = self->val_t_stroke_width ? gtk_range_get_value(GTK_RANGE(self->val_t_stroke_width)) : 2.5;
    double t_shadow_w = self->val_t_shadow_width ? gtk_range_get_value(GTK_RANGE(self->val_t_shadow_width)) : 2.0;
    double c_stroke_w = self->val_c_stroke_width ? gtk_range_get_value(GTK_RANGE(self->val_c_stroke_width)) : 2.0;
    double c_shadow_w = self->val_c_shadow_width ? gtk_range_get_value(GTK_RANGE(self->val_c_shadow_width)) : 4.0;

    if (!self || !filepath) return;

    /* The config directory must exist before writing. */
    ensure_parent_directory_exists(filepath);

    c_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_compass_pos));
    t_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_text_pos));
    if (c_idx == GTK_INVALID_LIST_POSITION) c_idx = 0;
    if (t_idx == GTK_INVALID_LIST_POSITION) t_idx = 0;

    c_pos = pos_index_to_string(c_idx);
    t_pos = pos_index_to_string(t_idx);

    /* Read the font name, guarding against invalid selection. */
    if (self->drop_text_font) {
        GListModel *font_model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_text_font));
        guint font_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_text_font));
        if (font_model && font_idx != GTK_INVALID_LIST_POSITION && GTK_IS_STRING_LIST(font_model)) {
            const char *fetched = gtk_string_list_get_string(GTK_STRING_LIST(font_model), font_idx);
            if (fetched && *fetched) t_font = fetched;
        }
    }

    t_align = "right";
    if (self->btn_t_align_left && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_align_left))) t_align = "left";
    else if (self->btn_t_align_center && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_align_center))) t_align = "center";

    if (self->val_text_spacing) {
        t_spacing = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_spacing));
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (self->btn_c_stroke_color) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_c_stroke_color), &rgba);
        color_hex_c_stroke = g_strdup_printf("#%02X%02X%02X", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
    } else {
        color_hex_c_stroke = g_strdup("#000000");
    }

    if (self->btn_c_shadow_color) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_c_shadow_color), &rgba);
        color_hex_c_shadow = g_strdup_printf("#%02X%02X%02X", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
    } else {
        color_hex_c_shadow = g_strdup("#000000");
    }

    if (self->btn_t_stroke_color) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_t_stroke_color), &rgba);
        color_hex_t_stroke = g_strdup_printf("#%02X%02X%02X", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
    } else {
        color_hex_t_stroke = g_strdup("#000000");
    }

    if (self->btn_t_shadow_color) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_t_shadow_color), &rgba);
        color_hex_t_shadow = g_strdup_printf("#%02X%02X%02X", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
    } else {
        color_hex_t_shadow = g_strdup("#000000");
    }

    if (self->btn_t_color) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(self->btn_t_color), &rgba);
        color_hex_t_color = g_strdup_printf("#%02X%02X%02X", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
    } else {
        color_hex_t_color = g_strdup("#FFFFFF");
    }
#pragma GCC diagnostic pop

    json_data = g_strdup_printf(
        "{\n"
        "  \"c_size\": %d,\n  \"c_margin_x\": %.1f,\n  \"c_margin_y\": %.1f,\n  \"c_opacity\": %d,\n  \"c_pos\": \"%s\",\n"
        "  \"c_shadow\": %s,\n  \"c_shadow_color\": \"%s\",\n  \"c_shadow_width\": %.1f,\n"
        "  \"c_stroke\": %s,\n  \"c_stroke_color\": \"%s\",\n  \"c_stroke_width\": %.1f,\n"
        "  \"t_size\": %.1f,\n  \"t_spacing\": %.1f,\n  \"t_margin_x\": %.1f,\n  \"t_margin_y\": %.1f,\n  \"t_opacity\": %d,\n  \"t_pos\": \"%s\",\n"
        "  \"t_font\": \"%s\",\n  \"t_align\": \"%s\",\n"
        "  \"t_bold\": %s,\n  \"t_italic\": %s,\n  \"t_underline\": %s,\n"
        "  \"t_shadow\": %s,\n  \"t_shadow_color\": \"%s\",\n  \"t_shadow_width\": %.1f,\n"
        "  \"t_stroke\": %s,\n  \"t_stroke_color\": \"%s\",\n  \"t_stroke_width\": %.1f,\n  \"t_color\": \"%s\",\n"
        "  \"metadata_inject\": %s,\n  \"xlsx_time_exif\": %s\n}\n",
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_size)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_margin_x)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_margin_y)),
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_opacity)), c_pos,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_c_shadow)) ? "true" : "false",
        color_hex_c_shadow, c_shadow_w,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_c_stroke)) ? "true" : "false",
        color_hex_c_stroke, c_stroke_w,
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_size)),
        t_spacing,
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_margin_x)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_margin_y)),
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_opacity)), t_pos,
        t_font, t_align,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_bold)) ? "true" : "false",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_italic)) ? "true" : "false",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_underline)) ? "true" : "false",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_shadow)) ? "true" : "false",
        color_hex_t_shadow, t_shadow_w,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_stroke)) ? "true" : "false",
        color_hex_t_stroke, t_stroke_w, color_hex_t_color,
        stampy_config_get_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, FALSE) ? "true" : "false",
        stampy_config_get_bool(STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, FALSE) ? "true" : "false"
    );

    if (!g_file_set_contents(filepath, json_data, -1, &error)) {
        if (error) {
            g_autofree char *log_msg = g_strdup_printf("[ERROR] Save failed: %s", error->message);
            stampy_append_log(self, log_msg);
            g_clear_error(&error);
        }
    }
}

void stampy_load_preset_from_file(StampyWindow *self, const char *filepath) {
    g_autofree char *content = NULL;
    gsize length = 0;
    int c_size = 140, c_opacity = 77;
    double c_margin_x = 20.0, c_margin_y = 20.0;
    double t_size = 29.0, t_margin_x = 20.0, t_margin_y = 20.0;
    double t_spacing = 12.0;
    int t_opacity = 100;
    char hex[10];
    GdkRGBA rgba;

    if (!self || !filepath || !g_file_get_contents(filepath, &content, &length, NULL)) return;

    /* Compass size and opacity. */
    if (strstr(content, "\"c_size\":")) sscanf(strstr(content, "\"c_size\":"), "\"c_size\": %d", &c_size);
    else if (strstr(content, "\"size_px\":")) sscanf(strstr(content, "\"size_px\":"), "\"size_px\": %d", &c_size);

    if (strstr(content, "\"c_opacity\":")) sscanf(strstr(content, "\"c_opacity\":"), "\"c_opacity\": %d", &c_opacity);
    else if (strstr(content, "\"opacity_percent\":")) sscanf(strstr(content, "\"opacity_percent\":"), "\"opacity_percent\": %d", &c_opacity);

    if (strstr(content, "\"c_margin_x\":")) sscanf(strstr(content, "\"c_margin_x\":"), "\"c_margin_x\": %lf", &c_margin_x);
    if (strstr(content, "\"c_margin_y\":")) sscanf(strstr(content, "\"c_margin_y\":"), "\"c_margin_y\": %lf", &c_margin_y);

    /* Text size, margins, and spacing. */
    if (strstr(content, "\"t_size\":")) sscanf(strstr(content, "\"t_size\":"), "\"t_size\": %lf", &t_size);
    else if (strstr(content, "\"font_size\":")) {
        sscanf(strstr(content, "\"font_size\":"), "\"font_size\": %lf", &t_size);
    }

    if (strstr(content, "\"t_spacing\":")) sscanf(strstr(content, "\"t_spacing\":"), "\"t_spacing\": %lf", &t_spacing);

    if (strstr(content, "\"t_opacity\":")) sscanf(strstr(content, "\"t_opacity\":"), "\"t_opacity\": %d", &t_opacity);

    if (strstr(content, "\"t_margin_x\":")) sscanf(strstr(content, "\"t_margin_x\":"), "\"t_margin_x\": %lf", &t_margin_x);
    else if (strstr(content, "\"margin_x\":")) {
        sscanf(strstr(content, "\"margin_x\":"), "\"margin_x\": %lf", &t_margin_x);
    }

    if (strstr(content, "\"t_margin_y\":")) sscanf(strstr(content, "\"t_margin_y\":"), "\"t_margin_y\": %lf", &t_margin_y);
    else if (strstr(content, "\"margin_y\":")) {
        sscanf(strstr(content, "\"margin_y\":"), "\"margin_y\": %lf", &t_margin_y);
    }

    /* Compass and text position dropdowns. */
    if (strstr(content, "\"c_pos\":")) {
        const char *pos_ptr = strstr(content, "\"c_pos\":");
        char line_buf[64] = {0};
        sscanf(pos_ptr, "\"c_pos\": \"%63[^\"]\"", line_buf);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_compass_pos), pos_string_to_index(line_buf));
    }

    if (strstr(content, "\"t_pos\":")) {
        const char *pos_ptr = strstr(content, "\"t_pos\":");
        char line_buf[64] = {0};
        sscanf(pos_ptr, "\"t_pos\": \"%63[^\"]\"", line_buf);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_text_pos), pos_string_to_index(line_buf));
    }

    /* Font family. */
    if (strstr(content, "\"t_font\":") || strstr(content, "\"font_family\":")) {
        const char *f_ptr = strstr(content, "\"t_font\":") ? strstr(content, "\"t_font\":") : strstr(content, "\"font_family\":");
        const char *val_start = strchr(f_ptr, ':');

        if (val_start) {
            val_start = strchr(val_start, '"');
            if (val_start) {
                const char *val_end;
                val_start++;
                val_end = strchr(val_start, '"');

                if (val_end) {
                    g_autofree char *font_buf = g_strndup(val_start, val_end - val_start);
                    GListModel *f_model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_text_font));

                    if (GTK_IS_STRING_LIST(f_model) && font_buf && strlen(font_buf) > 0) {
                        guint n_fonts = g_list_model_get_n_items(f_model);
                        gboolean matched = FALSE;
                        guint fi;

                        for (fi = 0; fi < n_fonts; fi++) {
                            const char *f_name = gtk_string_list_get_string(GTK_STRING_LIST(f_model), fi);
                            if (f_name && g_ascii_strcasecmp(f_name, font_buf) == 0) {
                                gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_text_font), fi);
                                matched = TRUE;
                                break;
                            }
                        }

                        if (!matched) {
                            for (fi = 0; fi < n_fonts; fi++) {
                                const char *f_name = gtk_string_list_get_string(GTK_STRING_LIST(f_model), fi);
                                if (f_name && (g_strrstr(f_name, font_buf) || g_strrstr(font_buf, f_name))) {
                                    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_text_font), fi);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Spin button values. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_compass_size), c_size);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_compass_margin_x), c_margin_x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_compass_margin_y), c_margin_y);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_compass_opacity), c_opacity);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_text_size), t_size);
    if (self->val_text_spacing) gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_text_spacing), t_spacing);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_text_margin_x), t_margin_x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_text_margin_y), t_margin_y);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->val_text_opacity), t_opacity);

    /* Toggle buttons. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_c_shadow), strstr(content, "\"c_shadow\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_c_stroke), strstr(content, "\"c_stroke\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_bold), strstr(content, "\"t_bold\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_italic), strstr(content, "\"t_italic\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_underline), strstr(content, "\"t_underline\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_shadow), strstr(content, "\"t_shadow\": true") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_stroke), strstr(content, "\"t_stroke\": true") != NULL);
    stampy_config_set_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, strstr(content, "\"metadata_inject\": true") != NULL);
    stampy_config_set_bool(STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, strstr(content, "\"xlsx_time_exif\": true") != NULL);

    /* Text alignment. */
    if (strstr(content, "\"t_align\": \"left\"") || strstr(content, "\"alignment\": \"left\""))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_left), TRUE);
    else if (strstr(content, "\"t_align\": \"center\"") || strstr(content, "\"alignment\": \"center\""))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_center), TRUE);
    else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_right), TRUE);

    /* Colors. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (strstr(content, "\"c_stroke_color\": \"") && self->btn_c_stroke_color) {
        sscanf(strstr(content, "\"c_stroke_color\": \""), "\"c_stroke_color\": \"%7s\"", hex);
        if (gdk_rgba_parse(&rgba, hex)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_c_stroke_color), &rgba);
    }
    if (strstr(content, "\"c_shadow_color\": \"") && self->btn_c_shadow_color) {
        sscanf(strstr(content, "\"c_shadow_color\": \""), "\"c_shadow_color\": \"%7s\"", hex);
        if (gdk_rgba_parse(&rgba, hex)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_c_shadow_color), &rgba);
    }
    if (strstr(content, "\"t_stroke_color\": \"") && self->btn_t_stroke_color) {
        sscanf(strstr(content, "\"t_stroke_color\": \""), "\"t_stroke_color\": \"%7s\"", hex);
        if (gdk_rgba_parse(&rgba, hex)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_t_stroke_color), &rgba);
    }
    if (strstr(content, "\"t_shadow_color\": \"") && self->btn_t_shadow_color) {
        sscanf(strstr(content, "\"t_shadow_color\": \""), "\"t_shadow_color\": \"%7s\"", hex);
        if (gdk_rgba_parse(&rgba, hex)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_t_shadow_color), &rgba);
    }
    if (strstr(content, "\"t_color\": \"") && self->btn_t_color) {
        sscanf(strstr(content, "\"t_color\": \""), "\"t_color\": \"%7s\"", hex);
        if (gdk_rgba_parse(&rgba, hex)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(self->btn_t_color), &rgba);
    }
#pragma GCC diagnostic pop

    /* Widths / Sliders */
    if (strstr(content, "\"t_stroke_width\":") && self->val_t_stroke_width) {
        double val = 2.5;
        if (sscanf(strstr(content, "\"t_stroke_width\":"), "\"t_stroke_width\": %lf", &val) == 1)
            gtk_range_set_value(GTK_RANGE(self->val_t_stroke_width), val);
    }
    if (strstr(content, "\"t_shadow_width\":") && self->val_t_shadow_width) {
        double val = 2.0;
        if (sscanf(strstr(content, "\"t_shadow_width\":"), "\"t_shadow_width\": %lf", &val) == 1)
            gtk_range_set_value(GTK_RANGE(self->val_t_shadow_width), val);
    }
    if (strstr(content, "\"c_stroke_width\":") && self->val_c_stroke_width) {
        double val = 2.0;
        if (sscanf(strstr(content, "\"c_stroke_width\":"), "\"c_stroke_width\": %lf", &val) == 1)
            gtk_range_set_value(GTK_RANGE(self->val_c_stroke_width), val);
    }
    if (strstr(content, "\"c_shadow_width\":") && self->val_c_shadow_width) {
        double val = 4.0;
        if (sscanf(strstr(content, "\"c_shadow_width\":"), "\"c_shadow_width\": %lf", &val) == 1)
            gtk_range_set_value(GTK_RANGE(self->val_c_shadow_width), val);
    }

    /* Refresh the preview. */
    stampy_render_live_preview(self);
}

void stampy_save_last_state(StampyWindow *self) {
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "stampy_last.json", NULL);
    GListModel *model;
    guint idx;
    const char *preset_name = "";
    char *json_data = NULL;
    GError *error = NULL;

    if (!self || self->loading_state) return;

    stampy_save_preset_to_file(self, path);

    /* Session data (last preset name). Preferences and the locked compass
       live in the config store, not here; see stampy-config.c. */
    if (self->drop_presets) {
        model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_presets));
        idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_presets));
        if (model && idx != GTK_INVALID_LIST_POSITION && GTK_IS_STRING_LIST(model)) {
            preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), idx);
        }
    }

    /* Append session fields before the closing brace. */
    if (g_file_get_contents(path, &json_data, NULL, &error)) {
        char *last_brace = g_strrstr(json_data, "}");
        if (last_brace) {
            char *extended;
            *last_brace = '\0';
            extended = g_strdup_printf(
                "%s,\n  \"last_preset\": \"%s\"\n}\n",
                json_data, preset_name ? preset_name : "");
            g_file_set_contents(path, extended, -1, &error);
            g_free(extended);
        }
        g_free(json_data);
    }
}

static gboolean on_save_state_timeout(gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    if (self && STAMPY_IS_WINDOW(self)) {
        self->save_state_timeout_id = 0;
        stampy_save_last_state(self);
    }
    return G_SOURCE_REMOVE;
}

void stampy_schedule_save_last_state(StampyWindow *self) {
    if (!self || self->loading_state) return;
    if (self->save_state_timeout_id > 0) {
        g_source_remove(self->save_state_timeout_id);
        self->save_state_timeout_id = 0;
    }
    self->save_state_timeout_id = g_timeout_add(300, on_save_state_timeout, self);
}

/* Restores the compass folder and lock flag from the config store,
   so a locked compass survives app restarts. */
static void restore_compass_from_config(StampyWindow *self) {
    g_autofree char *compass_path = NULL;
    gboolean locked = FALSE;

    if (!self) return;

    compass_path = stampy_config_get_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER, "");
    locked = stampy_config_get_bool(STAMPY_CONFIG_KEY_COMPASS_LOCKED, FALSE);

    if (compass_path && *compass_path && g_file_test(compass_path, G_FILE_TEST_IS_DIR)) {
        g_clear_object(&self->file_compass);
        self->file_compass = g_file_new_for_path(compass_path);
        if (self->lbl_compass) {
            g_autofree char *basename = g_path_get_basename(compass_path);
            gtk_label_set_text(GTK_LABEL(self->lbl_compass), basename);
        }
        stampy_update_clear_button_visibility(self);
    }

    if (locked && self->btn_compass_lock) {
        self->compass_locked = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_compass_lock), TRUE);
    } else if (self->btn_compass_lock) {
        self->compass_locked = FALSE;
        gtk_button_set_icon_name(GTK_BUTTON(self->btn_compass_lock), "padlock-open-symbolic");
    }
}

void stampy_load_last_state(StampyWindow *self) {
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "stampy_last.json", NULL);
    char *content = NULL;
    gsize len = 0;
    GError *error = NULL;

    /* Always restore the compass, even without a session file. */
    restore_compass_from_config(self);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return;

    self->loading_state = TRUE;

    /* Read first; applying values triggers re-entrant saves. */
    if (!g_file_get_contents(path, &content, &len, &error)) {
        self->loading_state = FALSE;
        return;
    }

    /* Apply the stored panel values. */
    stampy_load_preset_from_file(self, path);

    /* Restore the selected preset. */
    if (self->drop_presets && strstr(content, "\"last_preset\":")) {
        char preset_name[256] = {0};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
        sscanf(strstr(content, "\"last_preset\": \""), "\"last_preset\": \"%255[^\"]\"", preset_name);
#pragma GCC diagnostic pop
        if (preset_name[0]) {
            GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_presets));
            guint n = g_list_model_get_n_items(model);
            for (guint i = 0; i < n; i++) {
                const char *name = gtk_string_list_get_string(GTK_STRING_LIST(model), i);
                if (g_strcmp0(name, preset_name) == 0) {
                    g_signal_handlers_block_by_func(self->drop_presets, on_preset_dropdown_changed, self);
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_presets), i);
                    g_signal_handlers_unblock_by_func(self->drop_presets, on_preset_dropdown_changed, self);
                    break;
                }
            }
        }
    }

    g_free(content);

    /* Persist the restored state once. */
    self->loading_state = FALSE;
    stampy_save_last_state(self);
}

void on_preset_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    GListModel *model = gtk_drop_down_get_model(dropdown);
    guint selected_idx = gtk_drop_down_get_selected(dropdown);

    (void)pspec;
    if (!self || !STAMPY_IS_WINDOW(self) || !model) return;

    if (selected_idx != GTK_INVALID_LIST_POSITION) {
        const char *preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
        g_autofree char *fname = g_strdup_printf("%s.json", preset_name);
        g_autofree char *filepath = g_build_filename(g_get_user_config_dir(), "stampy_presets", fname, NULL);

        if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
            g_signal_handlers_block_by_func(dropdown, on_preset_dropdown_changed, self);

            stampy_load_preset_from_file(self, filepath);
            stampy_show_toast(self, "Preset loaded!");

            g_signal_handlers_unblock_by_func(dropdown, on_preset_dropdown_changed, self);
        }

        /* Persist the selection even without a preset file on disk
           (e.g. "Default Preset"). Suppressed during state load. */
        stampy_save_last_state(self);
    }
}

void on_btn_apply_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkDropDown *dropdown;
    GListModel *model;
    guint selected_idx;
    const char *preset_name = "Default Preset";
    g_autofree char *filename = NULL;
    g_autofree char *config_dir = NULL;
    g_autofree char *filepath = NULL;

    (void)btn;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    dropdown = GTK_DROP_DOWN(self->drop_presets);
    model = gtk_drop_down_get_model(dropdown);
    selected_idx = gtk_drop_down_get_selected(dropdown);

    if (selected_idx != GTK_INVALID_LIST_POSITION && model) {
        preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
    }

    config_dir = g_build_filename(g_get_user_config_dir(), "stampy_presets", NULL);
    g_mkdir_with_parents(config_dir, 0755);

    filename = g_strdup_printf("%s.json", preset_name);
    filepath = g_build_filename(config_dir, filename, NULL);

    stampy_save_preset_to_file(self, filepath);
    stampy_save_last_state(self);

    stampy_show_toast(self, "Preset saved successfully!");
    stampy_append_log(self, "[INFO] Preset saved.");
}

static void on_rename_dialog_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    PresetDialogData *data = (PresetDialogData *)user_data;
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "save") == 0 && data && data->entry) {
        const char *new_name = gtk_editable_get_text(GTK_EDITABLE(data->entry));
        if (new_name && strlen(new_name) > 0) {
            GtkDropDown *dropdown = GTK_DROP_DOWN(data->self->drop_presets);
            GListModel *model = gtk_drop_down_get_model(dropdown);
            guint selected_idx = gtk_drop_down_get_selected(dropdown);

            if (GTK_IS_STRING_LIST(model) && selected_idx != GTK_INVALID_LIST_POSITION) {
                const char *old_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
                g_autofree char *old_fname = g_strdup_printf("%s.json", old_name);
                g_autofree char *old_path = g_build_filename(g_get_user_config_dir(), "stampy_presets", old_fname, NULL);

                if (g_file_test(old_path, G_FILE_TEST_EXISTS)) g_remove(old_path);

                gtk_string_list_splice(GTK_STRING_LIST(model), selected_idx, 1, (const char *[]){ new_name, NULL });
                gtk_drop_down_set_selected(dropdown, selected_idx);

                on_btn_apply_preset_clicked(NULL, data->self);
                stampy_show_toast(data->self, "Preset renamed and saved!");
            }
        }
    }
    g_free(data);
}

void on_btn_edit_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkDropDown *dropdown;
    GListModel *model;
    guint selected_idx;
    AdwDialog *dialog;
    GtkWidget *entry;
    PresetDialogData *data;
    const char *current_name = "";

    (void)btn;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    dropdown = GTK_DROP_DOWN(self->drop_presets);
    model = gtk_drop_down_get_model(dropdown);
    selected_idx = gtk_drop_down_get_selected(dropdown);

    if (selected_idx != GTK_INVALID_LIST_POSITION && model) {
        current_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
    }

    dialog = ADW_DIALOG(adw_alert_dialog_new("Rename Preset", "Enter a new name for the current preset:"));
    entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), current_name);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "save", "Save");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);

    data = g_new0(PresetDialogData, 1);
    data->self = self;
    data->entry = entry;

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_rename_dialog_response, data);
}

static void on_add_preset_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    PresetDialogData *data = (PresetDialogData *)user_data;
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "save") == 0 && data && data->entry) {
        const char *new_name = gtk_editable_get_text(GTK_EDITABLE(data->entry));
        if (new_name && strlen(new_name) > 0) {
            GtkDropDown *dropdown = GTK_DROP_DOWN(data->self->drop_presets);
            GListModel *model = gtk_drop_down_get_model(dropdown);

            if (GTK_IS_STRING_LIST(model)) {
                gtk_string_list_append(GTK_STRING_LIST(model), new_name);
                gtk_drop_down_set_selected(dropdown, g_list_model_get_n_items(model) - 1);
                on_btn_apply_preset_clicked(NULL, data->self);
                stampy_show_toast(data->self, "New preset added!");
            }
        }
    }
    g_free(data);
}

void on_btn_add_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    GtkWidget *entry;
    PresetDialogData *data;

    (void)btn;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    dialog = ADW_DIALOG(adw_alert_dialog_new("Create New Preset", "Enter a name for the new preset:"));
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "e.g., DAS Project Style");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "save", "Add");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);

    data = g_new0(PresetDialogData, 1);
    data->self = self;
    data->entry = entry;

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_add_preset_response, data);
}

static void on_delete_preset_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "delete") == 0 && self) {
        GtkDropDown *dropdown = GTK_DROP_DOWN(self->drop_presets);
        GListModel *model = gtk_drop_down_get_model(dropdown);
        guint selected_idx = gtk_drop_down_get_selected(dropdown);
        guint total = model ? g_list_model_get_n_items(model) : 0;

        if (GTK_IS_STRING_LIST(model) && total > 1 && selected_idx != GTK_INVALID_LIST_POSITION) {
            const char *preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
            g_autofree char *fname = g_strdup_printf("%s.json", preset_name);
            g_autofree char *filepath = g_build_filename(g_get_user_config_dir(), "stampy_presets", fname, NULL);

            if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
                g_remove(filepath);
            }

            gtk_string_list_remove(GTK_STRING_LIST(model), selected_idx);
            gtk_drop_down_set_selected(dropdown, 0);

            stampy_show_toast(self, "Preset deleted!");
            stampy_append_log(self, "[INFO] Preset deleted.");
        } else {
            stampy_show_toast(self, "Cannot delete default preset!");
        }
    }
}

void on_btn_remove_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkDropDown *dropdown;
    GListModel *model;
    guint selected_idx;
    AdwDialog *dialog;
    const char *preset_name = "";

    (void)btn;
    if (!self || !STAMPY_IS_WINDOW(self)) return;

    dropdown = GTK_DROP_DOWN(self->drop_presets);
    model = gtk_drop_down_get_model(dropdown);
    selected_idx = gtk_drop_down_get_selected(dropdown);

    if (selected_idx != GTK_INVALID_LIST_POSITION && model) {
        preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
    }

    dialog = ADW_DIALOG(adw_alert_dialog_new("Delete Preset?", g_strdup_printf("Are you sure you want to delete '%s'?", preset_name)));

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", "Delete");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_delete_preset_response, self);
}

static void on_preset_import_file_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, NULL);

    if (file && self) {
        g_autofree char *path = g_file_get_path(file);
        stampy_load_preset_from_file(self, path);
        stampy_show_toast(self, "Preset imported!");
    }
}

void on_btn_import_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkFileDialog *dialog;

    (void)btn;
    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Preset JSON");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_preset_import_file_opened, self);
}

static void on_preset_export_file_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(dialog, res, NULL);

    if (file && self) {
        g_autofree char *path = g_file_get_path(file);
        stampy_save_preset_to_file(self, path);
        stampy_show_toast(self, "Preset exported!");
    }
}

void on_btn_export_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GtkFileDialog *dialog;
    GtkDropDown *dropdown;
    GListModel *model;
    guint selected_idx;
    const char *preset_name = "preset";
    g_autofree char *default_fname = NULL;

    (void)btn;
    if (!self) return;

    dropdown = GTK_DROP_DOWN(self->drop_presets);
    model = gtk_drop_down_get_model(dropdown);
    selected_idx = gtk_drop_down_get_selected(dropdown);

    if (selected_idx != GTK_INVALID_LIST_POSITION && model) {
        preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), selected_idx);
    }

    default_fname = g_strdup_printf("%s.json", preset_name);

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Preset JSON");
    gtk_file_dialog_set_initial_name(dialog, default_fname);
    gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_preset_export_file_saved, self);
}
