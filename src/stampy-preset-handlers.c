#include "stampy-preset-handlers.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

typedef struct {
    StampyWindow *self;
    GtkWidget *entry;
} PresetDialogData;

static JsonNode *json_obj_find_member(JsonObject *obj, const char *key, const char *legacy_key) {
    if (!obj) return NULL;
    if (key && json_object_has_member(obj, key))
        return json_object_get_member(obj, key);
    if (legacy_key && json_object_has_member(obj, legacy_key))
        return json_object_get_member(obj, legacy_key);
    return NULL;
}

static int json_obj_get_int(JsonObject *obj, const char *key, const char *legacy_key, int fallback) {
    JsonNode *node = json_obj_find_member(obj, key, legacy_key);
    GType vtype;

    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
    vtype = json_node_get_value_type(node);
    if (vtype == G_TYPE_INT64) return (int)json_node_get_int(node);
    if (vtype == G_TYPE_DOUBLE) return (int)json_node_get_double(node);
    return fallback;
}

static double json_obj_get_double(JsonObject *obj, const char *key, const char *legacy_key, double fallback) {
    JsonNode *node = json_obj_find_member(obj, key, legacy_key);
    GType vtype;

    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
    vtype = json_node_get_value_type(node);
    if (vtype == G_TYPE_DOUBLE) return json_node_get_double(node);
    if (vtype == G_TYPE_INT64) return (double)json_node_get_int(node);
    return fallback;
}

static gboolean json_obj_get_bool(JsonObject *obj, const char *key, gboolean fallback, gboolean *out_found) {
    JsonNode *node = json_obj_find_member(obj, key, NULL);

    if (out_found) *out_found = FALSE;
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) {
        if (out_found) *out_found = TRUE;
        return json_node_get_boolean(node);
    }
    return fallback;
}

static const char *json_obj_get_string(JsonObject *obj, const char *key, const char *legacy_key) {
    JsonNode *node = json_obj_find_member(obj, key, legacy_key);

    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return NULL;
    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return json_node_get_string(node);
    return NULL;
}

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

static char *rgba_to_hex(GtkWidget *color_btn, const char *fallback) {
    GdkRGBA rgba;

    if (!color_btn) return g_strdup(fallback);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(color_btn), &rgba);
#pragma GCC diagnostic pop
    return g_strdup_printf("#%02X%02X%02X",
                           (int)(rgba.red * 255),
                           (int)(rgba.green * 255),
                           (int)(rgba.blue * 255));
}

static void load_color_from_json(JsonObject *obj, const char *key, GtkWidget *color_btn) {
    const char *hex;
    GdkRGBA rgba;

    if (!obj || !key || !color_btn) return;
    hex = json_obj_get_string(obj, key, NULL);
    if (hex && gdk_rgba_parse(&rgba, hex)) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(color_btn), &rgba);
#pragma GCC diagnostic pop
    }
}

static void load_range_from_json(JsonObject *obj, const char *key, GtkWidget *range, double fallback) {
    if (!obj || !key || !range) return;
    if (json_object_has_member(obj, key)) {
        gtk_range_set_value(GTK_RANGE(range), json_obj_get_double(obj, key, NULL, fallback));
    }
}

