#include "stampy-window-private.h"
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <math.h>

/* Forward declarations. */
static void on_btn_compass_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_docx_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_xlsx_clicked(GtkButton *btn, gpointer user_data);

/* Column mapping: maps Excel column letter to field type.
   field_type: 0=id(NO), 1=longitude(X), 2=latitude(Y), 3=azimuth(Azh), 4=time(Jam) */
typedef struct {
    char col_letter;
    int field_type;
} ColMapEntry;

typedef struct {
    ColMapEntry entries[16];
    int count;
} ColMap;

/* Scan sheet1.xml row 1 headers to build column-to-field mapping.
   Resolves shared strings via shared_strings table.
   Returns number of fields mapped. */
static int scan_excel_column_map(const char *xml_buf, GPtrArray *shared_strings, ColMap *out_map) {
    const char *row_start;
    const char *row_end;
    const char *cell_p;

    if (!out_map) return 0;
    out_map->count = 0;
    if (!xml_buf) return 0;

    row_start = strstr(xml_buf, "<row");
    if (!row_start) return 0;

    row_end = strstr(row_start, "</row>");
    if (!row_end) return 0;

    cell_p = row_start;
    while ((cell_p = strstr(cell_p, "<c r=\"")) != NULL && cell_p < row_end) {
        char col_letter = '\0';
        const char *r_ptr = cell_p + 6;
        char *c_end;
        gboolean is_string = FALSE;
        const char *header_text = NULL;
        char val_buf[64] = {0};

        if (g_ascii_isalpha(*r_ptr)) {
            col_letter = g_ascii_toupper(*r_ptr);
        }
        if (!col_letter) {
            cell_p++;
            continue;
        }

        c_end = strchr(cell_p, '>');
        if (!c_end || c_end > row_end) {
            cell_p++;
            continue;
        }

        if (strstr(cell_p, "t=\"s\"") && strstr(cell_p, "t=\"s\"") < c_end) {
            is_string = TRUE;
        }

        /* Extract <v>...</v> */
        {
            char *v_start = strstr(cell_p, "<v>");
            if (v_start && v_start < row_end) {
                v_start += 3;
                char *v_end = strstr(v_start, "</v>");
                if (v_end && v_end < row_end) {
                    int v_len = v_end - v_start;
                    if (v_len > 0 && v_len < (int)sizeof(val_buf)) {
                        strncpy(val_buf, v_start, v_len);
                        val_buf[v_len] = '\0';

                        if (is_string) {
                            int ss_idx = atoi(val_buf);
                            if (shared_strings && ss_idx >= 0 && ss_idx < (int)shared_strings->len) {
                                header_text = g_ptr_array_index(shared_strings, ss_idx);
                            }
                        } else {
                            header_text = val_buf;
                        }
                    }
                }
            }
        }

        /* If no <v>, check for inline string <t>...</t> */
        if (!header_text) {
            char *t_start = strstr(cell_p, "<t>");
            if (t_start && t_start < row_end) {
                t_start += 3;
                char *t_end = strstr(t_start, "</t>");
                if (t_end && t_end < row_end) {
                    int t_len = t_end - t_start;
                    if (t_len > 0 && t_len < (int)sizeof(val_buf)) {
                        strncpy(val_buf, t_start, t_len);
                        val_buf[t_len] = '\0';
                        header_text = val_buf;
                    }
                }
            }
        }

        if (header_text && *header_text) {
            char clean[64];
            int clen = 0;
            int field_type = -1;

            for (const char *p = header_text; *p && clen < (int)sizeof(clean) - 1; p++) {
                if (g_ascii_isalnum(*p)) {
                    clean[clen++] = g_ascii_tolower(*p);
                }
            }
            clean[clen] = '\0';

            if (g_strcmp0(clean, "no") == 0 || g_strcmp0(clean, "nopu") == 0 ||
                g_strcmp0(clean, "nomor") == 0 || g_strcmp0(clean, "id") == 0 ||
                g_strcmp0(clean, "number") == 0) {
                field_type = 0; /* ID */
            } else if (g_strcmp0(clean, "x") == 0 || strstr(clean, "long") != NULL ||
                       strstr(clean, "bujur") != NULL || g_str_has_prefix(clean, "x")) {
                field_type = 1; /* Longitude */
            } else if (g_strcmp0(clean, "y") == 0 || strstr(clean, "lat") != NULL ||
                       strstr(clean, "lintang") != NULL || g_str_has_prefix(clean, "y")) {
                field_type = 2; /* Latitude */
            } else if (strstr(clean, "azh") != NULL || strstr(clean, "azim") != NULL ||
                       strstr(clean, "arah") != NULL || strstr(clean, "head") != NULL) {
                field_type = 3; /* Azimuth */
            } else if (strstr(clean, "jam") != NULL || strstr(clean, "time") != NULL ||
                       strstr(clean, "waktu") != NULL) {
                field_type = 4; /* Time */
            }

            if (field_type >= 0 && out_map->count < 16) {
                int found_idx = -1;
                for (int k = 0; k < out_map->count; k++) {
                    if (out_map->entries[k].col_letter == col_letter) {
                        found_idx = k;
                        break;
                    }
                }
                if (found_idx >= 0) {
                    out_map->entries[found_idx].field_type = field_type;
                } else {
                    out_map->entries[out_map->count].col_letter = col_letter;
                    out_map->entries[out_map->count].field_type = field_type;
                    out_map->count++;
                }
            }
        }

        cell_p = c_end;
    }

    return out_map->count;
}

