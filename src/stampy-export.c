#include "stampy-window-private.h"
#include "engine/stampy-metadata.h"
#include <gexiv2/gexiv2.h>
#include <glib/gstdio.h>
#include <cairo.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    gboolean has_compass;
    int comp_w;
    double comp_opacity;
    double comp_margin_x;
    double comp_margin_y;
    guint comp_pos_idx;
    gboolean comp_shadow;

    double font_size;
    double text_opacity;
    double text_margin_x;
    double text_margin_y;
    guint text_pos_idx;
    int text_align;
    double line_spacing;
    char *font_family;
    cairo_font_slant_t font_slant;
    cairo_font_weight_t font_weight;
    gboolean text_shadow;
    gboolean text_stroke;
    GdkRGBA text_color;
    GdkRGBA stroke_color;

    int jpeg_quality;
    gboolean title_experimental;  /* inject DOCX text as title/desc metadata */
    gboolean xlsx_time_exif;      /* inject XLSX clock/jam time as EXIF DateTimeOriginal/Digitized */
    gboolean use_native_metadata; /* use in-memory gexiv2 instead of exiftool */
    gboolean batch_exiftool;      /* run exiftool once with an argfile (-@) */
    GFile *file_compass;
} ExportSettingsSnapshot;

typedef struct {
    StampyWindow *self;
    GFile *export_folder;
    ExportSettingsSnapshot snapshot;
    guint inhibit_cookie;
} ExportThreadData;

typedef struct {
    StampyWindow *self;
    double fraction;
    gboolean visible;
    char *message;
    gboolean is_toast;
    gboolean is_finished;
} UIProgressUpdateData;

/* Shared export context. Read-only during export, safe across threads. */
typedef struct {
    StampyWindow *self;
    char *folder_path;
    ExportSettingsSnapshot snapshot;
    guint total;
    volatile gint completed;   /* atomic progress counter */
    volatile gint success_count;
    volatile gint error_count;
    volatile gint warn_gps_missing;
    volatile gint warn_wm_missing;
    volatile gint warn_id_missing;
    FILE *argfile_fp;
    GMutex argfile_mutex;
    char *argfile_path;
    gpointer compass_idx;
} ExportWorkerContext;

/* One image to export. */
typedef struct {
    ExportWorkerContext *ctx;
    guint index;
} ExportWorkItem;

/* Rendering helpers. */

static int extract_id_from_filename(const char *filename) {
    return stampy_extract_id_from_filename(filename);
}

static void remove_image_marker_from_text(char **text_ptr, int photo_id) {
    char *text;
    char *start;
    char *newline_pos;

    if (!text_ptr || !*text_ptr || photo_id < 0) return;

    text = *text_ptr;
    start = text;

    while (*start && g_ascii_isspace(*start)) start++;

    newline_pos = strchr(start, '\n');
    if (newline_pos) {
        char *first_line = g_strndup(start, newline_pos - start);
        int line_id = extract_id_from_filename(first_line);

        if (line_id == photo_id) {
            char *new_text = g_strdup(newline_pos + 1);
            g_strchug(new_text);
            g_free(text);
            *text_ptr = new_text;
        }
        g_free(first_line);
    } else {
        if (extract_id_from_filename(start) == photo_id) {
            g_free(text);
            *text_ptr = NULL;
        }
    }
}

static char *clean_compass_string(const char *str) {
    GString *out;
    const char *p;

    if (!str) return g_strdup("");

    out = g_string_new("");
    for (p = str; *p; p++) {
        if (g_ascii_isspace(*p)) continue;
        if ((guchar)*p == 0xC2 && (guchar)*(p + 1) == 0xBA) {
            g_string_append(out, "°");
            p++;
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

typedef struct {
    char *degree_path[360];
    GdkPixbuf *degree_pb[360];
    int degree_pb_w[360];
    char *default_path;
    GdkPixbuf *default_pb;
    int default_pb_w;
    GMutex mutex;
} CompassIndex;

static CompassIndex *compass_index_new(const char *compass_dir) {
    CompassIndex *idx;
    GDir *dir;
    const char *filename;
    char *default_p;

    if (!compass_dir || !g_file_test(compass_dir, G_FILE_TEST_IS_DIR)) return NULL;

    idx = g_new0(CompassIndex, 1);
    g_mutex_init(&idx->mutex);
    dir = g_dir_open(compass_dir, 0, NULL);
    if (dir) {
        while ((filename = g_dir_read_name(dir)) != NULL) {
            char *clean_fn = clean_compass_string(filename);
            char *lower_fn = g_ascii_strdown(clean_fn, -1);

            if (g_str_has_suffix(lower_fn, ".png") || g_str_has_suffix(lower_fn, ".jpg") || g_str_has_suffix(lower_fn, ".jpeg")) {
                int extracted = extract_id_from_filename(filename);
                if (extracted >= 0) {
                    int file_deg = (extracted % 360 + 360) % 360;
                    if (!idx->degree_path[file_deg]) {
                        idx->degree_path[file_deg] = g_build_filename(compass_dir, filename, NULL);
                    }
                }
            }
            g_free(clean_fn);
            g_free(lower_fn);
        }
        g_dir_close(dir);
    }

    default_p = g_build_filename(compass_dir, "compass.png", NULL);
    if (g_file_test(default_p, G_FILE_TEST_EXISTS)) {
        idx->default_path = default_p;
    } else {
        g_free(default_p);
    }

    return idx;
}

static void compass_index_free(CompassIndex *idx) {
    int i;
    if (!idx) return;
    for (i = 0; i < 360; i++) {
        g_free(idx->degree_path[i]);
        if (idx->degree_pb[i]) g_object_unref(idx->degree_pb[i]);
    }
    g_free(idx->default_path);
    if (idx->default_pb) g_object_unref(idx->default_pb);
    g_mutex_clear(&idx->mutex);
    g_free(idx);
}

static GdkPixbuf *compass_index_load_pixbuf(CompassIndex *idx, const char *azimuth_str, int req_w) {
    int target_deg = 0;
    const char *best_path = NULL;
    int best_deg = -1;
    int offset;
    GdkPixbuf *res = NULL;

    if (!idx) return NULL;

    if (azimuth_str && *azimuth_str != '\0') {
        if (sscanf(azimuth_str, "%d", &target_deg) == 1) {
            target_deg = (target_deg % 360 + 360) % 360;
        }
    }

    for (offset = 0; offset <= 180; offset++) {
        int d1 = (target_deg + offset) % 360;
        int d2 = (target_deg - offset + 360) % 360;
        if (idx->degree_path[d1]) {
            best_path = idx->degree_path[d1];
            best_deg = d1;
            break;
        }
        if (idx->degree_path[d2]) {
            best_path = idx->degree_path[d2];
            best_deg = d2;
            break;
        }
    }

    g_mutex_lock(&idx->mutex);
    if (best_deg >= 0) {
        if (idx->degree_pb[best_deg] && idx->degree_pb_w[best_deg] == req_w) {
            res = g_object_ref(idx->degree_pb[best_deg]);
        } else if (best_path && g_file_test(best_path, G_FILE_TEST_EXISTS)) {
            if (idx->degree_pb[best_deg]) g_object_unref(idx->degree_pb[best_deg]);
            idx->degree_pb[best_deg] = gdk_pixbuf_new_from_file_at_scale(best_path, req_w, -1, TRUE, NULL);
            idx->degree_pb_w[best_deg] = req_w;
            if (idx->degree_pb[best_deg]) res = g_object_ref(idx->degree_pb[best_deg]);
        }
    } else if (idx->default_path && g_file_test(idx->default_path, G_FILE_TEST_EXISTS)) {
        if (idx->default_pb && idx->default_pb_w == req_w) {
            res = g_object_ref(idx->default_pb);
        } else {
            if (idx->default_pb) g_object_unref(idx->default_pb);
            idx->default_pb = gdk_pixbuf_new_from_file_at_scale(idx->default_path, req_w, -1, TRUE, NULL);
            idx->default_pb_w = req_w;
            if (idx->default_pb) res = g_object_ref(idx->default_pb);
        }
    }
    g_mutex_unlock(&idx->mutex);

    return res;
}

static GdkPixbuf *load_best_compass_pixbuf(const char *compass_dir, const char *azimuth_str, int req_w) {
    GDir *dir;
    const char *filename;
    char *best_filepath = NULL;
    char *clean_azimuth = NULL;
    int target_deg = 0;

    if (!compass_dir || !g_file_test(compass_dir, G_FILE_TEST_IS_DIR)) return NULL;

    if (azimuth_str && *azimuth_str != '\0') {
        clean_azimuth = clean_compass_string(azimuth_str);
        if (sscanf(azimuth_str, "%d", &target_deg) == 1) {
            target_deg = (target_deg % 360 + 360) % 360;
        }
    }

    dir = g_dir_open(compass_dir, 0, NULL);
    if (dir) {
        int min_diff = 360;

        while ((filename = g_dir_read_name(dir)) != NULL) {
            char *clean_fn = clean_compass_string(filename);
            char *lower_fn = g_ascii_strdown(clean_fn, -1);

            if (g_str_has_suffix(lower_fn, ".png") || g_str_has_suffix(lower_fn, ".jpg") || g_str_has_suffix(lower_fn, ".jpeg")) {
                int extracted;

                if (clean_azimuth && *clean_azimuth != '\0' && g_str_has_prefix(clean_fn, clean_azimuth)) {
                    g_free(best_filepath);
                    best_filepath = g_build_filename(compass_dir, filename, NULL);
                    g_free(clean_fn);
                    g_free(lower_fn);
                    break;
                }

                extracted = extract_id_from_filename(filename);
                if (extracted >= 0) {
                    int file_deg = (extracted % 360 + 360) % 360;
                    int diff = abs(target_deg - file_deg);
                    if (diff > 180) diff = 360 - diff;

                    if (diff < min_diff) {
                        min_diff = diff;
                        g_free(best_filepath);
                        best_filepath = g_build_filename(compass_dir, filename, NULL);
                    }
                }
            }
            g_free(clean_fn);
            g_free(lower_fn);
        }
        g_dir_close(dir);
    }

    g_free(clean_azimuth);

    if (!best_filepath) {
        char *default_p = g_build_filename(compass_dir, "compass.png", NULL);
        if (g_file_test(default_p, G_FILE_TEST_EXISTS)) {
            best_filepath = default_p;
        } else {
            g_free(default_p);
        }
    }

    if (best_filepath && g_file_test(best_filepath, G_FILE_TEST_EXISTS)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(best_filepath, req_w, -1, TRUE, NULL);
        g_free(best_filepath);
        return pb;
    }

    g_free(best_filepath);
    return NULL;
}

static void snapshot_export_settings(StampyWindow *window_self, ExportSettingsSnapshot *snap) {
    memset(snap, 0, sizeof(ExportSettingsSnapshot));

    if (!window_self) return;

    snap->jpeg_quality = stampy_get_jpeg_quality(window_self);

    if (window_self->file_compass) {
        snap->file_compass = g_object_ref(window_self->file_compass);
    }

    if (window_self->val_compass_size) {
        snap->has_compass = TRUE;
        snap->comp_w = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_size));
        snap->comp_opacity = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_opacity)) / 100.0;
        snap->comp_margin_x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_margin_x));
        snap->comp_margin_y = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_margin_y));
        snap->comp_pos_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(window_self->drop_compass_pos));
        snap->comp_shadow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_c_shadow));
    }

    snap->title_experimental = stampy_config_get_bool(STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, FALSE);
    snap->xlsx_time_exif = stampy_config_get_bool(STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, FALSE) ||
                           (window_self->btn_xlsx_time_exif &&
                            gtk_switch_get_active(GTK_SWITCH(window_self->btn_xlsx_time_exif)));
    snap->use_native_metadata = stampy_config_get_bool(STAMPY_CONFIG_KEY_USE_NATIVE_METADATA, FALSE);
    snap->batch_exiftool = stampy_config_get_bool(STAMPY_CONFIG_KEY_BATCH_EXIFTOOL, FALSE);

    if (window_self->val_text_size) {
        GListModel *model;
        guint sel;

        snap->font_size = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_size));
        snap->text_opacity = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_opacity)) / 100.0;
        snap->text_margin_x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_margin_x));
        snap->text_margin_y = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_margin_y));
        snap->text_pos_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(window_self->drop_text_pos));

        if (window_self->val_text_spacing) {
            snap->line_spacing = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_spacing));
        } else {
            snap->line_spacing = 12.0;
        }

        if (window_self->btn_t_align_left && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_align_left))) {
            snap->text_align = 0;
        } else if (window_self->btn_t_align_center && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_align_center))) {
            snap->text_align = 1;
        } else {
            snap->text_align = 2;
        }

        model = gtk_drop_down_get_model(GTK_DROP_DOWN(window_self->drop_text_font));
        sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(window_self->drop_text_font));
        if (sel != GTK_INVALID_LIST_POSITION && model) {
            snap->font_family = g_strdup(gtk_string_list_get_string(GTK_STRING_LIST(model), sel));
        } else {
            snap->font_family = g_strdup("Arial");
        }

        snap->font_slant = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_italic)) ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL;
        snap->font_weight = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_bold)) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;
        snap->text_shadow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_shadow));
        snap->text_stroke = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_t_stroke));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(window_self->btn_t_color), &snap->text_color);
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(window_self->btn_t_stroke_color), &snap->stroke_color);
#pragma GCC diagnostic pop
    }
}