static void save_preset_json(StampyWindow *self, const char *filepath, gboolean include_compass, const char *last_preset_name) {
    const char *c_pos, *t_pos, *t_font = "Arial", *t_align;
    g_autofree char *color_hex_c_stroke = NULL;
    g_autofree char *color_hex_c_shadow = NULL;
    g_autofree char *color_hex_t_stroke = NULL;
    g_autofree char *color_hex_t_shadow = NULL;
    g_autofree char *color_hex_t_color = NULL;
    g_autofree char *m_strip_text_owned = NULL;
    g_autofree char *date_src_owned = NULL;
    const char *m_strip_text = "";
    const char *date_src = "auto";
    g_autoptr(GError) error = NULL;
    guint c_idx, t_idx;
    gboolean def_meta, def_time, m_title, m_subj, m_tags, m_comm, m_date, m_time;
    gboolean m_strip_all, m_remove_cam, m_remove_ser, m_remove_soft, m_remove_auth, m_remove_id, m_timephoto;
    double t_spacing = 12.0;
    double t_stroke_w;
    double t_shadow_w;
    double c_stroke_w;
    double c_shadow_w;
    g_autoptr(JsonObject) obj = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autoptr(JsonGenerator) gen = NULL;

    if (!self || !filepath) return;

    t_stroke_w = self->val_t_stroke_width ? gtk_range_get_value(GTK_RANGE(self->val_t_stroke_width)) : 2.5;
    t_shadow_w = self->val_t_shadow_width ? gtk_range_get_value(GTK_RANGE(self->val_t_shadow_width)) : 2.0;
    c_stroke_w = self->val_c_stroke_width ? gtk_range_get_value(GTK_RANGE(self->val_c_stroke_width)) : 2.0;
    c_shadow_w = self->val_c_shadow_width ? gtk_range_get_value(GTK_RANGE(self->val_c_shadow_width)) : 4.0;

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

    color_hex_c_stroke = rgba_to_hex(self->btn_c_stroke_color, "#000000");
    color_hex_c_shadow = rgba_to_hex(self->btn_c_shadow_color, "#000000");
    color_hex_t_stroke = rgba_to_hex(self->btn_t_stroke_color, "#000000");
    color_hex_t_shadow = rgba_to_hex(self->btn_t_shadow_color, "#000000");
    color_hex_t_color = rgba_to_hex(self->btn_t_color, "#FFFFFF");

    if (self->has_quick_meta) {
        const StampyQuickMeta *qm = &self->quick_meta;
        m_title = qm->meta_title;
        m_subj = qm->meta_subject;
        m_tags = qm->meta_tags;
        m_comm = qm->meta_comments;
        m_date = qm->meta_date;
        m_time = qm->meta_time;
        m_strip_all = qm->meta_strip_all_source;
        m_remove_cam = qm->meta_remove_camera;
        m_remove_ser = qm->meta_remove_serial;
        m_remove_soft = qm->meta_remove_software;
        m_remove_auth = qm->meta_remove_author;
        m_remove_id = qm->meta_remove_image_id;
        m_timephoto = qm->export_timephoto_rename;
        m_strip_text = qm->meta_strip_text ? qm->meta_strip_text : "";
        date_src = qm->meta_date_source[0] ? qm->meta_date_source : "auto";
    } else {
        def_meta = stampy_config_get_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, FALSE);
        def_time = stampy_config_get_bool(STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, FALSE);
        m_title = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_TITLE, def_meta);
        m_subj = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_SUBJECT, def_meta);
        m_tags = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_TAGS, def_meta);
        m_comm = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_COMMENTS, def_meta);
        m_date = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_DATE, def_time);
        m_time = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_TIME, def_time);
        m_strip_all = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_STRIP_ALL_SOURCE, FALSE);
        m_remove_cam = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_REMOVE_CAMERA, FALSE);
        m_remove_ser = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_REMOVE_SERIAL, FALSE);
        m_remove_soft = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_REMOVE_SOFTWARE, FALSE);
        m_remove_auth = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_REMOVE_AUTHOR, FALSE);
        m_remove_id = stampy_config_get_bool(STAMPY_CONFIG_KEY_META_REMOVE_IMAGE_ID, FALSE);
        m_timephoto = stampy_config_get_bool(STAMPY_CONFIG_KEY_TIMEPHOTO_RENAME, FALSE);
        m_strip_text_owned = stampy_config_get_string(STAMPY_CONFIG_KEY_META_STRIP_TEXT, "");
        date_src_owned = stampy_config_get_string(STAMPY_CONFIG_KEY_META_DATE_SOURCE, "auto");
        m_strip_text = m_strip_text_owned ? m_strip_text_owned : "";
        date_src = (date_src_owned && *date_src_owned) ? date_src_owned : "auto";
    }

    obj = json_object_new();
    json_object_set_int_member(obj, "c_size", (gint64)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_size)));
    json_object_set_double_member(obj, "c_margin_x", gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_margin_x)));
    json_object_set_double_member(obj, "c_margin_y", gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_margin_y)));
    json_object_set_int_member(obj, "c_opacity", (gint64)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_compass_opacity)));
    json_object_set_string_member(obj, "c_pos", c_pos);
    json_object_set_boolean_member(obj, "c_shadow", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_c_shadow)));
    json_object_set_string_member(obj, "c_shadow_color", color_hex_c_shadow);
    json_object_set_double_member(obj, "c_shadow_width", c_shadow_w);
    json_object_set_boolean_member(obj, "c_stroke", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_c_stroke)));
    json_object_set_string_member(obj, "c_stroke_color", color_hex_c_stroke);
    json_object_set_double_member(obj, "c_stroke_width", c_stroke_w);

    json_object_set_double_member(obj, "t_size", gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_size)));
    json_object_set_double_member(obj, "t_spacing", t_spacing);
    json_object_set_double_member(obj, "t_margin_x", gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_margin_x)));
    json_object_set_double_member(obj, "t_margin_y", gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_margin_y)));
    json_object_set_int_member(obj, "t_opacity", (gint64)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->val_text_opacity)));
    json_object_set_string_member(obj, "t_pos", t_pos);
    json_object_set_string_member(obj, "t_font", t_font);
    json_object_set_string_member(obj, "t_align", t_align);
    json_object_set_boolean_member(obj, "t_bold", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_bold)));
    json_object_set_boolean_member(obj, "t_italic", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_italic)));
    json_object_set_boolean_member(obj, "t_underline", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_underline)));
    json_object_set_boolean_member(obj, "t_shadow", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_shadow)));
    json_object_set_string_member(obj, "t_shadow_color", color_hex_t_shadow);
    json_object_set_double_member(obj, "t_shadow_width", t_shadow_w);
    json_object_set_boolean_member(obj, "t_stroke", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_t_stroke)));
    json_object_set_string_member(obj, "t_stroke_color", color_hex_t_stroke);
    json_object_set_double_member(obj, "t_stroke_width", t_stroke_w);
    json_object_set_string_member(obj, "t_color", color_hex_t_color);

    json_object_set_boolean_member(obj, "metadata_inject", (m_title || m_subj || m_tags || m_comm));
    json_object_set_boolean_member(obj, "xlsx_time_exif", (m_date || m_time));
    json_object_set_boolean_member(obj, "meta_title", m_title);
    json_object_set_boolean_member(obj, "meta_subject", m_subj);
    json_object_set_boolean_member(obj, "meta_tags", m_tags);
    json_object_set_boolean_member(obj, "meta_comments", m_comm);
    json_object_set_boolean_member(obj, "meta_date", m_date);
    json_object_set_string_member(obj, "meta_date_source", date_src);
    json_object_set_boolean_member(obj, "meta_time", m_time);
    json_object_set_boolean_member(obj, "meta_strip_all_source", m_strip_all);
    json_object_set_boolean_member(obj, "meta_remove_camera", m_remove_cam);
    json_object_set_boolean_member(obj, "meta_remove_serial", m_remove_ser);
    json_object_set_boolean_member(obj, "meta_remove_software", m_remove_soft);
    json_object_set_boolean_member(obj, "meta_remove_author", m_remove_auth);
    json_object_set_boolean_member(obj, "meta_remove_image_id", m_remove_id);
    json_object_set_string_member(obj, "meta_strip_text", m_strip_text);
    json_object_set_boolean_member(obj, "export_timephoto_rename", m_timephoto);

    if (include_compass) {
        g_autofree char *compass_path = self->file_compass ? g_file_get_path(self->file_compass) : NULL;
        json_object_set_string_member(obj, "compass_folder", compass_path ? compass_path : "");
    }

    if (last_preset_name != NULL) {
        json_object_set_string_member(obj, "last_preset", last_preset_name);
    }

    gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, obj);
    json_generator_set_root(gen, root);

    if (!json_generator_to_file(gen, filepath, &error)) {
        if (error) {
            g_autofree char *log_msg = g_strdup_printf("[ERROR] Save failed: %s", error->message);
            stampy_append_log(self, log_msg);
        }
    }
}