/* Get field type for a column letter from the map. */
static int colmap_get_field_type(const ColMap *map, char col_letter) {
    if (!map) return -1;
    for (int i = 0; i < map->count; i++) {
        if (map->entries[i].col_letter == col_letter)
            return map->entries[i].field_type;
    }
    return -1;
}

/* Parse a coordinate string (from a string Excel cell).
   Handles decimal ("-6,123456" / "6,123456 S"), DMS ("6°7'24.44\" S"),
   comma or period as decimal separator,
   and a leading minus or S/W direction letter (which makes it negative). */
static double parse_coord_string(const char *str) {
    double nums[3] = {0.0, 0.0, 0.0};
    int n = 0;
    const char *p;
    double deg = 0.0;

    if (!str) return 0.0;

    for (p = str; *p && n < 3; p++) {
        if (g_ascii_isdigit(*p) || *p == '.' || *p == ',') {
            char buf[64];
            int buf_len = 0;

            if (p > str && (p[-1] == '-' || p[-1] == '+')) {
                buf[buf_len++] = p[-1];
            }
            while (g_ascii_isdigit(*p) || *p == '.' || *p == ',') {
                if (buf_len < (int)sizeof(buf) - 1) buf[buf_len++] = *p;
                p++;
            }
            buf[buf_len] = '\0';

            /* Convert comma decimal separator to period for g_ascii_strtod */
            {
                char *q;
                for (q = buf; *q; q++) {
                    if (*q == ',') *q = '.';
                }
            }

            nums[n++] = g_ascii_strtod(buf, NULL);
            if (*p == '\0') break;
        }
    }

    if (n == 0) return 0.0;

    deg = nums[0];
    if (n >= 2) {
        deg += nums[1] / 60.0;
        if (n >= 3) deg += nums[2] / 3600.0;
    }

    /* Direction letter (S/W) makes a coordinate negative; N/E keeps it positive */
    if (strchr(str, 'S') || strchr(str, 's') ||
        strchr(str, 'W') || strchr(str, 'w')) {
        deg = -fabs(deg);
    }
    return deg;
}

/* Parse an azimuth/heading string from an Excel cell.
   Handles comma as decimal separator ("19,78" → 19.78) and plain numbers.
   Does NOT apply direction logic (azimuth is always positive 0-360). */
static double parse_azimuth_string(const char *str) {
    char buf[128];
    char *q;
    const char *p;
    int len;

    if (!str) return 0.0;

    p = str;
    while (*p == ' ') p++;
    len = strlen(p);
    while (len > 0 && p[len-1] == ' ') len--;

    if (len <= 0 || len >= (int)sizeof(buf)) return 0.0;

    memcpy(buf, p, len);
    buf[len] = '\0';

    for (q = buf; *q; q++) {
        if (*q == ',') *q = '.';
    }

    return g_ascii_strtod(buf, NULL);
}