static void free_export_snapshot(ExportSettingsSnapshot *snap) {
    if (!snap) return;
    if (snap->file_compass) g_object_unref(snap->file_compass);
    if (snap->font_family) g_free(snap->font_family);
}

static GdkPixbuf *render_stamped_on_pixbuf(GdkPixbuf *pixbuf, const ExportSettingsSnapshot *snap, const char *in_custom_text, const char *in_azimuth_str, CompassIndex *cidx) {
    cairo_surface_t *surface;
    cairo_t *cr;
    int width, height;
    double scale;
    char *custom_text;
    char *azimuth_str;
    GdkPixbuf *res_pixbuf;

    if (!pixbuf) return NULL;

    custom_text = (in_custom_text && *in_custom_text) ? g_strdup(in_custom_text) : NULL;
    azimuth_str = (in_azimuth_str && *in_azimuth_str) ? g_strdup(in_azimuth_str) : NULL;

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);

    scale = (double)(width < height ? width : height) / 500.0;
    if (scale < 1.0) scale = 1.0;

    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cr = cairo_create(surface);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
#pragma GCC diagnostic pop
    cairo_paint(cr);

    /* Compass overlay. */
    if (snap->has_compass && snap->file_compass) {
        int comp_w = (int)(snap->comp_w * scale);
        double opacity = snap->comp_opacity;
        int margin_x = (int)(snap->comp_margin_x * scale);
        int margin_y = (int)(snap->comp_margin_y * scale);
        guint pos_idx = snap->comp_pos_idx;

        char *compass_dir = NULL;
        GdkPixbuf *comp_pb = NULL;

        if (cidx) {
            comp_pb = compass_index_load_pixbuf(cidx, azimuth_str, comp_w);
        } else {
            compass_dir = g_file_get_path(snap->file_compass);
            if (compass_dir) {
                comp_pb = load_best_compass_pixbuf(compass_dir, azimuth_str, comp_w);
                g_free(compass_dir);
            }
        }

        if (comp_pb) {
            int comp_h = gdk_pixbuf_get_height(comp_pb);
            int cx, cy;

            if (pos_idx == 0) { cx = margin_x; cy = margin_y; }
            else if (pos_idx == 1) { cx = width - comp_w - margin_x; cy = margin_y; }
            else if (pos_idx == 2) { cx = margin_x; cy = height - comp_h - margin_y; }
            else { cx = width - comp_w - margin_x; cy = height - comp_h - margin_y; }

            cairo_save(cr);

            if (snap->comp_shadow) {
                cairo_set_source_rgba(cr, 0, 0, 0, opacity * 0.6);
                cairo_rectangle(cr, cx + (4 * scale), cy + (4 * scale), comp_w, comp_h);
                cairo_fill(cr);
            }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            gdk_cairo_set_source_pixbuf(cr, comp_pb, cx, cy);
#pragma GCC diagnostic pop
            cairo_paint_with_alpha(cr, opacity);
            cairo_restore(cr);
            g_object_unref(comp_pb);
        }
    }

    /* Text overlay. */
    if (snap->font_size > 0 && custom_text && *custom_text != '\0') {
        double font_size = snap->font_size * scale;
        double opacity = snap->text_opacity;
        int margin_x = (int)(snap->text_margin_x * scale);
        int margin_y = (int)(snap->text_margin_y * scale);
        guint pos_idx = snap->text_pos_idx;

        const char *font_family = snap->font_family ? snap->font_family : "Arial";
        char **raw_lines;
        char **lines;
        guint raw_len;
        guint valid_n_lines = 0;
        guint i;
        GdkRGBA tc = snap->text_color;
        GdkRGBA sc = snap->stroke_color;
        cairo_font_extents_t fext;
        double max_line_w = 0.0;
        double line_spacing = snap->line_spacing * scale;
        double pad_right = 0.0, pad_bottom = 0.0, pad_left = 0.0, pad_top = 0.0;
        double left_bound, right_bound;

        char *clean_text = g_strdup(custom_text);
        char *p = clean_text, *q = clean_text;
        while (*p) {
            if (*p != '\r') *q++ = *p;
            p++;
        }
        *q = '\0';
        g_strchomp(clean_text);

        raw_lines = g_strsplit(clean_text, "\n", -1);
        raw_len = g_strv_length(raw_lines);
        lines = g_new0(char *, raw_len + 1);

        for (i = 0; i < raw_len; i++) {
            g_strstrip(raw_lines[i]);
            if (raw_lines[i][0] != '\0') {
                lines[valid_n_lines++] = raw_lines[i];
            }
        }

        if (valid_n_lines > 0) {
            cairo_save(cr);
            cairo_select_font_face(cr, font_family, snap->font_slant, snap->font_weight);
            cairo_set_font_size(cr, font_size);
            cairo_font_extents(cr, &fext);

            for (i = 0; i < valid_n_lines; i++) {
                cairo_text_extents_t ext;
                cairo_text_extents(cr, lines[i], &ext);
                if (ext.width > max_line_w) max_line_w = ext.width;
            }

            if (snap->text_shadow) {
                pad_right = 2.0 * scale;
                pad_bottom = 2.0 * scale;
            }
            if (snap->text_stroke) {
                double stroke_r = (2.5 * scale) / 2.0;
                if (stroke_r > pad_right) pad_right = stroke_r;
                if (stroke_r > pad_bottom) pad_bottom = stroke_r;
                pad_left = stroke_r;
                pad_top = stroke_r;
            }

            left_bound = margin_x + pad_left;
            right_bound = width - margin_x - pad_right;

            for (i = 0; i < valid_n_lines; i++) {
                cairo_text_extents_t ext;
                double tx, ty;

                cairo_text_extents(cr, lines[i], &ext);

                if (pos_idx == 1 || pos_idx == 3) {
                    if (snap->text_align == 2) tx = right_bound - (ext.x_bearing + ext.width);
                    else if (snap->text_align == 1) tx = right_bound - max_line_w + (max_line_w - ext.width) / 2.0 - ext.x_bearing;
                    else tx = right_bound - max_line_w - ext.x_bearing;
                } else {
                    if (snap->text_align == 0) tx = left_bound - ext.x_bearing;
                    else if (snap->text_align == 1) tx = left_bound + (max_line_w - ext.width) / 2.0 - ext.x_bearing;
                    else tx = left_bound + max_line_w - (ext.x_bearing + ext.width);
                }

                if (pos_idx == 0 || pos_idx == 1) {
                    double top_y = margin_y + fext.ascent + pad_top;
                    ty = top_y + (i * (fext.height + line_spacing));
                } else {
                    double bottom_y = height - margin_y - fext.descent - pad_bottom;
                    ty = bottom_y - ((valid_n_lines - 1 - i) * (fext.height + line_spacing));
                }

                if (snap->text_shadow) {
                    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, opacity * 0.7);
                    cairo_move_to(cr, tx + (2.0 * scale), ty + (2.0 * scale));
                    cairo_show_text(cr, lines[i]);
                }

                if (snap->text_stroke) {
                    cairo_move_to(cr, tx, ty);
                    cairo_text_path(cr, lines[i]);
                    cairo_set_source_rgba(cr, sc.red, sc.green, sc.blue, opacity);
                    cairo_set_line_width(cr, 2.5 * scale);
                    cairo_stroke(cr);
                }

                cairo_move_to(cr, tx, ty);
                cairo_set_source_rgba(cr, tc.red, tc.green, tc.blue, opacity);
                cairo_show_text(cr, lines[i]);
            }
            cairo_restore(cr);
        }

        g_free(lines);
        g_strfreev(raw_lines);
        g_free(clean_text);
    }

    g_free(custom_text);
    g_free(azimuth_str);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    res_pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
