#include "stampy-window-private.h"
#include "engine/stampy-metadata.h"
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
    int comp_margin_x;
    int comp_margin_y;
    guint comp_pos_idx;
    gboolean comp_shadow;

    double font_size;
    double text_opacity;
    int text_margin_x;
    int text_margin_y;
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

/* Shared context for parallel export — read-only during export, safe to share across threads */
typedef struct {
    StampyWindow *self;
    char *folder_path;
    ExportSettingsSnapshot snapshot;
    guint total;
    volatile gint completed;   /* atomic counter */
} ExportWorkerContext;

/* Per-image work item */
typedef struct {
    ExportWorkerContext *ctx;
    guint index;
} ExportWorkItem;

/* ========================================================================= */
/* --- HELPER FUNCTIONS ---                                                  */
/* ========================================================================= */

static int extract_id_from_filename(const char *filename) {
    char *basename;
    GString *digits;
    const char *p;
    int id_val = -1;

    if (!filename || *filename == '\0') return -1;

    basename = g_path_get_basename(filename);
    if (!basename) return -1;

    digits = g_string_new("");
    for (p = basename; *p; p++) {
        if (g_ascii_isdigit(*p)) {
            g_string_append_c(digits, *p);
        }
    }

    if (digits->len > 0) {
        id_val = atoi(digits->str);
    }

    g_string_free(digits, TRUE);
    g_free(basename);
    return id_val;
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
        snap->comp_margin_x = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_margin_x));
        snap->comp_margin_y = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_compass_margin_y));
        snap->comp_pos_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(window_self->drop_compass_pos));
        snap->comp_shadow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window_self->btn_c_shadow));
    }

    if (window_self->val_text_size) {
        GListModel *model;
        guint sel;

        snap->font_size = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_size));
        snap->text_opacity = gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_opacity)) / 100.0;
        snap->text_margin_x = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_margin_x));
        snap->text_margin_y = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(window_self->val_text_margin_y));
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

static GdkPixbuf *render_stamped_pixbuf_from_snapshot(GFile *image_file, const ExportSettingsSnapshot *snap, const char *in_custom_text, const char *in_azimuth_str) {
    char *path;
    GdkPixbuf *raw_pixbuf;
    GdkPixbuf *pixbuf = NULL;
    cairo_surface_t *surface;
    cairo_t *cr;
    int width, height;
    double scale;
    char *orig_basename;
    char *custom_text;
    char *azimuth_str;

    custom_text = (in_custom_text && *in_custom_text) ? g_strdup(in_custom_text) : NULL;
    azimuth_str = (in_azimuth_str && *in_azimuth_str) ? g_strdup(in_azimuth_str) : NULL;

    if (!image_file) {
        g_free(custom_text);
        g_free(azimuth_str);
        return NULL;
    }

    path = g_file_get_path(image_file);
    if (!path) {
        g_free(custom_text);
        g_free(azimuth_str);
        return NULL;
    }

    raw_pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    if (!raw_pixbuf) {
        g_free(path);
        g_free(custom_text);
        g_free(azimuth_str);
        return NULL;
    }

    pixbuf = gdk_pixbuf_apply_embedded_orientation(raw_pixbuf);
    g_object_unref(raw_pixbuf);
    if (!pixbuf) pixbuf = raw_pixbuf;

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);

    scale = (double)(width < height ? width : height) / 500.0;
    if (scale < 1.0) scale = 1.0;

    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cr = cairo_create(surface);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