/* Parses an XLSX file via its raw XML parts (shared strings, auto-swapped lat/lon). */
void parse_excel_file(StampyWindow *self, GFile *file) {
    char *path = NULL;
    char cmd[1024];
    FILE *fp;
    GString *xml_buf;
    char chunk[1024];
    int loaded_count = 0;
    char *lower_path;
    GPtrArray *shared_strings = NULL;

    if (!self || !file) return;

    path = g_file_get_path(file);
    if (!path) return;

    lower_path = g_utf8_strdown(path, -1);
    if (!g_str_has_suffix(lower_path, ".xlsx") && !g_str_has_suffix(lower_path, ".xls")) {
        stampy_append_log(self, "[WARN] Not a valid Excel file!");
        g_free(lower_path);
        g_free(path);
        return;
    }
    g_free(lower_path);

    if (!self->geo_table) {
        self->geo_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    } else {
        g_hash_table_remove_all(self->geo_table);
    }

    stampy_append_log(self, "[INFO] Reading Excel structure by cell reference...");

    /* 1. Read the shared-strings table. */
    shared_strings = g_ptr_array_new_with_free_func(g_free);
    snprintf(cmd, sizeof(cmd), "unzip -p \"%s\" xl/sharedStrings.xml 2>/dev/null", path);
    fp = popen(cmd, "r");
    if (fp) {
        GString *ss_buf = g_string_new("");
        char *p;

        while (fgets(chunk, sizeof(chunk), fp)) {
            g_string_append(ss_buf, chunk);
        }
        pclose(fp);

        p = ss_buf->str;
        while ((p = strstr(p, "<t")) != NULL) {
            char *t_start = strchr(p, '>');
            if (t_start) {
                char *t_end;
                t_start++;
                t_end = strstr(t_start, "</t>");
                if (t_end) {
                    g_ptr_array_add(shared_strings, g_strndup(t_start, t_end - t_start));
                    p = t_end + 4;
                    continue;
                }
            }
            p++;
        }
        g_string_free(ss_buf, TRUE);
    }

    /* 2. Parse the main worksheet. */
    snprintf(cmd, sizeof(cmd), "unzip -p \"%s\" xl/worksheets/sheet1.xml 2>/dev/null", path);
    fp = popen(cmd, "r");

    if (fp) {
        xml_buf = g_string_new("");
        while (fgets(chunk, sizeof(chunk), fp)) {
            g_string_append(xml_buf, chunk);
        }
        pclose(fp);

        if (xml_buf->len > 0) {
            ColMap col_map;
            gboolean has_map_lon = FALSE;
            gboolean has_map_lat = FALSE;
            char *row_p;
            int k;

            scan_excel_column_map(xml_buf->str, shared_strings, &col_map);

            for (k = 0; k < col_map.count; k++) {
                if (col_map.entries[k].field_type == 1) has_map_lon = TRUE;
                if (col_map.entries[k].field_type == 2) has_map_lat = TRUE;
            }

            if (!has_map_lon || !has_map_lat) {
                /* Safe default fallback if auto-detection misses coordinates */
                col_map.count = 0;
                col_map.entries[0] = (ColMapEntry){'A', 0};
                col_map.entries[1] = (ColMapEntry){'B', 1};
                col_map.entries[2] = (ColMapEntry){'C', 2};
                col_map.entries[3] = (ColMapEntry){'D', 3};
                col_map.entries[4] = (ColMapEntry){'E', 4};
                col_map.entries[5] = (ColMapEntry){'F', 4};
                col_map.count = 6;
            }

            row_p = xml_buf->str;

            while ((row_p = strstr(row_p, "<row")) != NULL) {
                char *row_end = strstr(row_p, "</row>");
                int current_id = 0;
                double current_lon = 0.0;
                double current_lat = 0.0;
                double current_azimuth = 0.0;
                double current_time = 0.0;
                gboolean has_id = FALSE, has_lon = FALSE, has_lat = FALSE, has_time = FALSE;
                char *cell_p;

                if (!row_end) break;

                cell_p = row_p;
                while (cell_p < row_end && (cell_p = strstr(cell_p, "<c r=\"")) != NULL) {
                    char col_letter = cell_p[6];
                    gboolean is_string = FALSE;
                    char *c_end = strchr(cell_p, '>');
                    char *v_start;

                    if (c_end) {
                        char *t_attr = strstr(cell_p, "t=\"s\"");
                        if (t_attr && t_attr < c_end) is_string = TRUE;
                    }

                    v_start = strstr(cell_p, "<v>");
                    if (v_start && v_start < row_end) {
                        char *v_end;
                        v_start += 3;
                        v_end = strstr(v_start, "</v>");

                        if (v_end && v_end < row_end) {
                            char val_str[128] = {0};
                            size_t len = v_end - v_start;

                            if (len < sizeof(val_str)) {
                                double val = 0.0;
                                int field_type = colmap_get_field_type(&col_map, col_letter);
                                strncpy(val_str, v_start, len);

                                if (is_string) {
                                    int ss_idx = atoi(val_str);
                                    if (ss_idx >= 0 && ss_idx < (int)shared_strings->len) {
                                        const char *cell_str = g_ptr_array_index(shared_strings, ss_idx);
                                        if (field_type == 3) {
                                            val = parse_azimuth_string(cell_str);
                                        } else if (field_type == 4) {
                                            int h = 0, m = 0, s = 0;
                                            if (sscanf(cell_str, "%d:%d:%d", &h, &m, &s) >= 2) {
                                                val = (h * 3600.0 + m * 60.0 + s) / 86400.0;
                                            } else {
                                                val = g_ascii_strtod(cell_str, NULL);
                                            }
                                        } else {
                                            val = parse_coord_string(cell_str);
                                        }
                                    }
                                } else {
                                    if (field_type == 1 || field_type == 2) {
                                        /* Lat/Lon: use parse_coord_string to handle comma decimals & direction */
                                        val = parse_coord_string(val_str);
                                    } else if (field_type == 3) {
                                        /* Azimuth numeric cell: user enters "335,044" meaning 335.044,
                                           but Excel stores 335044 (comma was thousands separator).
                                           If raw value > 360, assume 3 decimal places were intended. */
                                        val = g_ascii_strtod(val_str, NULL);
                                        if (val > 360.0) val = val / 1000.0;
                                    } else if (field_type == 4) {
                                        int h = 0, m = 0, s = 0;
                                        if (sscanf(val_str, "%d:%d:%d", &h, &m, &s) >= 2) {
                                            val = (h * 3600.0 + m * 60.0 + s) / 86400.0;
                                        } else {
                                            val = g_ascii_strtod(val_str, NULL);
                                        }
                                    } else {
                                        val = g_ascii_strtod(val_str, NULL);
                                    }
                                }

                                if (field_type == 0) {
                                    current_id = (int)val;
                                    has_id = TRUE;
                                } else if (field_type == 1) {
                                    current_lon = val;
                                    has_lon = TRUE;
                                } else if (field_type == 2) {
                                    current_lat = val;
                                    has_lat = TRUE;
                                } else if (field_type == 3) {
                                    current_azimuth = val;
                                } else if (field_type == 4) {
                                    current_time = val;
                                    has_time = TRUE;
                                }
                            }
                        }
                    }
                    cell_p++;
                }

                if (has_id && has_lat && has_lon && current_id > 0) {
                    GeolocationEntry *entry;
                    char key_str[64];

                    if (fabs(current_lon) < 90.0 && fabs(current_lat) > 90.0) {
                        double temp = current_lon;
                        current_lon = current_lat;
                        current_lat = temp;
                    }

                    entry = g_new0(GeolocationEntry, 1);
                    entry->no = current_id;
                    entry->longitude = current_lon;
                    entry->latitude = current_lat;
                    entry->azimuth_raw = current_azimuth;
                    entry->time_val = current_time;

                    snprintf(key_str, sizeof(key_str), "%d", current_id);
                    g_hash_table_insert(self->geo_table, g_strdup(key_str), entry);
                    loaded_count++;
                }
                row_p = row_end + 6;
            }
        }
        g_string_free(xml_buf, TRUE);
    }

    g_ptr_array_unref(shared_strings);
    g_free(path);

    if (loaded_count > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[INFO] Geolocation loaded %d entries from Excel cells.", loaded_count);
        stampy_show_toast(self, "Geolocation Excel loaded!");
        stampy_append_log(self, msg);
    } else {
        stampy_append_log(self, "[WARN] Failed to parse cell references from XLSX.");
    }
}