#pragma GCC diagnostic pop

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return res_pixbuf;
}

static GdkPixbuf *render_stamped_pixbuf_from_snapshot(GFile *image_file, const ExportSettingsSnapshot *snap, const char *in_custom_text, const char *in_azimuth_str, CompassIndex *cidx) {
    char *path;
    GdkPixbuf *raw_pixbuf;
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *res = NULL;

    if (!image_file) return NULL;

    path = g_file_get_path(image_file);
    if (!path) return NULL;

    raw_pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    g_free(path);
    if (!raw_pixbuf) return NULL;

    pixbuf = gdk_pixbuf_apply_embedded_orientation(raw_pixbuf);
    g_object_unref(raw_pixbuf);
    if (!pixbuf) return NULL;

    res = render_stamped_on_pixbuf(pixbuf, snap, in_custom_text, in_azimuth_str, cidx);
    g_object_unref(pixbuf);

    return res;
}

void stampy_render_live_preview(StampyWindow *window_self) {
    GList *selected_rows;
    ExportSettingsSnapshot snap;

    if (!window_self || !window_self->file_listbox || !window_self->image_files) return;

    selected_rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(window_self->file_listbox));
    if (g_list_length(selected_rows) == 1) {
        int idx = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(selected_rows->data));
        if (idx >= 0 && idx < (int)window_self->image_files->len) {
            GFile *file = g_ptr_array_index(window_self->image_files, idx);
            if (file) {
                GdkPixbuf *stamped = NULL;
                char *orig_basename = g_file_get_basename(file);
                char *file_path = g_file_get_path(file);
                char *custom_text = NULL;
                char *azimuth_str = NULL;

                if (file_path) {
                    if (!window_self->preview_cached_pixbuf ||
                        !window_self->preview_cached_path ||
                        strcmp(window_self->preview_cached_path, file_path) != 0) {
                        GdkPixbuf *raw = gdk_pixbuf_new_from_file_at_scale(file_path, 1600, 1600, TRUE, NULL);
                        if (!raw) raw = gdk_pixbuf_new_from_file(file_path, NULL);
                        if (raw) {
                            if (window_self->preview_cached_pixbuf)
                                g_object_unref(window_self->preview_cached_pixbuf);
                            window_self->preview_cached_pixbuf = gdk_pixbuf_apply_embedded_orientation(raw);
                            g_object_unref(raw);
                            g_free(window_self->preview_cached_path);
                            window_self->preview_cached_path = g_strdup(file_path);
                        }
                    }
                    g_free(file_path);
                }

                if (orig_basename && *orig_basename != '\0') {
                    int photo_id = extract_id_from_filename(orig_basename);

                    if (window_self->file_watermark) {
                        if (stampy_lookup_docx_data(window_self, orig_basename, &custom_text, &azimuth_str)) {
                            remove_image_marker_from_text(&custom_text, photo_id);
                        }
                    }

                    if (photo_id > 0 && window_self->file_geolocation && window_self->geo_table) {
                        GeolocationEntry geo_entry;
                        if (stampy_lookup_geolocation_data(window_self, photo_id, &geo_entry)) {
                            char lat_str[32], lon_str[32];
                            double compass_deg = 0.0;
                            format_geolocation_text(&geo_entry, lat_str, lon_str, &compass_deg);

                            if (!custom_text) custom_text = g_strdup_printf("%s %s", lat_str, lon_str);
                            if (!azimuth_str) azimuth_str = g_strdup_printf("%.2f", compass_deg);
                        }
                    }
                }

                snapshot_export_settings(window_self, &snap);
                if (window_self->preview_cached_pixbuf) {
                    stamped = render_stamped_on_pixbuf(window_self->preview_cached_pixbuf, &snap, custom_text, azimuth_str, NULL);
                } else {
                    stamped = render_stamped_pixbuf_from_snapshot(file, &snap, custom_text, azimuth_str, NULL);
                }

                if (stamped) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                    GdkTexture *texture = gdk_texture_new_for_pixbuf(stamped);
#pragma GCC diagnostic pop
                    gtk_picture_set_paintable(GTK_PICTURE(window_self->img_preview), GDK_PAINTABLE(texture));
                    g_object_unref(texture);
                    g_object_unref(stamped);
                }
                free_export_snapshot(&snap);
                g_free(orig_basename);
                g_free(custom_text);
                g_free(azimuth_str);
            }
        }
    }
    g_list_free(selected_rows);
}

/* Cancellable background export engine. */