#pragma GCC diagnostic pop
    cairo_paint(cr);
    g_object_unref(pixbuf);

    orig_basename = g_file_get_basename(image_file);

    /* 1. Render Compass Overlay */
    if (snap->has_compass && snap->file_compass) {
        int comp_w = (int)(snap->comp_w * scale);
        double opacity = snap->comp_opacity;
        int margin_x = (int)(snap->comp_margin_x * scale);
        int margin_y = (int)(snap->comp_margin_y * scale);
        guint pos_idx = snap->comp_pos_idx;

        char *compass_dir = g_file_get_path(snap->file_compass);
        GdkPixbuf *comp_pb = NULL;

        if (compass_dir) {
            comp_pb = load_best_compass_pixbuf(compass_dir, azimuth_str, comp_w);
            g_free(compass_dir);
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

    /* 2. Render Text Overlay */
    if (snap->font_size > 0 && custom_text && *custom_text != '\0') {
        double font_size = snap->font_size * scale;
        double opacity = snap->text_opacity;
        int margin_x = (int)(snap->text_margin_x * scale);
        int margin_y = (int)(snap->text_margin_y * scale);
        guint pos_idx = snap->text_pos_idx;

        const char *font_family = snap->font_family ? snap->font_family : "Arial";
        char **raw_lines;
        char **lines;
        guint valid_n_lines = 0;
        guint i;
        GdkRGBA tc = snap->text_color;
        GdkRGBA sc = snap->stroke_color;
        cairo_font_extents_t fext;
        double max_line_w = 0.0;
        double line_spacing = snap->line_spacing * scale;

        char *clean_text = g_strdup(custom_text);
        char *p = clean_text, *q = clean_text;
        while (*p) {
            if (*p != '\r') *q++ = *p;
            p++;
        }
        *q = '\0';
        g_strchomp(clean_text);

        raw_lines = g_strsplit(clean_text, "\n", -1);
        guint raw_len = g_strv_length(raw_lines);
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

            double pad_right = 0.0, pad_bottom = 0.0, pad_left = 0.0, pad_top = 0.0;
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

            double left_bound = margin_x + pad_left;
            double right_bound = width - margin_x - pad_right;

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

    g_free(orig_basename);
    g_free(custom_text);
    g_free(azimuth_str);
    g_free(path);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
#pragma GCC diagnostic pop

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return pixbuf;
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
                GdkPixbuf *stamped;
                char *orig_basename = g_file_get_basename(file);
                char *custom_text = NULL;
                char *azimuth_str = NULL;

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
                stamped = render_stamped_pixbuf_from_snapshot(file, &snap, custom_text, azimuth_str);

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

/* ========================================================================= */
/* --- NEW INTERRUPTABLE EXPORT ENGINE ---                                   */
/* ========================================================================= */

static gboolean update_ui_progress_idle(gpointer user_data) {
    UIProgressUpdateData *data = (UIProgressUpdateData *)user_data;
    if (!data) return G_SOURCE_REMOVE;

    if (data->self) {
        stampy_set_progress(data->self, data->fraction, data->visible);

        if (data->message) {
            if (data->is_toast) {
                stampy_show_toast(data->self, data->message);
            } else {
                stampy_append_log(data->self, data->message);
            }
        }

        if (data->is_finished) {
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

/* --- PARALLEL EXPORT WORKER (one per image) --- */
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
            }
        }
    }

    stamped = render_stamped_pixbuf_from_snapshot(file, &ctx->snapshot, custom_text, azimuth_str);

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
            char *exif_err = NULL;

            if (has_geo) {
                if (!stampy_metadata_write_gps_and_heading(src_path, out_path,
                                                          geo_entry.latitude,
                                                          geo_entry.longitude,
                                                          clean_heading_val,
                                                          &exif_err)) {
                    char *wmsg = g_strdup_printf("[WARN] GPS write failed for %s: %s", orig_basename, exif_err ? exif_err : "Unknown");
                    send_progress_to_ui(window_self, 0.0, FALSE, wmsg, FALSE, FALSE);
                    g_free(wmsg);
                    g_free(exif_err);
                    exif_err = NULL;
                }
            } else {
                gchar *quoted_source = g_shell_quote(src_path);
                gchar *quoted_target = g_shell_quote(out_path);
                char *copy_cmd = g_strdup_printf(
                    "%s -overwrite_original -quiet -TagsFromFile %s \"-all:all\" \"-Orientation=\" %s",
                    g_getenv("STAMPY_EXIFTOOL") ? g_getenv("STAMPY_EXIFTOOL") : "exiftool",
                    quoted_source, quoted_target);
                g_spawn_command_line_sync(copy_cmd, NULL, NULL, NULL, NULL);
                g_free(copy_cmd);
                g_free(quoted_source);
                g_free(quoted_target);
            }

            g_free(src_path);
        } else {
            char *emsg = g_strdup_printf("[ERROR] Failed saving: %s (%s)", orig_basename, error ? error->message : "Unknown");
            send_progress_to_ui(window_self, 0.0, FALSE, emsg, FALSE, FALSE);
            g_free(emsg);
            if (error) g_error_free(error);
        }

        g_object_unref(stamped);
    } else {
        char *emsg = g_strdup_printf("[ERROR] Failed rendering: %s", orig_basename);
        send_progress_to_ui(window_self, 0.0, FALSE, emsg, FALSE, FALSE);
        g_free(emsg);
    }

    g_free(custom_text);
    g_free(azimuth_str);
    g_free(orig_basename);
    g_free(out_path);
    g_free(item);

    /* Update progress */
    int done = g_atomic_int_add(&ctx->completed, 1) + 1;
    if (done % 10 == 0 || done == (int)total) {
        char *msg = g_strdup_printf("[%d/%u] photos processed...", done, total);
        send_progress_to_ui(window_self, (double)done / (double)total, TRUE, msg, FALSE, FALSE);
        g_free(msg);
    }
}