/* Imports dropped image files and folders. */
static void stampy_add_image_files(StampyWindow *self, GSList *files) {
    GSList *l;
    int added_count = 0;

    if (!self || !files) return;

    if (!self->image_files) {
        self->image_files = g_ptr_array_new_with_free_func(g_object_unref);
    }

    for (l = files; l != NULL; l = l->next) {
        if (l->data) {
            stampy_add_file_or_directory(self, G_FILE(l->data), &added_count);
        }
    }

    if (added_count > 0) {
        stampy_populate_file_listbox(self);
        stampy_append_log(self, "[INFO] Images imported successfully.");
        stampy_show_toast(self, "Images added!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
    }
}

/* Drag-and-drop handlers. */
static gboolean on_file_listbox_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)target; (void)x; (void)y;

    if (!self) return FALSE;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files) {
                stampy_add_image_files(self, files);
                g_slist_free(files);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean on_compass_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)target; (void)x; (void)y;

    if (!self) return FALSE;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files && files->data) {
                GFile *file = G_FILE(files->data);
                char *basename = NULL;

                if (self->file_compass) g_object_unref(self->file_compass);
                self->file_compass = g_object_ref(file);

                basename = g_file_get_basename(file);
                if (self->lbl_compass && basename) {
                    gtk_label_set_text(GTK_LABEL(self->lbl_compass), basename);
                }
                g_free(basename);

                /* Persisted, restored on next launch. */
                {
                    g_autofree char *dir_path = g_file_get_path(file);
                    stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER,
                                             dir_path ? dir_path : "");
                }

                stampy_show_toast(self, "Compass folder set!");
                stampy_append_log(self, "[INFO] Compass folder updated via Drag & Drop.");
                stampy_render_live_preview(self);
                stampy_update_clear_button_visibility(self);
                g_slist_free(files);
                return TRUE;
            }
            g_slist_free(files);
        }
    }
    return FALSE;
}