static gboolean update_ui_progress_idle(gpointer user_data) {
    UIProgressUpdateData *data = (UIProgressUpdateData *)user_data;
    if (!data) return G_SOURCE_REMOVE;

    if (data->self) {
        stampy_set_progress(data->self, data->fraction, data->visible);

        if (data->visible && data->fraction > 0.0 && !data->is_finished) {
            char *title = g_strdup_printf("Exporting… %d%%",
                                          (int)(data->fraction * 100.0 + 0.5));
            gtk_window_set_title(GTK_WINDOW(data->self), title);
            g_free(title);
        }

        if (data->message) {
            if (data->is_toast) {
                stampy_show_toast(data->self, data->message);
            } else if (strstr(data->message, "photos processed") != NULL) {
                /* Live progress reuses one terminal line instead of scrolling. */
                stampy_update_status_line(data->self, data->message);
            } else {
                stampy_append_log(data->self, data->message);
            }
        }

        if (data->is_finished) {
            data->self->export_active = FALSE;
            gtk_window_set_title(GTK_WINDOW(data->self), "Stampy");
            if (data->self->btn_header_export) gtk_widget_set_visible(data->self->btn_header_export, TRUE);
            if (data->self->btn_header_cancel) gtk_widget_set_visible(data->self->btn_header_cancel, FALSE);
            if (data->self->box_right_panel) gtk_widget_set_sensitive(data->self->box_right_panel, TRUE);
            if (data->self->file_listbox) gtk_widget_set_sensitive(data->self->file_listbox, TRUE);
        }
    }

    g_free(data->message);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void send_progress_to_ui(StampyWindow *window_self, double fraction, gboolean visible, const char *log_msg, gboolean is_toast, gboolean is_finished) {
    UIProgressUpdateData *data = g_new0(UIProgressUpdateData, 1);
    data->self = window_self;
    data->fraction = fraction;
    data->visible = visible;
    data->message = log_msg ? g_strdup(log_msg) : NULL;
    data->is_toast = is_toast;
    data->is_finished = is_finished;
    g_idle_add(update_ui_progress_idle, data);
}

static gchar *format_double_safe(double val, int decimals) {
    char buf[128];
    char *p;
    snprintf(buf, sizeof(buf), "%.*f", decimals, val);
    for (p = buf; *p; p++) {
        if (*p == ',') *p = '.';
    }
    return g_strdup(buf);
}

static void set_gexiv2_xp_tag(GExiv2Metadata *meta, const char *tag, const char *utf8_text) {
    glong items_written = 0;
    guint16 *u16;
    GString *bstr;
    glong i;
    guint16 val;
    guint8 b0;
    guint8 b1;

    if (!meta || !tag || !utf8_text || *utf8_text == '\0') return;

    u16 = g_utf8_to_utf16(utf8_text, -1, NULL, &items_written, NULL);
    if (!u16) return;

    bstr = g_string_sized_new((items_written + 1) * 8);
    for (i = 0; i <= items_written; i++) {
        val = (i < items_written) ? u16[i] : 0;
        b0 = (guint8)(val & 0xFF);
        b1 = (guint8)((val >> 8) & 0xFF);
        if (i > 0) g_string_append_c(bstr, ' ');
        g_string_append_printf(bstr, "%u %u", (unsigned int)b0, (unsigned int)b1);
    }

    gexiv2_metadata_try_set_tag_string(meta, tag, bstr->str, NULL);
    g_string_free(bstr, TRUE);
    g_free(u16);
}

static GExiv2Metadata *stampy_gexiv2_open_source(const char *src_path, GError **error) {
    GExiv2Metadata *meta = gexiv2_metadata_new();
    GError *open_err = NULL;

    if (gexiv2_metadata_open_path(meta, src_path, &open_err)) {
        return meta;
    }

    /* Fallback for JPEGs with corrupted/non-standard non-EXIF segments (e.g. invalid ICC profiles):
       Extract the APP1 EXIF segment directly and load it via gexiv2_metadata_from_app1_segment. */
    if (src_path) {
        gchar *contents = NULL;
        gsize length = 0;
        if (g_file_get_contents(src_path, &contents, &length, NULL)) {
            const guint8 *data = (const guint8 *)contents;
            if (length > 4 && data[0] == 0xFF && data[1] == 0xD8) {
                gsize pos = 2;
                while (pos + 4 <= length) {
                    guint8 marker;
                    guint16 seg_len;
                    if (data[pos] != 0xFF) break;
                    marker = data[pos + 1];
                    if (marker == 0xD8 || marker == 0xD9 || marker == 0x00) {
                        pos += 1;
                        continue;
                    }
                    seg_len = (((guint16)data[pos + 2]) << 8) | data[pos + 3];
                    if (seg_len < 2 || pos + 2 + seg_len > length) break;
                    if (marker == 0xE1) {
                        const guint8 *payload = data + pos + 2;
                        if (seg_len >= 8 && memcmp(payload + 2, "Exif\0\0", 6) == 0) {
                            GError *app1_err = NULL;
                            GExiv2Metadata *app1_meta = gexiv2_metadata_new();
                            if (gexiv2_metadata_from_app1_segment(app1_meta, payload, seg_len, &app1_err)) {
                                g_clear_error(&open_err);
                                g_free(contents);
                                g_object_unref(meta);
                                return app1_meta;
                            }
                            g_clear_error(&app1_err);
                            g_object_unref(app1_meta);
                        }
                    }
                    pos += 2 + seg_len;
                }
            }
            g_free(contents);
        }
    }

    if (error && open_err) {
        *error = open_err;
    } else if (open_err) {
        g_clear_error(&open_err);
    }
    g_object_unref(meta);
    return NULL;
}

static void stampy_ensure_standard_exif_tags(GExiv2Metadata *meta) {
    if (!meta) return;
    if (!gexiv2_metadata_try_has_tag(meta, "Exif.Photo.ColorSpace", NULL)) {
        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.ColorSpace", "65535", NULL);
    }
    if (!gexiv2_metadata_try_has_tag(meta, "Exif.Photo.ExifVersion", NULL)) {
        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.ExifVersion", "48 50 51 50", NULL);
    }
    if (!gexiv2_metadata_try_has_tag(meta, "Exif.Photo.ComponentsConfiguration", NULL)) {
        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.ComponentsConfiguration", "1 2 3 0", NULL);
    }
    if (!gexiv2_metadata_try_has_tag(meta, "Exif.Photo.FlashpixVersion", NULL)) {
        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.FlashpixVersion", "48 49 48 48", NULL);
    }
}

static void stampy_set_gps_coordinates(GExiv2Metadata *meta, double lat, double lon) {
    double abs_lat;
    double abs_lon;
    char lat_ref;
    char lon_ref;
    int lat_deg;
    double lat_rem1;
    int lat_min;
    double lat_rem2;
    long long lat_sec_num;
    int lon_deg;
    double lon_rem1;
    int lon_min;
    double lon_rem2;
    long long lon_sec_num;
    char buf[128];
    char ref_buf[2];

    if (!meta) return;

    abs_lat = fabs(lat);
    abs_lon = fabs(lon);
    lat_ref = (lat >= 0.0) ? 'N' : 'S';
    lon_ref = (lon >= 0.0) ? 'E' : 'W';

    lat_deg = (int)floor(abs_lat);
    lat_rem1 = (abs_lat - (double)lat_deg) * 60.0;
    lat_min = (int)floor(lat_rem1);
    lat_rem2 = (lat_rem1 - (double)lat_min) * 60.0;
    lat_sec_num = (long long)llround(lat_rem2 * 10000000.0);

    lon_deg = (int)floor(abs_lon);
    lon_rem1 = (abs_lon - (double)lon_deg) * 60.0;
    lon_min = (int)floor(lon_rem1);
    lon_rem2 = (lon_rem1 - (double)lon_min) * 60.0;
    lon_sec_num = (long long)llround(lon_rem2 * 10000000.0);

    /* EXIF GPS Tags */
    snprintf(buf, sizeof(buf), "%d/1 %d/1 %lld/10000000", lat_deg, lat_min, lat_sec_num);
    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSLatitude", buf, NULL);

    ref_buf[0] = lat_ref;
    ref_buf[1] = '\0';
    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSLatitudeRef", ref_buf, NULL);

    snprintf(buf, sizeof(buf), "%d/1 %d/1 %lld/10000000", lon_deg, lon_min, lon_sec_num);
    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSLongitude", buf, NULL);

    ref_buf[0] = lon_ref;
    ref_buf[1] = '\0';
    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSLongitudeRef", ref_buf, NULL);

    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSVersionID", "2 3 0 0", NULL);
    gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSMapDatum", "WGS-84", NULL);

    /* XMP GPS Tags */
    snprintf(buf, sizeof(buf), "%d,%.6f%c", lat_deg, lat_rem1, lat_ref);
    gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSLatitude", buf, NULL);

    snprintf(buf, sizeof(buf), "%d,%.6f%c", lon_deg, lon_rem1, lon_ref);
    gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSLongitude", buf, NULL);

    gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSVersionID", "2.3.0.0", NULL);
    gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSMapDatum", "WGS-84", NULL);
}

static gchar *stampy_resolve_timezone_offset(GExiv2Metadata *meta) {
    gchar *tz_offset = NULL;

    if (meta) {
        tz_offset = gexiv2_metadata_try_get_tag_string(meta, "Exif.Photo.OffsetTimeOriginal", NULL);
        if (!tz_offset)
            tz_offset = gexiv2_metadata_try_get_tag_string(meta, "Exif.Photo.OffsetTime", NULL);
        if (!tz_offset)
            tz_offset = gexiv2_metadata_try_get_tag_string(meta, "Exif.Photo.OffsetTimeDigitized", NULL);

        if (!tz_offset) {
            gchar *xmp_orig = gexiv2_metadata_try_get_tag_string(meta, "Xmp.exif.DateTimeOriginal", NULL);
            if (!xmp_orig)
                xmp_orig = gexiv2_metadata_try_get_tag_string(meta, "Xmp.xmp.CreateDate", NULL);
            if (xmp_orig) {
                gsize len = strlen(xmp_orig);
                if (len >= 6 && (xmp_orig[len - 6] == '+' || xmp_orig[len - 6] == '-') && xmp_orig[len - 3] == ':') {
                    tz_offset = g_strdup(xmp_orig + len - 6);
                }
                g_free(xmp_orig);
            }
        }
    }

    if (!tz_offset) {
        GDateTime *now = g_date_time_new_now_local();
        if (now) {
            tz_offset = g_date_time_format(now, "%:z");
            g_date_time_unref(now);
        }
    }

    if (!tz_offset) {
        tz_offset = g_strdup("+07:00");
    }

    return tz_offset;
}

static char *get_exiftool_cmd(void) {
    const char *configured = g_getenv("STAMPY_EXIFTOOL");
    if (configured && *configured) return g_strdup(configured);
    if (g_file_test("/app/bin/perl", G_FILE_TEST_IS_EXECUTABLE) &&
        g_file_test("/app/bin/exiftool", G_FILE_TEST_EXISTS)) {
        return g_strdup("/app/bin/perl /app/bin/exiftool");
    }
    return g_strdup("exiftool");
}

/* Exports one image on a thread-pool worker. */
static void export_image_worker(gpointer data, gpointer user_data) {
    ExportWorkItem *item = (ExportWorkItem *)data;
    ExportWorkerContext *ctx = (ExportWorkerContext *)user_data;
    StampyWindow *window_self = ctx->self;
    guint i = item->index;
    guint total = ctx->total;

    GFile *file;
    char *orig_basename = NULL;
    char *out_path = NULL;
    GeolocationEntry geo_entry;
    gboolean has_geo = FALSE;
    GdkPixbuf *stamped = NULL;
    int photo_id;
    char *custom_text = NULL;
    char *azimuth_str = NULL;
    double clean_heading_val = -1.0;
    int done;

    if (g_atomic_int_get(&window_self->export_cancel_flag) == 1) {
        g_free(item);
        return;
    }

    file = g_ptr_array_index(window_self->image_files, i);
    if (!file) { g_free(item); return; }

    orig_basename = g_file_get_basename(file);
    if (!orig_basename || *orig_basename == '\0') {
        g_free(orig_basename);
        g_free(item);
        return;
    }

    out_path = g_build_filename(ctx->folder_path, orig_basename, NULL);
    photo_id = extract_id_from_filename(orig_basename);

    if (photo_id > 0) {
        if (window_self->file_watermark) {
            if (stampy_lookup_docx_data(window_self, orig_basename, &custom_text, &azimuth_str)) {
                remove_image_marker_from_text(&custom_text, photo_id);
            } else {
                g_atomic_int_add(&ctx->warn_wm_missing, 1);
            }
        }

        if (window_self->file_geolocation && window_self->geo_table) {
            has_geo = stampy_lookup_geolocation_data(window_self, photo_id, &geo_entry);
            if (has_geo) {
                char lat_str[32], lon_str[32];
                double compass_deg = 0.0;
                format_geolocation_text(&geo_entry, lat_str, lon_str, &compass_deg);
                clean_heading_val = compass_deg;
                if (!custom_text) custom_text = g_strdup_printf("%s %s", lat_str, lon_str);
                if (!azimuth_str) azimuth_str = g_strdup_printf("%.2f", compass_deg);
            } else {
                g_atomic_int_add(&ctx->warn_gps_missing, 1);
            }
        }
    } else {
        g_atomic_int_add(&ctx->warn_id_missing, 1);
    }

    stamped = render_stamped_pixbuf_from_snapshot(file, &ctx->snapshot, custom_text, azimuth_str, (CompassIndex *)ctx->compass_idx);

    if (stamped) {
        GError *error = NULL;
        gboolean saved = FALSE;
        char *lower_name = g_ascii_strdown(orig_basename, -1);
        char *src_path = g_file_get_path(file);

        if (lower_name && g_str_has_suffix(lower_name, ".png")) {
            saved = gdk_pixbuf_save(stamped, out_path, "png", &error, NULL);
        } else {
            char *quality_str = g_strdup_printf("%d", ctx->snapshot.jpeg_quality > 0 ? ctx->snapshot.jpeg_quality : 90);
            saved = gdk_pixbuf_save(stamped, out_path, "jpeg", &error, "quality", quality_str, NULL);
            g_free(quality_str);
        }
        g_free(lower_name);

        if (saved) {
            char *desc_text = NULL;
            char *docx_date = NULL;
            char time_part[16] = {0};
            char new_dt[32] = {0};
            char iptc_date[16] = {0};
            char tz_sys[16] = {0};
            char iso_dt_common[64] = {0};
            char iptc_time_common[32] = {0};
            GExiv2Metadata *native_meta = NULL;
            GError *native_gex_err = NULL;

            g_atomic_int_add(&ctx->success_count, 1);

            if (ctx->snapshot.use_native_metadata) {
                native_meta = stampy_gexiv2_open_source(src_path, &native_gex_err);
            }

            /* Extract project description if enabled */
            if (ctx->snapshot.title_experimental && custom_text && *custom_text != '\0') {
                stampy_extract_project_description(custom_text, &desc_text);
            }

            /* Extract date and time if enabled */
            if (ctx->snapshot.xlsx_time_exif && has_geo) {
                int total_seconds, hours, minutes, seconds;
                GDateTime *now_dt;
                gchar *fmt_tz;

                stampy_extract_docx_date(custom_text, &docx_date);

                if (!docx_date && src_path && *src_path) {
                    if (native_meta) {
                        gchar *existing_dt = gexiv2_metadata_try_get_tag_string(native_meta, "Exif.Photo.DateTimeOriginal", NULL);
                        if (!existing_dt)
                            existing_dt = gexiv2_metadata_try_get_tag_string(native_meta, "Exif.Image.DateTime", NULL);
                        if (existing_dt) {
                            char existing_date[16] = {0};
                            if (sscanf(existing_dt, "%15[^ ]", existing_date) == 1 && strlen(existing_date) == 10) {
                                docx_date = g_strdup(existing_date);
                            }
                            g_free(existing_dt);
                        }
                    }
                    if (!docx_date && !ctx->snapshot.use_native_metadata) {
                        char *exif_cmd_fallback = get_exiftool_cmd();
                        char *quoted_src_dt = g_shell_quote(src_path);
                        char *read_dt_cmd = g_strdup_printf("%s -DateTimeOriginal -s3 %s", exif_cmd_fallback, quoted_src_dt);
                        FILE *fp_dt = popen(read_dt_cmd, "r");
                        char dt_buf[64] = {0};
                        if (fp_dt) {
                            if (fgets(dt_buf, sizeof(dt_buf), fp_dt)) {
                                char existing_date[16] = {0};
                                if (sscanf(dt_buf, "%15[^ ]", existing_date) == 1 && strlen(existing_date) == 10) {
                                    docx_date = g_strdup(existing_date);
                                }
                            }
                            pclose(fp_dt);
                        }
                        g_free(read_dt_cmd);
                        g_free(quoted_src_dt);
                        g_free(exif_cmd_fallback);
                    }
                }

                if (geo_entry.time_val > 0.0) {
                    total_seconds = (int)(geo_entry.time_val * 86400.0 + 0.5);
                    total_seconds = (total_seconds % 86400 + 86400) % 86400;
                    hours = total_seconds / 3600;
                    minutes = (total_seconds % 3600) / 60;
                    seconds = total_seconds % 60;
                    snprintf(time_part, sizeof(time_part), "%02d:%02d:%02d", hours, minutes, seconds);
                } else {
                    /* No time column: default to midnight so EXIF datetime stays valid */
                    snprintf(time_part, sizeof(time_part), "00:00:00");
                }

                if (docx_date && strlen(docx_date) == 10) {
                    now_dt = g_date_time_new_now_local();
                    if (now_dt) {
                        fmt_tz = g_date_time_format(now_dt, "%:z");
                        if (fmt_tz) {
                            g_strlcpy(tz_sys, fmt_tz, sizeof(tz_sys));
                            g_free(fmt_tz);
                        }
                        g_date_time_unref(now_dt);
                    }
                    if (tz_sys[0] == '\0') {
                        g_strlcpy(tz_sys, "+07:00", sizeof(tz_sys));
                    }

                    /* EXIF date/time: "YYYY:MM:DD HH:MM:SS" (colons). */
                    snprintf(new_dt, sizeof(new_dt), "%s %s", docx_date, time_part);
                    /* IPTC date: "YYYY-MM-DD" (dashes). */
                    snprintf(iptc_date, sizeof(iptc_date), "%.4s-%.2s-%.2s",
                             docx_date, docx_date + 5, docx_date + 8);
                    /* ISO-8601 date-time: "YYYY-MM-DDTHH:MM:SS+07:00" */
                    snprintf(iso_dt_common, sizeof(iso_dt_common), "%.4s-%.2s-%.2sT%s%s",
                             docx_date, docx_date + 5, docx_date + 8, time_part, tz_sys);
                    snprintf(iptc_time_common, sizeof(iptc_time_common), "%s%s", time_part, tz_sys);
                }
            }

            if (ctx->snapshot.use_native_metadata) {
                /* Option 3: In-memory GExiv2 metadata writer (0 subprocesses) */
                GError *gex_err = native_gex_err;
                GExiv2Metadata *meta = native_meta;

                if (!meta) {
                    meta = gexiv2_metadata_new();
                    if (!gexiv2_metadata_open_path(meta, out_path, NULL)) {
                        char *wmsg = g_strdup_printf("[WARN] gexiv2 open failed for %s: %s",
                                                    orig_basename, gex_err ? gex_err->message : "Unknown");
                        send_progress_to_ui(window_self, 0.0, FALSE, wmsg, FALSE, FALSE);
                        g_free(wmsg);
                        g_clear_error(&gex_err);
                        g_object_unref(meta);
                        meta = NULL;
                    }
                }

                if (meta) {
                    g_clear_error(&gex_err);
                    gexiv2_metadata_try_set_orientation(meta, GEXIV2_ORIENTATION_NORMAL, NULL);
                    stampy_ensure_standard_exif_tags(meta);

                    if (has_geo) {
                        stampy_set_gps_coordinates(meta, geo_entry.latitude, geo_entry.longitude);

                        if (clean_heading_val >= 0.0) {
                            double clean_heading = fmod(clean_heading_val, 360.0);
                            gint nom;
                            char head_buf[32];
                            if (clean_heading < 0.0) clean_heading += 360.0;
                            nom = (gint)lround(clean_heading * 100.0);
                            snprintf(head_buf, sizeof(head_buf), "%d/100", nom);

                            gexiv2_metadata_try_set_exif_tag_rational(meta, "Exif.GPSInfo.GPSImgDirection", nom, 100, NULL);
                            gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSImgDirectionRef", "T", NULL);
                            gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSImgDirection", head_buf, NULL);
                            gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.GPSImgDirectionRef", "T", NULL);
                        }
                    }

                    if (desc_text && *desc_text != '\0') {
                        set_gexiv2_xp_tag(meta, "Exif.Image.XPTitle", desc_text);
                        set_gexiv2_xp_tag(meta, "Exif.Image.XPComment", desc_text);
                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Image.ImageDescription", desc_text, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.dc.description", desc_text, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.dc.title", desc_text, NULL);
                    }

                    if (new_dt[0] != '\0') {
                        gchar *tz = stampy_resolve_timezone_offset(meta);
                        char iso_dt[64];
                        char iptc_time[32];

                        snprintf(iso_dt, sizeof(iso_dt), "%.4s-%.2s-%.2sT%s%s",
                                 docx_date, docx_date + 5, docx_date + 8, time_part, tz);
                        snprintf(iptc_time, sizeof(iptc_time), "%s%s", time_part, tz);

                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.DateTimeOriginal", new_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.DateTimeDigitized", new_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Image.DateTime", new_dt, NULL);

                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.OffsetTimeOriginal", tz, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.OffsetTimeDigitized", tz, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Exif.Photo.OffsetTime", tz, NULL);

                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.DateTimeOriginal", iso_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.exif.DateTimeDigitized", iso_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.xmp.CreateDate", iso_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.xmp.ModifyDate", iso_dt, NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Xmp.photoshop.DateCreated", iso_dt, NULL);

                        if (docx_date && time_part[0] != '\0') {
                            gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSDateStamp", docx_date, NULL);
                            gexiv2_metadata_try_set_tag_string(meta, "Exif.GPSInfo.GPSTimeStamp", time_part, NULL);
                        }
                        /* Clear IPTC DateCreated to prevent it from clobbering DateCreated in exiftool -csv */
                        gexiv2_metadata_try_clear_tag(meta, "Iptc.Application2.DateCreated", NULL);
                        gexiv2_metadata_try_set_tag_string(meta, "Iptc.Application2.TimeCreated", iptc_time, NULL);
                        g_free(tz);
                    }

                    if (!gexiv2_metadata_save_file(meta, out_path, &gex_err)) {
                        char *wmsg = g_strdup_printf("[WARN] gexiv2 save failed for %s: %s",
                                                    orig_basename, gex_err ? gex_err->message : "Unknown");
                        send_progress_to_ui(window_self, 0.0, FALSE, wmsg, FALSE, FALSE);
                        g_free(wmsg);
                        g_clear_error(&gex_err);
                    }
                    g_object_unref(meta);
                }
            } else if (ctx->argfile_fp != NULL) {
                /* Option 2: Write commands to batch argfile (-@) */
                g_mutex_lock(&ctx->argfile_mutex);
                fprintf(ctx->argfile_fp, "-overwrite_original\n-quiet\n");
                fprintf(ctx->argfile_fp, "-TagsFromFile\n%s\n-all:all\n-Orientation=\n", src_path);

                if (has_geo) {
                    double abs_lat = fabs(geo_entry.latitude);
                    double abs_lon = fabs(geo_entry.longitude);
                    char lat_ref = (geo_entry.latitude >= 0.0) ? 'N' : 'S';
                    char lon_ref = (geo_entry.longitude >= 0.0) ? 'E' : 'W';
                    gchar *lat_str = format_double_safe(abs_lat, 8);
                    gchar *lon_str = format_double_safe(abs_lon, 8);

                    fprintf(ctx->argfile_fp, "-gps:all=\n-XMP:GPS:all=\n");
                    fprintf(ctx->argfile_fp, "-GPSLatitude=%s\n-GPSLatitudeRef=%c\n", lat_str, lat_ref);
                    fprintf(ctx->argfile_fp, "-GPSLongitude=%s\n-GPSLongitudeRef=%c\n", lon_str, lon_ref);

                    if (clean_heading_val >= 0.0) {
                        double clean_heading = fmod(clean_heading_val, 360.0);
                        gchar *head_str;
                        if (clean_heading < 0.0) clean_heading += 360.0;
                        head_str = format_double_safe(clean_heading, 2);
                        fprintf(ctx->argfile_fp, "-GPSImgDirection=%s\n-GPSImgDirectionRef=T\n", head_str);
                        g_free(head_str);
                    }

                    g_free(lat_str);
                    g_free(lon_str);
                }

                if (desc_text && *desc_text != '\0') {
                    fprintf(ctx->argfile_fp, "-XMP:Title=%s\n", desc_text);
                    fprintf(ctx->argfile_fp, "-XMP:Description=%s\n", desc_text);
                    fprintf(ctx->argfile_fp, "-XPTitle=%s\n", desc_text);
                    fprintf(ctx->argfile_fp, "-XPComment=%s\n", desc_text);
                    fprintf(ctx->argfile_fp, "-ImageDescription=%s\n", desc_text);
                }

                if (new_dt[0] != '\0') {
                    fprintf(ctx->argfile_fp, "-EXIF:DateTimeOriginal=%s\n", new_dt);
                    fprintf(ctx->argfile_fp, "-EXIF:CreateDate=%s\n", new_dt);
                    fprintf(ctx->argfile_fp, "-EXIF:ModifyDate=%s\n", new_dt);
                    if (tz_sys[0] != '\0') {
                        fprintf(ctx->argfile_fp, "-OffsetTimeOriginal=%s\n", tz_sys);
                        fprintf(ctx->argfile_fp, "-OffsetTimeDigitized=%s\n", tz_sys);
                        fprintf(ctx->argfile_fp, "-OffsetTime=%s\n", tz_sys);
                    }
                    fprintf(ctx->argfile_fp, "-XMP-exif:DateTimeOriginal=%s\n", iso_dt_common[0] ? iso_dt_common : new_dt);
                    fprintf(ctx->argfile_fp, "-XMP-exif:DateTimeDigitized=%s\n", iso_dt_common[0] ? iso_dt_common : new_dt);
                    fprintf(ctx->argfile_fp, "-XMP-xmp:CreateDate=%s\n", iso_dt_common[0] ? iso_dt_common : new_dt);
                    fprintf(ctx->argfile_fp, "-XMP-xmp:ModifyDate=%s\n", iso_dt_common[0] ? iso_dt_common : new_dt);
                    fprintf(ctx->argfile_fp, "-XMP-photoshop:DateCreated=%s\n", iso_dt_common[0] ? iso_dt_common : new_dt);
                    if (docx_date && time_part[0] != '\0') {
                        fprintf(ctx->argfile_fp, "-IPTC:DateCreated=\n");
                        fprintf(ctx->argfile_fp, "-IPTC:TimeCreated=%s\n", iptc_time_common[0] ? iptc_time_common : time_part);
                        fprintf(ctx->argfile_fp, "-GPSDateStamp=%s\n", docx_date);
                        fprintf(ctx->argfile_fp, "-GPSTimeStamp=%s\n", time_part);
                    }
                }

                fprintf(ctx->argfile_fp, "%s\n-execute\n", out_path);
                g_mutex_unlock(&ctx->argfile_mutex);
            } else {
                /* Option 1: Single combined ExifTool run (1 subprocess) */
                char *exif_cmd = get_exiftool_cmd();
                gchar *quoted_source = g_shell_quote(src_path);
                gchar *quoted_target = g_shell_quote(out_path);
                GString *cmd = g_string_new(exif_cmd);

                g_string_append(cmd, " -overwrite_original -quiet ");
                g_string_append_printf(cmd, "-TagsFromFile %s \"-all:all\" \"-Orientation=\" ", quoted_source);

                if (has_geo) {
                    double abs_lat = fabs(geo_entry.latitude);
                    double abs_lon = fabs(geo_entry.longitude);
                    char lat_ref = (geo_entry.latitude >= 0.0) ? 'N' : 'S';
                    char lon_ref = (geo_entry.longitude >= 0.0) ? 'E' : 'W';
                    gchar *lat_str = format_double_safe(abs_lat, 8);
                    gchar *lon_str = format_double_safe(abs_lon, 8);

                    g_string_append_printf(cmd,
                        "-gps:all= -XMP:GPS:all= "
                        "-GPSLatitude=%s -GPSLatitudeRef=%c "
                        "-GPSLongitude=%s -GPSLongitudeRef=%c ",
                        lat_str, lat_ref, lon_str, lon_ref);

                    if (clean_heading_val >= 0.0) {
                        double clean_heading = fmod(clean_heading_val, 360.0);
                        gchar *head_str;
                        if (clean_heading < 0.0) clean_heading += 360.0;
                        head_str = format_double_safe(clean_heading, 2);
                        g_string_append_printf(cmd,
                            "-GPSImgDirection=%s -GPSImgDirectionRef=T ",
                            head_str);
                        g_free(head_str);
                    }

                    g_free(lat_str);
                    g_free(lon_str);
                }

                if (desc_text && *desc_text != '\0') {
                    g_string_append_printf(cmd,
                        "-XMP:Title=\"%s\" -XMP:Description=\"%s\" -XPTitle=\"%s\" -XPComment=\"%s\" -ImageDescription=\"%s\" ",
                        desc_text, desc_text, desc_text, desc_text, desc_text);
                }

                if (new_dt[0] != '\0') {
                    g_string_append_printf(cmd,
                        "-EXIF:DateTimeOriginal=\"%s\" -EXIF:CreateDate=\"%s\" -EXIF:ModifyDate=\"%s\" ",
                        new_dt, new_dt, new_dt);
                    if (tz_sys[0] != '\0') {
                        g_string_append_printf(cmd,
                            "-OffsetTimeOriginal=\"%s\" -OffsetTimeDigitized=\"%s\" -OffsetTime=\"%s\" ",
                            tz_sys, tz_sys, tz_sys);
                    }
                    g_string_append_printf(cmd,
                        "-XMP-exif:DateTimeOriginal=\"%s\" -XMP-exif:DateTimeDigitized=\"%s\" "
                        "-XMP-xmp:CreateDate=\"%s\" -XMP-xmp:ModifyDate=\"%s\" "
                        "-XMP-photoshop:DateCreated=\"%s\" ",
                        iso_dt_common[0] ? iso_dt_common : new_dt,
                        iso_dt_common[0] ? iso_dt_common : new_dt,
                        iso_dt_common[0] ? iso_dt_common : new_dt,
                        iso_dt_common[0] ? iso_dt_common : new_dt,
                        iso_dt_common[0] ? iso_dt_common : new_dt);
                    if (docx_date && time_part[0] != '\0') {
                        g_string_append_printf(cmd,
                            "-IPTC:DateCreated= -IPTC:TimeCreated=\"%s\" "
                            "-GPSDateStamp=\"%s\" -GPSTimeStamp=\"%s\" ",
                            iptc_time_common[0] ? iptc_time_common : time_part,
                            docx_date, time_part);
                    }
                }

                g_string_append(cmd, quoted_target);

                {
                    gint exit_status = 0;
                    GError *exif_error = NULL;
                    if (!g_spawn_command_line_sync(cmd->str, NULL, NULL, &exit_status, &exif_error) || exit_status != 0) {
                        char *wmsg = g_strdup_printf("[WARN] Metadata write failed for %s: %s",
                                                    orig_basename, exif_error ? exif_error->message : "ExifTool error");
                        send_progress_to_ui(window_self, 0.0, FALSE, wmsg, FALSE, FALSE);
                        g_free(wmsg);
                    }
                    g_clear_error(&exif_error);
                }

                g_string_free(cmd, TRUE);
                g_free(exif_cmd);
                g_free(quoted_source);
                g_free(quoted_target);
            }

            g_free(desc_text);
            g_free(docx_date);
            g_free(src_path);
        } else {
            char *emsg;
            g_atomic_int_add(&ctx->error_count, 1);
            emsg = g_strdup_printf("[ERROR] Failed saving: %s (%s)", orig_basename, error ? error->message : "Unknown");
            send_progress_to_ui(window_self, 0.0, FALSE, emsg, FALSE, FALSE);
            g_free(emsg);
            if (error) g_error_free(error);
        }

        g_object_unref(stamped);
    } else {
        char *emsg;
        g_atomic_int_add(&ctx->error_count, 1);
        emsg = g_strdup_printf("[ERROR] Failed rendering: %s", orig_basename);
        send_progress_to_ui(window_self, 0.0, FALSE, emsg, FALSE, FALSE);
        g_free(emsg);
    }

    g_free(custom_text);
    g_free(azimuth_str);
    g_free(orig_basename);
    g_free(out_path);
    g_free(item);

    /* Report progress on every image; the UI updates it in place on one line. */
    done = g_atomic_int_add(&ctx->completed, 1) + 1;
    {
        char *msg = g_strdup_printf("[%d/%u] photos processed...", done, total);
        send_progress_to_ui(window_self, (double)done / (double)total, TRUE, msg, FALSE, FALSE);
        g_free(msg);
    }
}

/* Releases the sleep inhibitor and notifies on completion (main thread). */
typedef struct {
    GtkApplication *app;
    StampyWindow *self;
    guint cookie;
    guint total;
    guint completed;
    guint success;
    guint errors;
    guint warn_gps;
    guint warn_wm;
    guint warn_id;
} ExportFinishData;

static gboolean on_export_finished_idle(gpointer user_data) {
    ExportFinishData *data = (ExportFinishData *)user_data;
    char *heading;
    GString *body;
    AdwDialog *alert;

    /* Release the sleep inhibitor. */
    if (data->app && data->cookie > 0) {
        gtk_application_uninhibit(data->app, data->cookie);
    }

    /* Desktop notification. */
    if (data->app) {
        GNotification *notif = g_notification_new("Stampy Export Complete");
        char *notif_body = g_strdup_printf("%u photos exported successfully.", data->success);
        g_notification_set_body(notif, notif_body);
        g_notification_set_priority(notif, G_NOTIFICATION_PRIORITY_NORMAL);
        g_application_send_notification(G_APPLICATION(data->app), "export-complete", notif);
        g_free(notif_body);
        g_object_unref(notif);
    }

    /* Dialog showing post-export summary */
    if (data->self) {
        body = g_string_new("");

        if (data->errors == 0 && data->warn_gps == 0 && data->warn_wm == 0 && data->warn_id == 0) {
            heading = g_strdup("Export Finished Successfully");
            g_string_append_printf(body, "All %u photos were exported successfully with complete metadata.", data->total);
        } else {
            heading = g_strdup_printf("Export Finished (%u Photos)", data->total);
            g_string_append_printf(body, "• Successful: %u\n", data->success);
            if (data->warn_gps > 0 || data->warn_wm > 0 || data->warn_id > 0) {
                g_string_append(body, "\n• Warnings:\n");
                if (data->warn_gps > 0)
                    g_string_append_printf(body, "   - Missing GPS coordinates: %u\n", data->warn_gps);
                if (data->warn_wm > 0)
                    g_string_append_printf(body, "   - Missing Watermark text: %u\n", data->warn_wm);
                if (data->warn_id > 0)
                    g_string_append_printf(body, "   - Missing Image Code/ID: %u\n", data->warn_id);
            }
            if (data->errors > 0) {
                g_string_append_printf(body, "\n• Errors:\n   - Failed to render/save: %u\n", data->errors);
            }
        }

        alert = adw_alert_dialog_new(heading, body->str);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "OK");
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "ok");
        adw_dialog_present(alert, GTK_WIDGET(data->self));

        g_free(heading);
        g_string_free(body, TRUE);
    }

    g_object_unref(data->app);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static gpointer export_worker_thread(gpointer user_data) {
    ExportThreadData *td;
    StampyWindow *window_self;
    char *folder_path;
    guint total;
    guint i;
    char *msg;
    int success = 0, errors = 0, warn_gps = 0, warn_wm = 0, warn_id = 0;

    td = (ExportThreadData *)user_data;
    if (!td || !td->self) return NULL;

    window_self = td->self;
    folder_path = td->export_folder ? g_file_get_path(td->export_folder) : NULL;
    total = (window_self->image_files) ? window_self->image_files->len : 0;

    if (!folder_path) {
        free_export_snapshot(&td->snapshot);
        if (td->export_folder) g_object_unref(td->export_folder);
        g_free(td);
        return NULL;
    }

    send_progress_to_ui(window_self, 0.0, TRUE, "Exporting photos in background...", TRUE, FALSE);

    /* Export in parallel with a thread pool. */
    {
        ExportWorkerContext ctx;
        GThreadPool *pool;
        gint max_threads = stampy_config_get_export_threads();
        int n_cpus = g_get_num_processors();
        GError *pool_err = NULL;
        ExportWorkItem *item;

        if (n_cpus < 1) n_cpus = 1;

        if (max_threads > n_cpus) {
            msg = g_strdup_printf("[INFO] Starting parallel export of %u items (using %d worker threads across %d CPU cores)...",
                                  total, max_threads, n_cpus);
        } else {
            msg = g_strdup_printf("[INFO] Starting parallel export of %u items (using %d worker threads on %d CPU cores)...",
                                  total, max_threads, n_cpus);
        }
        send_progress_to_ui(window_self, 0.0, TRUE, msg, FALSE, FALSE);
        g_free(msg);

        ctx.self = window_self;
        ctx.folder_path = folder_path;
        ctx.snapshot = td->snapshot;  /* read-only during export */
        ctx.total = total;
        ctx.completed = 0;
        ctx.success_count = 0;
        ctx.error_count = 0;
        ctx.warn_gps_missing = 0;
        ctx.warn_wm_missing = 0;
        ctx.warn_id_missing = 0;
        ctx.argfile_fp = NULL;
        ctx.argfile_path = NULL;
        ctx.compass_idx = NULL;
        g_mutex_init(&ctx.argfile_mutex);

        if (ctx.snapshot.has_compass && ctx.snapshot.file_compass) {
            char *compass_dir = g_file_get_path(ctx.snapshot.file_compass);
            if (compass_dir) {
                ctx.compass_idx = compass_index_new(compass_dir);
                g_free(compass_dir);
            }
        }

        if (ctx.snapshot.batch_exiftool && !ctx.snapshot.use_native_metadata) {
            int arg_fd;
            ctx.argfile_path = g_build_filename(g_get_tmp_dir(), "stampy_args_XXXXXX", NULL);
            arg_fd = g_mkstemp(ctx.argfile_path);
            if (arg_fd >= 0) {
                ctx.argfile_fp = fdopen(arg_fd, "w");
            }
        }

        /* Thread count comes from Preferences (all cores or a manual limit). */
        pool = g_thread_pool_new(export_image_worker, &ctx, max_threads, FALSE, &pool_err);
        if (!pool_err) {
            for (i = 0; i < total; i++) {
                if (g_atomic_int_get(&window_self->export_cancel_flag) == 1) break;
                item = g_new0(ExportWorkItem, 1);
                item->ctx = &ctx;
                item->index = i;
                g_thread_pool_push(pool, item, NULL);
            }
            /* Wait for all tasks to finish. */
            g_thread_pool_free(pool, FALSE, TRUE);
        } else {
            send_progress_to_ui(window_self, 0.0, FALSE, "[ERROR] Failed to create thread pool.", FALSE, TRUE);
            g_error_free(pool_err);
        }

        if (ctx.compass_idx) {
            compass_index_free((CompassIndex *)ctx.compass_idx);
            ctx.compass_idx = NULL;
        }

        if (ctx.argfile_fp) {
            fputs("-execute\n-stay_open\nFalse\n", ctx.argfile_fp);
            fclose(ctx.argfile_fp);
            ctx.argfile_fp = NULL;

            if (g_atomic_int_get(&window_self->export_cancel_flag) == 0) {
                char *exif_cmd = get_exiftool_cmd();
                char *quoted_arg = g_shell_quote(ctx.argfile_path);
                char *batch_cmd = g_strdup_printf("%s -stay_open True -@ %s", exif_cmd, quoted_arg);
                char *batch_stderr = NULL;
                gint exit_status = 0;
                GError *batch_err = NULL;

                send_progress_to_ui(window_self, 0.99, TRUE, "Executing ExifTool batch metadata pass...", FALSE, FALSE);
                g_spawn_command_line_sync(batch_cmd, NULL, &batch_stderr, &exit_status, &batch_err);
                {
                    char *batch_log = g_strdup_printf("[INFO] ExifTool batch argfile: %s (exit %d)%s%s",
                                                ctx.argfile_path,
                                                exit_status,
                                                batch_stderr && *batch_stderr ? " stderr: " : "",
                                                batch_stderr && *batch_stderr ? batch_stderr : "");
                    send_progress_to_ui(window_self, 0.0, FALSE, batch_log, FALSE, FALSE);
                    g_free(batch_log);
                }
                if (batch_err || exit_status != 0) {
                    char *wmsg = g_strdup_printf("[WARN] ExifTool batch failed; argfile kept at %s: %s",
                                                ctx.argfile_path,
                                                batch_err ? batch_err->message :
                                                (batch_stderr && *batch_stderr ? batch_stderr : "exit code non-zero"));
                    send_progress_to_ui(window_self, 0.0, FALSE, wmsg, FALSE, FALSE);
                    g_free(wmsg);
                }
                g_clear_error(&batch_err);
                g_free(batch_stderr);
                g_free(batch_cmd);
                g_free(quoted_arg);
                g_free(exif_cmd);
            }

            g_free(ctx.argfile_path);
            ctx.argfile_path = NULL;
        }
        g_mutex_clear(&ctx.argfile_mutex);

        success = g_atomic_int_get(&ctx.success_count);
        errors = g_atomic_int_get(&ctx.error_count);
        warn_gps = g_atomic_int_get(&ctx.warn_gps_missing);
        warn_wm = g_atomic_int_get(&ctx.warn_wm_missing);
        warn_id = g_atomic_int_get(&ctx.warn_id_missing);
    }

    send_progress_to_ui(window_self, 1.0, FALSE, "Export Finished!", TRUE, TRUE);

    /* Post-export terminal summary */
    {
        GString *summary = g_string_new("[INFO] ========================================\n");
        g_string_append_printf(summary, "[INFO] Export Summary (%u total):\n", total);
        g_string_append_printf(summary, "[INFO]   ✓ Successfully exported: %d\n", success);
        if (warn_gps > 0)
            g_string_append_printf(summary, "[WARN]   ⚠ Missing GPS: %d\n", warn_gps);
        if (warn_wm > 0)
            g_string_append_printf(summary, "[WARN]   ⚠ Missing Watermark: %d\n", warn_wm);
        if (warn_id > 0)
            g_string_append_printf(summary, "[WARN]   ⚠ Missing Image Code/ID: %d\n", warn_id);
        if (errors > 0)
            g_string_append_printf(summary, "[ERROR]  ✗ Errors (failed saving): %d\n", errors);
        else
            g_string_append(summary, "[INFO]   ✓ Errors: 0\n");
        g_string_append(summary, "[INFO] ========================================");
        send_progress_to_ui(window_self, 1.0, FALSE, summary->str, FALSE, TRUE);
        g_string_free(summary, TRUE);
    }

    /* The snapshot was copied into ctx; td->snapshot is still ours to free. */
    free_export_snapshot(&td->snapshot);

    /* Uninhibit and notify on the main thread. */
    {
        GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(window_self)));
        if (app) {
            ExportFinishData *finish = g_new0(ExportFinishData, 1);
            finish->app = g_object_ref(app);
            finish->self = window_self;
            finish->cookie = td->inhibit_cookie;
            finish->completed = total;
            finish->total = total;
            finish->success = success;
            finish->errors = errors;
            finish->warn_gps = warn_gps;
            finish->warn_wm = warn_wm;
            finish->warn_id = warn_id;
            g_idle_add(on_export_finished_idle, finish);
        }
    }

    g_free(folder_path);
    if (td->export_folder) g_object_unref(td->export_folder);
    g_free(td);
    return NULL;
}