void stampy_save_preset_to_file(StampyWindow *self, const char *filepath) {
    save_preset_json(self, filepath, TRUE, NULL);
}

void stampy_load_preset_from_file(StampyWindow *self, const char *filepath) {
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    int c_size, c_opacity, t_opacity;
    double c_margin_x, c_margin_y, t_size, t_margin_x, t_margin_y, t_spacing;
    const char *c_pos, *t_pos, *font_buf, *t_align, *date_src, *m_strip_text, *compass_folder;
    gboolean legacy_meta, legacy_time;
    gboolean m_title, m_subj, m_tags, m_comm, m_date, m_time;
    gboolean m_strip_all, m_remove_cam, m_remove_ser, m_remove_soft, m_remove_auth, m_remove_id, m_timephoto;

    if (!self || !filepath) return;

    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, filepath, NULL)) return;

    root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
    obj = json_node_get_object(root);
    if (!obj) return;

    /* Compass size and opacity. */
    c_size = json_obj_get_int(obj, "c_size", "size_px", 140);
    c_opacity = json_obj_get_int(obj, "c_opacity", "opacity_percent", 77);
    c_margin_x = json_obj_get_double(obj, "c_margin_x", NULL, 20.0);
    c_margin_y = json_obj_get_double(obj, "c_margin_y", NULL, 20.0);

    /* Text size, margins, and spacing. */
    t_size = json_obj_get_double(obj, "t_size", "font_size", 29.0);
    t_spacing = json_obj_get_double(obj, "t_spacing", NULL, 12.0);
    t_opacity = json_obj_get_int(obj, "t_opacity", NULL, 100);
    t_margin_x = json_obj_get_double(obj, "t_margin_x", "margin_x", 20.0);
    t_margin_y = json_obj_get_double(obj, "t_margin_y", "margin_y", 20.0);

    /* Compass and text position dropdowns. */
    c_pos = json_obj_get_string(obj, "c_pos", NULL);
    if (c_pos) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_compass_pos), pos_string_to_index(c_pos));
    }

    t_pos = json_obj_get_string(obj, "t_pos", NULL);
    if (t_pos) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drop_text_pos), pos_string_to_index(t_pos));
    }

    /* Font family. */
    font_buf = json_obj_get_string(obj, "t_font", "font_family");
    if (font_buf && *font_buf && self->drop_text_font) {
        GListModel *f_model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_text_font));

        if (GTK_IS_STRING_LIST(f_model)) {
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
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_c_shadow), json_obj_get_bool(obj, "c_shadow", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_c_stroke), json_obj_get_bool(obj, "c_stroke", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_bold), json_obj_get_bool(obj, "t_bold", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_italic), json_obj_get_bool(obj, "t_italic", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_underline), json_obj_get_bool(obj, "t_underline", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_shadow), json_obj_get_bool(obj, "t_shadow", FALSE, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_stroke), json_obj_get_bool(obj, "t_stroke", FALSE, NULL));

    legacy_meta = json_obj_get_bool(obj, "metadata_inject", FALSE, NULL);
    legacy_time = json_obj_get_bool(obj, "xlsx_time_exif", FALSE, NULL);
    m_title = json_obj_get_bool(obj, "meta_title", legacy_meta, NULL);
    m_subj = json_obj_get_bool(obj, "meta_subject", legacy_meta, NULL);
    m_tags = json_obj_get_bool(obj, "meta_tags", legacy_meta, NULL);
    m_comm = json_obj_get_bool(obj, "meta_comments", legacy_meta, NULL);
    m_date = json_obj_get_bool(obj, "meta_date", legacy_time, NULL);
    m_time = json_obj_get_bool(obj, "meta_time", legacy_time, NULL);
    m_strip_all = json_obj_get_bool(obj, "meta_strip_all_source", FALSE, NULL);
    m_remove_cam = json_obj_get_bool(obj, "meta_remove_camera", FALSE, NULL);
    m_remove_ser = json_obj_get_bool(obj, "meta_remove_serial", FALSE, NULL);
    m_remove_soft = json_obj_get_bool(obj, "meta_remove_software", FALSE, NULL);
    m_remove_auth = json_obj_get_bool(obj, "meta_remove_author", FALSE, NULL);
    m_remove_id = json_obj_get_bool(obj, "meta_remove_image_id", FALSE, NULL);
    m_timephoto = json_obj_get_bool(obj, "export_timephoto_rename", FALSE, NULL);
    m_strip_text = json_obj_get_string(obj, "meta_strip_text", NULL);
    date_src = json_obj_get_string(obj, "meta_date_source", NULL);

    if (self->quick_mode) {
        if (!self->has_quick_meta) {
            g_autofree char *cur_src = stampy_config_get_string(STAMPY_CONFIG_KEY_META_DATE_SOURCE, "auto");
            g_strlcpy(self->quick_meta.meta_date_source,
                      (cur_src && *cur_src) ? cur_src : "auto",
                      sizeof(self->quick_meta.meta_date_source));
        }
        self->quick_meta.title_experimental = legacy_meta;
        self->quick_meta.xlsx_time_exif = legacy_time;
        self->quick_meta.meta_title = m_title;
        self->quick_meta.meta_subject = m_subj;
        self->quick_meta.meta_tags = m_tags;
        self->quick_meta.meta_comments = m_comm;
        self->quick_meta.meta_date = m_date;
        self->quick_meta.meta_time = m_time;
        self->quick_meta.meta_strip_all_source = m_strip_all;
        self->quick_meta.meta_remove_camera = m_remove_cam;
        self->quick_meta.meta_remove_serial = m_remove_ser;
        self->quick_meta.meta_remove_software = m_remove_soft;
        self->quick_meta.meta_remove_author = m_remove_auth;
        self->quick_meta.meta_remove_image_id = m_remove_id;
        self->quick_meta.export_timephoto_rename = m_timephoto;
        g_free(self->quick_meta.meta_strip_text);
        self->quick_meta.meta_strip_text = g_strdup(m_strip_text ? m_strip_text : "");
        if (date_src && (g_strcmp0(date_src, "watermark") == 0 ||
                         g_strcmp0(date_src, "geolocation") == 0 ||
                         g_strcmp0(date_src, "auto") == 0)) {
            g_strlcpy(self->quick_meta.meta_date_source, date_src, sizeof(self->quick_meta.meta_date_source));
        }
        self->has_quick_meta = TRUE;
    } else {
        stampy_config_set_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, legacy_meta);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, legacy_time);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_TITLE, m_title);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_SUBJECT, m_subj);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_TAGS, m_tags);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_COMMENTS, m_comm);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_DATE, m_date);
        if (date_src && (g_strcmp0(date_src, "watermark") == 0 ||
                         g_strcmp0(date_src, "geolocation") == 0 ||
                         g_strcmp0(date_src, "auto") == 0)) {
            stampy_config_set_string(STAMPY_CONFIG_KEY_META_DATE_SOURCE, date_src);
        }
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_TIME, m_time);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_STRIP_ALL_SOURCE, m_strip_all);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_REMOVE_CAMERA, m_remove_cam);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_REMOVE_SERIAL, m_remove_ser);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_REMOVE_SOFTWARE, m_remove_soft);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_REMOVE_AUTHOR, m_remove_auth);
        stampy_config_set_bool(STAMPY_CONFIG_KEY_META_REMOVE_IMAGE_ID, m_remove_id);
        stampy_config_set_string(STAMPY_CONFIG_KEY_META_STRIP_TEXT, m_strip_text ? m_strip_text : "");
        stampy_config_set_bool(STAMPY_CONFIG_KEY_TIMEPHOTO_RENAME, m_timephoto);
    }

    /* Text alignment. */
    t_align = json_obj_get_string(obj, "t_align", "alignment");
    if (t_align && g_strcmp0(t_align, "left") == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_left), TRUE);
    else if (t_align && g_strcmp0(t_align, "center") == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_center), TRUE);
    else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_t_align_right), TRUE);

    /* Colors. */
    load_color_from_json(obj, "c_stroke_color", self->btn_c_stroke_color);
    load_color_from_json(obj, "c_shadow_color", self->btn_c_shadow_color);
    load_color_from_json(obj, "t_stroke_color", self->btn_t_stroke_color);
    load_color_from_json(obj, "t_shadow_color", self->btn_t_shadow_color);
    load_color_from_json(obj, "t_color", self->btn_t_color);

    /* Widths / Sliders */
    load_range_from_json(obj, "t_stroke_width", self->val_t_stroke_width, 2.5);
    load_range_from_json(obj, "t_shadow_width", self->val_t_shadow_width, 2.0);
    load_range_from_json(obj, "c_stroke_width", self->val_c_stroke_width, 2.0);
    load_range_from_json(obj, "c_shadow_width", self->val_c_shadow_width, 4.0);

    /* Sync compass folder from preset (overrides compass lock when present and valid). */
    compass_folder = json_obj_get_string(obj, "compass_folder", NULL);
    if (compass_folder && *compass_folder && g_file_test(compass_folder, G_FILE_TEST_IS_DIR)) {
        g_autofree char *basename = g_path_get_basename(compass_folder);
        g_clear_object(&self->file_compass);
        self->file_compass = g_file_new_for_path(compass_folder);
        if (self->lbl_compass && basename) {
            gtk_label_set_text(GTK_LABEL(self->lbl_compass), basename);
        }
        if (!self->quick_mode) {
            stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER, compass_folder);
        }
        stampy_update_clear_button_visibility(self);
    }

    /* Refresh the preview or notify Quick-Stamp UI. */
    if (!self->quick_mode) {
        stampy_render_live_preview(self);
    } else if (self->quick_banner_cb) {
        self->quick_banner_cb(self, self->quick_cb_data);
    }
}