static gboolean on_docx_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)target; (void)x; (void)y;

    if (!self) return FALSE;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files && files->data) {
                GFile *file = G_FILE(files->data);
                char *path = g_file_get_path(file);

                if (path && g_str_has_suffix(path, ".docx")) {
                    char *basename = NULL;

                    if (self->file_watermark) g_object_unref(self->file_watermark);
                    self->file_watermark = g_object_ref(file);

                    basename = g_file_get_basename(file);
                    if (self->lbl_watermark && basename) {
                        gtk_label_set_text(GTK_LABEL(self->lbl_watermark), basename);
                    }
                    g_free(basename);

                    stampy_show_toast(self, "Watermark DOCX set!");
                    stampy_append_log(self, "[INFO] Watermark file updated via Drag & Drop.");
                    stampy_render_live_preview(self);
                    stampy_update_clear_button_visibility(self);
                    g_free(path);
                    g_slist_free(files);
                    return TRUE;
                }
                g_free(path);
            }
            g_slist_free(files);
        }
    }
    return FALSE;
}

static gboolean on_xlsx_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)target; (void)x; (void)y;

    if (!self) return FALSE;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files && files->data) {
                GFile *file = G_FILE(files->data);
                char *path = g_file_get_path(file);

                if (path && (g_str_has_suffix(path, ".xlsx") || g_str_has_suffix(path, ".xls"))) {
                    char *basename = NULL;

                    if (self->file_geolocation) g_object_unref(self->file_geolocation);
                    self->file_geolocation = g_object_ref(file);

                    parse_excel_file(self, file);

                    basename = g_file_get_basename(file);
                    if (self->lbl_geolocation && basename) {
                        gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), basename);
                    }
                    g_free(basename);

                    stampy_show_toast(self, "Geolocation XLSX loaded!");
                    stampy_append_log(self, "[INFO] Geolocation file loaded via Drag & Drop.");
                    stampy_render_live_preview(self);
                    stampy_update_clear_button_visibility(self);
                    g_free(path);
                    g_slist_free(files);
                    return TRUE;
                }
                g_free(path);
            }
            g_slist_free(files);
        }
    }
    return FALSE;
}

static gboolean on_json_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)target; (void)x; (void)y;

    if (!self) return FALSE;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files && files->data) {
                GFile *file = G_FILE(files->data);
                char *path = g_file_get_path(file);

                if (path && g_str_has_suffix(path, ".json")) {
                    stampy_load_preset_from_file(self, path);
                    stampy_scan_and_populate_presets(self);
                    stampy_show_toast(self, "Preset loaded via Drag & Drop!");
                    stampy_append_log(self, "[INFO] Preset loaded from dropped JSON file.");
                    g_free(path);
                    g_slist_free(files);
                    return TRUE;
                }
                g_free(path);
            }
            g_slist_free(files);
        }
    }
    return FALSE;
}

/* Asset file-picker handlers. */