static void begin_export_to_folder(StampyWindow *window_self, GFile *folder) {
    ExportThreadData *td;

    g_atomic_int_set(&window_self->export_cancel_flag, 0);
    window_self->export_active = TRUE;
    gtk_window_set_title(GTK_WINDOW(window_self), "Exporting…");

    if (window_self->btn_header_export) gtk_widget_set_visible(window_self->btn_header_export, FALSE);
    if (window_self->btn_header_cancel) gtk_widget_set_visible(window_self->btn_header_cancel, TRUE);
    if (window_self->btn_header_cancel) gtk_widget_set_sensitive(window_self->btn_header_cancel, TRUE);
    if (window_self->box_right_panel) gtk_widget_set_sensitive(window_self->box_right_panel, FALSE);
    if (window_self->file_listbox) gtk_widget_set_sensitive(window_self->file_listbox, FALSE);

    td = g_new0(ExportThreadData, 1);
    td->self = window_self;
    td->export_folder = folder;

    /* Keep the display awake during export. */
    {
        GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(window_self)));
        if (app) {
            td->inhibit_cookie = gtk_application_inhibit(app, GTK_WINDOW(window_self),
                GTK_APPLICATION_INHIBIT_IDLE | GTK_APPLICATION_INHIBIT_SUSPEND,
                "Photo export is in progress");
        }
    }

    snapshot_export_settings(window_self, &td->snapshot);

    g_thread_new("stampy-export-thread", export_worker_thread, td);
}