void stampy_save_last_state(StampyWindow *self) {
    g_autofree char *path = NULL;
    GListModel *model;
    guint idx;
    const char *preset_name = "";

    if (!self || self->loading_state || self->quick_mode) return;

    path = g_build_filename(g_get_user_config_dir(), "stampy_last.json", NULL);

    /* Session data (last preset name). Preferences and the locked compass
       live in the config store, not here; see stampy-config.c. */
    if (self->drop_presets) {
        model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_presets));
        idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drop_presets));
        if (model && idx != GTK_INVALID_LIST_POSITION && GTK_IS_STRING_LIST(model)) {
            preset_name = gtk_string_list_get_string(GTK_STRING_LIST(model), idx);
        }
    }

    save_preset_json(self, path, FALSE, preset_name ? preset_name : "");
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
    if (!self || self->loading_state || self->quick_mode) return;
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
    g_autofree char *path = NULL;
    g_autoptr(JsonParser) parser = NULL;

    if (!self) return;

    path = g_build_filename(g_get_user_config_dir(), "stampy_last.json", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        self->loading_state = TRUE;
        restore_compass_from_config(self);
        self->loading_state = FALSE;
        return;
    }

    self->loading_state = TRUE;

    /* Apply the stored panel values. */
    stampy_load_preset_from_file(self, path);

    /* Restore the selected preset. */
    parser = json_parser_new();
    if (self->drop_presets && json_parser_load_from_file(parser, path, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            const char *preset_name = json_obj_get_string(obj, "last_preset", NULL);
            if (preset_name && *preset_name) {
                GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->drop_presets));
                guint n = g_list_model_get_n_items(model);
                guint i;
                for (i = 0; i < n; i++) {
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
    }

    /* Restore compass from stampy_prefs.json after loading session state so
       stampy_prefs.json remains the single source of truth across restarts. */
    restore_compass_from_config(self);

    /* Persist the restored state once. */
    self->loading_state = FALSE;
    if (!self->quick_mode) {
        stampy_save_last_state(self);
    }
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
        } else if (self->quick_mode && self->quick_banner_cb) {
            self->quick_banner_cb(self, self->quick_cb_data);
        }

        /* Persist the selection even without a preset file on disk
           (e.g. "Default Preset"). Suppressed during state load and quick mode. */
        if (!self->quick_mode) {
            stampy_save_last_state(self);
        }
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