/* --- Uninhibit + Send Notification (runs on main thread via g_idle_add) --- */
typedef struct {
    GtkApplication *app;
    guint cookie;
    guint total;
    guint completed;
} ExportFinishData;

static gboolean on_export_finished_idle(gpointer user_data) {
    ExportFinishData *data = (ExportFinishData *)user_data;

    /* Uninhibit sleep */
    if (data->app && data->cookie > 0) {
        gtk_application_uninhibit(data->app, data->cookie);
    }

    /* Send desktop notification */
    if (data->app) {
        GNotification *notif = g_notification_new("Stampy Export Complete");
        char *body = g_strdup_printf("All %u photos have been exported successfully!", data->completed);
        g_notification_set_body(notif, body);
        g_notification_set_priority(notif, G_NOTIFICATION_PRIORITY_NORMAL);
        g_application_send_notification(G_APPLICATION(data->app), "export-complete", notif);
        g_free(body);
        g_object_unref(notif);
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

    /* --- PARALLEL EXPORT via GThreadPool --- */
    {
        ExportWorkerContext ctx;
        GThreadPool *pool;
        gint max_threads = stampy_config_get_export_threads();
        GError *pool_err = NULL;

        msg = g_strdup_printf("[INFO] Starting parallel export of %u items (%d worker thread%s)...",
                              total, max_threads, max_threads == 1 ? "" : "s");
        send_progress_to_ui(window_self, 0.0, TRUE, msg, FALSE, FALSE);
        g_free(msg);

        ctx.self = window_self;
        ctx.folder_path = folder_path;
        ctx.snapshot = td->snapshot;  /* struct copy — read-only during export */
        ctx.total = total;
        ctx.completed = 0;

        /* max_threads comes from Preferences (all cores, or a manual limit) */
        pool = g_thread_pool_new(export_image_worker, &ctx, max_threads, FALSE, &pool_err);
        if (!pool_err) {
            for (i = 0; i < total; i++) {
                if (g_atomic_int_get(&window_self->export_cancel_flag) == 1) break;
                ExportWorkItem *item = g_new0(ExportWorkItem, 1);
                item->ctx = &ctx;
                item->index = i;
                g_thread_pool_push(pool, item, NULL);
            }
            /* Wait for all tasks to finish */
            g_thread_pool_free(pool, FALSE, TRUE);
        } else {
            send_progress_to_ui(window_self, 0.0, FALSE, "[ERROR] Failed to create thread pool.", FALSE, TRUE);
            g_error_free(pool_err);
        }
    }

    send_progress_to_ui(window_self, 1.0, FALSE, "Export Finished!", TRUE, TRUE);
    msg = g_strdup_printf("[INFO] All %u photos processed.", total);
    send_progress_to_ui(window_self, 1.0, FALSE, msg, FALSE, TRUE);
    g_free(msg);

    /* Cleanup — snapshot was struct-copied into ctx, but we still own td->snapshot */
    free_export_snapshot(&td->snapshot);

    /* Uninhibit sleep + send completion notification on main thread */
    {
        GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(window_self)));
        if (app) {
            ExportFinishData *finish = g_new0(ExportFinishData, 1);
            finish->app = g_object_ref(app);
            finish->cookie = td->inhibit_cookie;
            finish->completed = total;
            finish->total = total;
            g_idle_add(on_export_finished_idle, finish);
        }
    }

    g_free(folder_path);
    if (td->export_folder) g_object_unref(td->export_folder);
    g_free(td);
    return NULL;
}

static void on_export_folder_selected(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *window_self = STAMPY_WINDOW(user_data);
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);
    ExportThreadData *td;

    if (error || !folder) {
        if (error) g_clear_error(&error);
        return;
    }

    g_atomic_int_set(&window_self->export_cancel_flag, 0);

    if (window_self->btn_header_export) gtk_widget_set_visible(window_self->btn_header_export, FALSE);
    if (window_self->btn_header_cancel) gtk_widget_set_visible(window_self->btn_header_cancel, TRUE);
    if (window_self->btn_header_cancel) gtk_widget_set_sensitive(window_self->btn_header_cancel, TRUE);
    if (window_self->box_right_panel) gtk_widget_set_sensitive(window_self->box_right_panel, FALSE);
    if (window_self->file_listbox) gtk_widget_set_sensitive(window_self->file_listbox, FALSE);

    td = g_new0(ExportThreadData, 1);
    td->self = window_self;
    td->export_folder = folder;

    /* Inhibit sleep during export */
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

void stampy_start_export(StampyWindow *window_self) {
    GtkFileDialog *dialog;

    if (!window_self || !window_self->image_files || window_self->image_files->len == 0) {
        stampy_show_toast(window_self, "No images loaded in list!");
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