static void on_export_folder_selected(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *window_self = STAMPY_WINDOW(user_data);
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);

    if (error || !folder) {
        if (error) g_clear_error(&error);
        return;
    }

    begin_export_to_folder(window_self, folder);
}

void stampy_start_export(StampyWindow *window_self) {
    GtkFileDialog *dialog;

    if (!window_self || !window_self->image_files || window_self->image_files->len == 0) {
        stampy_show_toast(window_self, "No images loaded in list!");
        return;
    }

    /* Auto-created <folder>_stamped target skips the folder picker. */
    if (window_self->default_export_folder) {
        begin_export_to_folder(window_self, g_object_ref(window_self->default_export_folder));
        return;
    }

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Export Destination Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window_self), NULL, on_export_folder_selected, window_self);
}

static void on_btn_header_export_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    stampy_start_export(STAMPY_WINDOW(user_data));
}

void stampy_cancel_export(StampyWindow *window_self) {
    if (!window_self) return;

    g_atomic_int_set(&window_self->export_cancel_flag, 1);
    if (window_self->btn_header_cancel)
        gtk_widget_set_sensitive(GTK_WIDGET(window_self->btn_header_cancel), FALSE);
}

static void on_btn_header_cancel_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    stampy_cancel_export(STAMPY_WINDOW(user_data));
}

void stampy_connect_export_logic(StampyWindow *window_self) {
    if (!window_self) return;

    if (window_self->btn_header_export) {
        g_signal_connect(window_self->btn_header_export, "clicked", G_CALLBACK(on_btn_header_export_clicked), window_self);
    }
    if (window_self->btn_header_cancel) {
        g_signal_connect(window_self->btn_header_cancel, "clicked", G_CALLBACK(on_btn_header_cancel_clicked), window_self);
    }
}