static void preset_dialog_data_free(PresetDialogData *data) {
    if (!data) return;
    g_clear_object(&data->self);
    g_free(data);
}

static void on_rename_dialog_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    PresetDialogData *data = (PresetDialogData *)user_data;
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "save") == 0 && data && data->self && !data->self->disposed && data->entry) {
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
    preset_dialog_data_free(data);
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
    data->self = g_object_ref(self);
    data->entry = entry;

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_rename_dialog_response, data);
}

static void on_add_preset_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    PresetDialogData *data = (PresetDialogData *)user_data;
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "save") == 0 && data && data->self && !data->self->disposed && data->entry) {
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
    preset_dialog_data_free(data);
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
    data->self = g_object_ref(self);
    data->entry = entry;

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_add_preset_response, data);
}

static void on_delete_preset_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source);
    g_autoptr(StampyWindow) self = STAMPY_WINDOW(user_data);
    const char *response = adw_alert_dialog_choose_finish(dialog, res);

    if (g_strcmp0(response, "delete") == 0 && self && !self->disposed) {
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

    {
        g_autofree char *msg = g_strdup_printf("Are you sure you want to delete '%s'?", preset_name);
        dialog = ADW_DIALOG(adw_alert_dialog_new("Delete Preset?", msg));
    }

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", "Delete");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);

    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), NULL, on_delete_preset_response, g_object_ref(self));
}

static void on_preset_import_file_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    g_autoptr(StampyWindow) self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, NULL);

    if (file && self && !self->disposed) {
        g_autofree char *path = g_file_get_path(file);
        stampy_load_preset_from_file(self, path);
        stampy_show_toast(self, "Preset imported!");
    }
}

void on_btn_import_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GtkFileDialog) dialog = NULL;

    (void)btn;
    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Preset JSON");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_preset_import_file_opened, g_object_ref(self));
}

static void on_preset_export_file_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    g_autoptr(StampyWindow) self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(dialog, res, NULL);

    if (file && self && !self->disposed) {
        g_autofree char *path = g_file_get_path(file);
        stampy_save_preset_to_file(self, path);
        stampy_show_toast(self, "Preset exported!");
    }
}

void on_btn_export_preset_clicked(GtkButton *btn, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GtkFileDialog) dialog = NULL;
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
    gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_preset_export_file_saved, g_object_ref(self));
}