static void on_compass_folder_selected(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(dialog, res, NULL);

    if (folder && self) {
        char *basename = NULL;

        if (self->file_compass) g_object_unref(self->file_compass);
        self->file_compass = g_object_ref(folder);

        basename = g_file_get_basename(folder);
        if (self->lbl_compass && basename) {
            gtk_label_set_text(GTK_LABEL(self->lbl_compass), basename);
        }
        g_free(basename);

        /* Persisted, restored on next launch. */
        {
            g_autofree char *dir_path = g_file_get_path(folder);
            stampy_config_set_string(STAMPY_CONFIG_KEY_COMPASS_FOLDER,
                                     dir_path ? dir_path : "");
        }

        stampy_show_toast(self, "Compass folder selected!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
    }
}

void stampy_open_compass_picker(StampyWindow *self) {
    GtkFileDialog *dialog;

    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Compass Asset Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL, on_compass_folder_selected, self);
}

static void on_btn_compass_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    stampy_open_compass_picker(STAMPY_WINDOW(user_data));
}

static void on_docx_file_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, NULL);

    if (file && self) {
        char *basename = NULL;

        if (self->file_watermark) g_object_unref(self->file_watermark);
        self->file_watermark = g_object_ref(file);

        basename = g_file_get_basename(file);
        if (self->lbl_watermark && basename) {
            gtk_label_set_text(GTK_LABEL(self->lbl_watermark), basename);
        }
        g_free(basename);

        stampy_show_toast(self, "Watermark file selected!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
    }
}

void stampy_open_watermark_picker(StampyWindow *self) {
    GtkFileDialog *dialog;

    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Watermark DOCX File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_docx_file_opened, self);
}

static void on_btn_docx_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    stampy_open_watermark_picker(STAMPY_WINDOW(user_data));
}

static void on_xlsx_file_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, NULL);

    if (file && self) {
        char *basename = NULL;

        if (self->file_geolocation) g_object_unref(self->file_geolocation);
        self->file_geolocation = g_object_ref(file);

        parse_excel_file(self, file);

        basename = g_file_get_basename(file);
        if (self->lbl_geolocation && basename) {
            gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), basename);
        }
        g_free(basename);

        stampy_show_toast(self, "Geolocation XLSX selected!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
    }
}

void stampy_open_geolocation_picker(StampyWindow *self) {
    GtkFileDialog *dialog;

    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Geolocation XLSX File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_xlsx_file_opened, self);
}

static void on_btn_xlsx_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    stampy_open_geolocation_picker(STAMPY_WINDOW(user_data));
}

/* Registers drop targets and button signals. */
void stampy_setup_file_handlers_and_dnd(StampyWindow *self) {
    GtkDropTarget *target_listbox;
    GtkDropTarget *target_compass;
    GtkDropTarget *target_docx;
    GtkDropTarget *target_xlsx;
    GtkDropTarget *target_json;

    if (!self) return;

    if (self->file_listbox) {
        target_listbox = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_listbox, "drop", G_CALLBACK(on_file_listbox_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->file_listbox), GTK_EVENT_CONTROLLER(target_listbox));
    }

    if (self->drop_area_frame) {
        GtkDropTarget *target_drop_frame = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_drop_frame, "drop", G_CALLBACK(on_file_listbox_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->drop_area_frame), GTK_EVENT_CONTROLLER(target_drop_frame));
    }

    if (self->btn_compass) {
        target_compass = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_compass, "drop", G_CALLBACK(on_compass_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->btn_compass), GTK_EVENT_CONTROLLER(target_compass));
        g_signal_connect(self->btn_compass, "clicked", G_CALLBACK(on_btn_compass_clicked), self);
    }

    if (self->btn_watermark) {
        target_docx = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_docx, "drop", G_CALLBACK(on_docx_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->btn_watermark), GTK_EVENT_CONTROLLER(target_docx));
        g_signal_connect(self->btn_watermark, "clicked", G_CALLBACK(on_btn_docx_clicked), self);
    }

    if (self->btn_geolocation) {
        target_xlsx = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_xlsx, "drop", G_CALLBACK(on_xlsx_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->btn_geolocation), GTK_EVENT_CONTROLLER(target_xlsx));
        g_signal_connect(self->btn_geolocation, "clicked", G_CALLBACK(on_btn_xlsx_clicked), self);
    }

    if (self->box_right_panel) {
        target_json = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_signal_connect(target_json, "drop", G_CALLBACK(on_json_drop), self);
        gtk_widget_add_controller(GTK_WIDGET(self->box_right_panel), GTK_EVENT_CONTROLLER(target_json));
    }
}
