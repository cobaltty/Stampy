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
   field_type: 0=No, 1=longitude(X), 2=latitude(Y), 3=azimuth(Azh),
               4=time(Jam), 5=PU group code */
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
                char *v_end;
                v_start += 3;
                v_end = strstr(v_start, "</v>");
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
                char *t_end;
                t_start += 3;
                t_end = strstr(t_start, "</t>");
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

            if (g_strcmp0(clean, "pu") == 0) {
                /* New sheets put PU in column B; elsewhere "PU" was the old
                 * photo-id header, so keep that legacy mapping intact. */
                field_type = (col_letter == 'B') ? 5 : 0;
            } else if (g_strcmp0(clean, "kodepu") == 0 ||
                       g_strcmp0(clean, "namapu") == 0 ||
                       g_strcmp0(clean, "kode") == 0 ||
                       g_strcmp0(clean, "kodeunit") == 0 ||
                       g_strcmp0(clean, "unit") == 0 ||
                       (strstr(clean, "pu") != NULL && strstr(clean, "no") == NULL)) {
                field_type = 5; /* PU group code */
            } else if (g_strcmp0(clean, "no") == 0 || g_strcmp0(clean, "nopu") == 0 ||
                       g_strcmp0(clean, "nomor") == 0 || g_strcmp0(clean, "id") == 0 ||
                       g_strcmp0(clean, "number") == 0 || g_strcmp0(clean, "idfoto") == 0 ||
                       g_strcmp0(clean, "photoid") == 0) {
                field_type = 0; /* No / ID */
            } else if (g_strcmp0(clean, "x") == 0 || strstr(clean, "long") != NULL ||
                       strstr(clean, "bujur") != NULL || strstr(clean, "east") != NULL ||
                       g_str_has_prefix(clean, "x")) {
                field_type = 1; /* Longitude */
            } else if (g_strcmp0(clean, "y") == 0 || strstr(clean, "lat") != NULL ||
                       strstr(clean, "lintang") != NULL || strstr(clean, "north") != NULL ||
                       g_str_has_prefix(clean, "y")) {
                field_type = 2; /* Latitude */
            } else if (strstr(clean, "azm") != NULL || strstr(clean, "azh") != NULL ||
                       strstr(clean, "azim") != NULL || strstr(clean, "bearing") != NULL ||
                       strstr(clean, "arah") != NULL || strstr(clean, "sudut") != NULL ||
                       strstr(clean, "kompas") != NULL || strstr(clean, "compass") != NULL ||
                       strstr(clean, "head") != NULL || strstr(clean, "derajat") != NULL ||
                       g_strcmp0(clean, "deg") == 0 || g_strcmp0(clean, "degree") == 0 ||
                       g_strcmp0(clean, "degrees") == 0 || g_str_has_prefix(clean, "deg")) {
                field_type = 3; /* Azimuth */
            } else if (strstr(clean, "jam") != NULL || strstr(clean, "time") != NULL ||
                       strstr(clean, "waktu") != NULL || strstr(clean, "clock") != NULL ||
                       strstr(clean, "pukul") != NULL) {
                field_type = 4; /* Time */
            } else if (strstr(clean, "tanggal") != NULL || strstr(clean, "tgl") != NULL ||
                       strstr(clean, "date") != NULL) {
                field_type = 6; /* Date */
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

static gboolean parse_date_string(const char *str, char out[16]) {
    int a = 0, b = 0, c = 0;
    int y = 0, m = 0, d = 0;
    char *extracted = NULL;
    double num_val;

    if (!str || !*str) return FALSE;

    if (stampy_extract_docx_date(str, &extracted) && extracted) {
        g_strlcpy(out, extracted, 16);
        g_free(extracted);
        return TRUE;
    }

    num_val = g_ascii_strtod(str, NULL);
    if (num_val > 30000.0 && num_val < 60000.0) {
        GDateTime *base = g_date_time_new_utc(1899, 12, 30, 0, 0, 0);
        if (base) {
            GDateTime *dt = g_date_time_add_days(base, (gint)num_val);
            if (dt) {
                gchar *fmt = g_date_time_format(dt, "%Y:%m:%d");
                if (fmt) {
                    g_strlcpy(out, fmt, 16);
                    g_free(fmt);
                }
                g_date_time_unref(dt);
            }
            g_date_time_unref(base);
            if (out[0] != '\0') return TRUE;
        }
    }

    if (sscanf(str, "%d/%d/%d", &a, &b, &c) == 3 ||
        sscanf(str, "%d-%d-%d", &a, &b, &c) == 3 ||
        sscanf(str, "%d.%d.%d", &a, &b, &c) == 3) {
        if (a > 1000) {
            y = a;
            if (b > 12) { d = b; m = c; }
            else { m = b; d = c; }
        } else if (c > 1000) {
            y = c;
            if (a > 12) {
                d = a; m = b;
            } else if (b > 12) {
                m = a; d = b;
            } else {
                m = a; d = b;
            }
        } else if (a > 31) {
            y = (a < 70) ? (2000 + a) : (1900 + a);
            m = b; d = c;
        } else if (c > 31) {
            y = (c < 70) ? (2000 + c) : (1900 + c);
            d = a; m = b;
        }
        if (y > 1900 && y < 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            snprintf(out, 16, "%04d:%02d:%02d", y, m, d);
            return TRUE;
        }
    }
    return FALSE;
}

static GPtrArray *parse_csv_row(const char *line, char sep) {
    GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
    GString *cur = g_string_new("");
    gboolean in_quotes = FALSE;
    const char *p = line;

    while (*p) {
        if (*p == '"') {
            if (in_quotes && *(p + 1) == '"') {
                g_string_append_c(cur, '"');
                p += 2;
                continue;
            }
            in_quotes = !in_quotes;
        } else if (*p == sep && !in_quotes) {
            char *s = g_strdup(cur->str);
            g_strstrip(s);
            g_ptr_array_add(tokens, s);
            g_string_truncate(cur, 0);
        } else if ((*p == '\r' || *p == '\n') && !in_quotes) {
            break;
        } else {
            g_string_append_c(cur, *p);
        }
        p++;
    }
    {
        char *s = g_strdup(cur->str);
        g_strstrip(s);
        g_ptr_array_add(tokens, s);
    }
    g_string_free(cur, TRUE);
    return tokens;
}

/* Parses an XLSX or CSV file for geolocation coordinates, azimuth, date, and time. */
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
    if (!g_str_has_suffix(lower_path, ".xlsx") && !g_str_has_suffix(lower_path, ".xls") && !g_str_has_suffix(lower_path, ".csv")) {
        stampy_append_log(self, "[WARN] Not a valid Excel or CSV file!");
        g_free(lower_path);
        g_free(path);
        return;
    }

    if (!self->geo_table) {
        self->geo_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    } else {
        g_hash_table_remove_all(self->geo_table);
    }

    if (g_str_has_suffix(lower_path, ".csv")) {
        char line_buf[4096];
        int col_types[64];
        int num_cols = 0;
        gboolean first_row = TRUE;
        char sep = ',';

        stampy_append_log(self, "[INFO] Reading Geolocation CSV file...");
        fp = fopen(path, "r");
        if (!fp) {
            stampy_append_log(self, "[WARN] Could not open CSV file!");
            g_free(lower_path);
            g_free(path);
            return;
        }

        while (fgets(line_buf, sizeof(line_buf), fp)) {
            GPtrArray *tokens;
            int c;
            int current_id = 0;
            double current_lon = 0.0, current_lat = 0.0, current_azimuth = 0.0, current_time = 0.0;
            char current_pu[64] = {0};
            char current_date[16] = {0};
            gboolean has_id = FALSE, has_lon = FALSE, has_lat = FALSE;

            if (first_row) {
                if (strchr(line_buf, ';') != NULL && strchr(line_buf, ',') == NULL) {
                    sep = ';';
                }
            }
            tokens = parse_csv_row(line_buf, sep);
            if (!tokens || tokens->len == 0) {
                if (tokens) g_ptr_array_unref(tokens);
                continue;
            }

            if (first_row) {
                num_cols = (int)tokens->len;
                if (num_cols > 64) num_cols = 64;
                for (c = 0; c < num_cols; c++) {
                    const char *h = g_ptr_array_index(tokens, c);
                    char clean[64] = {0};
                    int clen = 0;
                    for (const char *p = h; *p && clen < (int)sizeof(clean) - 1; p++) {
                        if (g_ascii_isalnum(*p)) clean[clen++] = g_ascii_tolower(*p);
                    }
                    clean[clen] = '\0';
                    col_types[c] = -1;
                    if (g_strcmp0(clean, "pu") == 0) {
                        col_types[c] = (c == 1) ? 5 : 0;
                    } else if (g_strcmp0(clean, "kodepu") == 0 ||
                               g_strcmp0(clean, "namapu") == 0 ||
                               g_strcmp0(clean, "kode") == 0 ||
                               g_strcmp0(clean, "kodeunit") == 0 ||
                               g_strcmp0(clean, "unit") == 0 ||
                               (strstr(clean, "pu") != NULL && strstr(clean, "no") == NULL)) {
                        col_types[c] = 5;
                    } else if (g_strcmp0(clean, "no") == 0 || g_strcmp0(clean, "nopu") == 0 ||
                               g_strcmp0(clean, "nomor") == 0 || g_strcmp0(clean, "id") == 0 ||
                               g_strcmp0(clean, "number") == 0 || g_strcmp0(clean, "idfoto") == 0 ||
                               g_strcmp0(clean, "photoid") == 0) {
                        col_types[c] = 0;
                    } else if (g_strcmp0(clean, "x") == 0 || strstr(clean, "long") != NULL ||
                               strstr(clean, "bujur") != NULL || strstr(clean, "east") != NULL ||
                               g_str_has_prefix(clean, "x")) {
                        col_types[c] = 1;
                    } else if (g_strcmp0(clean, "y") == 0 || strstr(clean, "lat") != NULL ||
                               strstr(clean, "lintang") != NULL || strstr(clean, "north") != NULL ||
                               g_str_has_prefix(clean, "y")) {
                        col_types[c] = 2;
                    } else if (strstr(clean, "azm") != NULL || strstr(clean, "azh") != NULL ||
                               strstr(clean, "azim") != NULL || strstr(clean, "bearing") != NULL ||
                               strstr(clean, "arah") != NULL || strstr(clean, "sudut") != NULL ||
                               strstr(clean, "kompas") != NULL || strstr(clean, "compass") != NULL ||
                               strstr(clean, "head") != NULL || strstr(clean, "derajat") != NULL ||
                               g_strcmp0(clean, "deg") == 0 || g_strcmp0(clean, "degree") == 0 ||
                               g_strcmp0(clean, "degrees") == 0 || g_str_has_prefix(clean, "deg")) {
                        col_types[c] = 3;
                    } else if (strstr(clean, "jam") != NULL || strstr(clean, "time") != NULL ||
                               strstr(clean, "waktu") != NULL || strstr(clean, "clock") != NULL ||
                               strstr(clean, "pukul") != NULL) {
                        col_types[c] = 4;
                    } else if (strstr(clean, "tanggal") != NULL || strstr(clean, "tgl") != NULL ||
                               strstr(clean, "date") != NULL) {
                        col_types[c] = 6;
                    }
                }
                first_row = FALSE;
                g_ptr_array_unref(tokens);
                continue;
            }

            for (c = 0; c < (int)tokens->len && c < num_cols; c++) {
                const char *val_str = g_ptr_array_index(tokens, c);
                int ft = col_types[c];
                if (ft == 0) {
                    current_id = atoi(val_str);
                    has_id = TRUE;
                } else if (ft == 1) {
                    current_lon = parse_coord_string(val_str);
                    has_lon = TRUE;
                } else if (ft == 2) {
                    current_lat = parse_coord_string(val_str);
                    has_lat = TRUE;
                } else if (ft == 3) {
                    current_azimuth = parse_azimuth_string(val_str);
                } else if (ft == 4) {
                    int h = 0, m = 0, s = 0;
                    if (sscanf(val_str, "%d:%d:%d", &h, &m, &s) >= 2) {
                        current_time = (h * 3600.0 + m * 60.0 + s) / 86400.0;
                    } else {
                        current_time = g_ascii_strtod(val_str, NULL);
                    }
                } else if (ft == 5) {
                    g_strlcpy(current_pu, val_str, sizeof(current_pu));
                    g_strstrip(current_pu);
                } else if (ft == 6) {
                    parse_date_string(val_str, current_date);
                }
            }
            g_ptr_array_unref(tokens);

            if (has_id && has_lat && has_lon && current_id > 0) {
                GeolocationEntry *entry;
                char *key_str;

                if (fabs(current_lon) < 90.0 && fabs(current_lat) > 90.0) {
                    double temp = current_lon;
                    current_lon = current_lat;
                    current_lat = temp;
                }

                entry = g_new0(GeolocationEntry, 1);
                entry->no = current_id;
                g_strlcpy(entry->pu_code, current_pu, sizeof(entry->pu_code));
                entry->longitude = current_lon;
                entry->latitude = current_lat;
                entry->azimuth_raw = current_azimuth;
                entry->time_val = current_time;
                if (current_date[0] != '\0') {
                    g_strlcpy(entry->date_str, current_date, sizeof(entry->date_str));
                }

                key_str = stampy_make_geo_key(current_pu, current_id);
                g_hash_table_insert(self->geo_table, key_str, entry);
                loaded_count++;
            }
        }
        fclose(fp);

        g_free(lower_path);
        g_free(path);

        if (loaded_count > 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[INFO] Geolocation loaded %d entries from CSV.", loaded_count);
            stampy_show_toast(self, "Geolocation CSV loaded!");
            stampy_append_log(self, msg);
        } else {
            stampy_append_log(self, "[WARN] Failed to parse entries from CSV.");
        }
        return;
    }

    g_free(lower_path);

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
            gboolean has_map_azh = FALSE;
            gboolean has_map_pu = FALSE;
            gboolean has_map_id = FALSE;
            char *row_p;
            int k;

            scan_excel_column_map(xml_buf->str, shared_strings, &col_map);

            for (k = 0; k < col_map.count; k++) {
                if (col_map.entries[k].field_type == 1) has_map_lon = TRUE;
                if (col_map.entries[k].field_type == 2) has_map_lat = TRUE;
                if (col_map.entries[k].field_type == 3) has_map_azh = TRUE;
                if (col_map.entries[k].field_type == 5) has_map_pu = TRUE;
            }

            /* New sheets keep the PU group code in column B. Only fall back to
             * that position when B is not already a coordinate/time column. */
            if (!has_map_pu && col_map.count < 16) {
                int b_type = colmap_get_field_type(&col_map, 'B');
                if (b_type != 1 && b_type != 2 && b_type != 3 && b_type != 4) {
                    if (b_type >= 0) {
                        for (k = 0; k < col_map.count; k++) {
                            if (col_map.entries[k].col_letter == 'B') {
                                col_map.entries[k].field_type = 5;
                                break;
                            }
                        }
                    } else {
                        col_map.entries[col_map.count].col_letter = 'B';
                        col_map.entries[col_map.count].field_type = 5;
                        col_map.count++;
                    }
                }
            }

            /* Column A always carries the per-PU sequence number. */
            has_map_id = FALSE;
            for (k = 0; k < col_map.count; k++) {
                if (col_map.entries[k].field_type == 0) has_map_id = TRUE;
            }
            if (!has_map_id && col_map.count < 16 &&
                colmap_get_field_type(&col_map, 'A') < 0) {
                col_map.entries[col_map.count].col_letter = 'A';
                col_map.entries[col_map.count].field_type = 0;
                col_map.count++;
            }

            if (!has_map_lon || !has_map_lat || !has_map_azh) {
                GString *missing_cols = g_string_new("");
                gboolean first_m = TRUE;
                char *b_title;
                char *f_base;

                if (!has_map_lon) {
                    g_string_append(missing_cols, "X");
                    first_m = FALSE;
                }
                if (!has_map_lat) {
                    if (!first_m) g_string_append(missing_cols, ", ");
                    g_string_append(missing_cols, "Y");
                    first_m = FALSE;
                }
                if (!has_map_azh) {
                    if (!first_m) g_string_append(missing_cols, ", ");
                    g_string_append(missing_cols, "Azimuth");
                }

                b_title = g_strdup_printf("Critical: Missing coordinates (%s not detected)", missing_cols->str);
                if (self->banner_critical_coords) {
                    adw_banner_set_title(ADW_BANNER(self->banner_critical_coords), b_title);
                    adw_banner_set_revealed(ADW_BANNER(self->banner_critical_coords), TRUE);
                }
                g_free(b_title);

                f_base = g_file_get_basename(file);
                g_free(self->critical_coords_details);
                self->critical_coords_details = g_strdup_printf(
                    "Auto-parsing could not detect required column(s) in '%s':\n\n"
                    "• Missing: %s\n\n"
                    "Please ensure the spreadsheet header contains recognized column names:\n"
                    "• X (or Longitude)\n"
                    "• Y (or Latitude)\n"
                    "• Azimuth (or AZM, Azh, Arah, Bearing, Sudut)",
                    f_base ? f_base : "Excel", missing_cols->str);
                g_free(f_base);
                g_string_free(missing_cols, TRUE);

                /* Safe default fallback if auto-detection misses coordinates.
                 * New layout: A=No, B=PU, C=X, D=Y, E=Azimuth, F=Time. */
                col_map.count = 0;
                col_map.entries[0] = (ColMapEntry){'A', 0};
                col_map.entries[1] = (ColMapEntry){'B', 5};
                col_map.entries[2] = (ColMapEntry){'C', 1};
                col_map.entries[3] = (ColMapEntry){'D', 2};
                col_map.entries[4] = (ColMapEntry){'E', 3};
                col_map.entries[5] = (ColMapEntry){'F', 4};
                col_map.count = 6;
                stampy_append_log(self, "[INFO] Using default column mapping: A=No B=PU C=X D=Y E=Azimuth");
            } else {
                const char *f_names[] = {"No", "Lon", "Lat", "Azimuth", "Time", "PU", "Date"};
                GString *map_str = g_string_new("[INFO] Excel columns mapped: ");

                if (self->banner_critical_coords) {
                    adw_banner_set_revealed(ADW_BANNER(self->banner_critical_coords), FALSE);
                }
                g_clear_pointer(&self->critical_coords_details, g_free);

                for (k = 0; k < col_map.count; k++) {
                    int ft = col_map.entries[k].field_type;
                    if (ft >= 0 && ft <= 6) {
                        g_string_append_printf(map_str, "%c=%s ",
                                               col_map.entries[k].col_letter, f_names[ft]);
                    }
                }
                stampy_append_log(self, map_str->str);
                g_string_free(map_str, TRUE);
            }

            row_p = xml_buf->str;

            while ((row_p = strstr(row_p, "<row")) != NULL) {
                char *row_end = strstr(row_p, "</row>");
                int current_id = 0;
                double current_lon = 0.0;
                double current_lat = 0.0;
                double current_azimuth = 0.0;
                double current_time = 0.0;
                char current_pu[64] = {0};
                char current_date[16] = {0};
                gboolean has_id = FALSE, has_lon = FALSE, has_lat = FALSE;
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
                                        if (field_type == 5) {
                                            g_strlcpy(current_pu, cell_str, sizeof(current_pu));
                                            g_strstrip(current_pu);
                                        } else if (field_type == 0) {
                                            val = g_ascii_strtod(cell_str, NULL);
                                        } else if (field_type == 3) {
                                            val = parse_azimuth_string(cell_str);
                                        } else if (field_type == 4) {
                                            int h = 0, m = 0, s = 0;
                                            if (sscanf(cell_str, "%d:%d:%d", &h, &m, &s) >= 2) {
                                                val = (h * 3600.0 + m * 60.0 + s) / 86400.0;
                                            } else {
                                                val = g_ascii_strtod(cell_str, NULL);
                                            }
                                        } else if (field_type == 6) {
                                            parse_date_string(cell_str, current_date);
                                        } else {
                                            val = parse_coord_string(cell_str);
                                        }
                                    }
                                } else {
                                    if (field_type == 5) {
                                        g_strlcpy(current_pu, val_str, sizeof(current_pu));
                                        g_strstrip(current_pu);
                                    } else if (field_type == 1 || field_type == 2) {
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
                                    } else if (field_type == 6) {
                                        val = g_ascii_strtod(val_str, NULL);
                                        if (val > 30000.0 && val < 60000.0) {
                                            GDateTime *base = g_date_time_new_utc(1899, 12, 30, 0, 0, 0);
                                            if (base) {
                                                GDateTime *dt = g_date_time_add_days(base, (gint)val);
                                                if (dt) {
                                                    gchar *fmt = g_date_time_format(dt, "%Y:%m:%d");
                                                    if (fmt) {
                                                        g_strlcpy(current_date, fmt, sizeof(current_date));
                                                        g_free(fmt);
                                                    }
                                                    g_date_time_unref(dt);
                                                }
                                                g_date_time_unref(base);
                                            }
                                        } else {
                                            parse_date_string(val_str, current_date);
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
                                }
                            }
                        }
                    }
                    cell_p++;
                }

                if (has_id && has_lat && has_lon && current_id > 0) {
                    GeolocationEntry *entry;
                    char *key_str;

                    if (fabs(current_lon) < 90.0 && fabs(current_lat) > 90.0) {
                        double temp = current_lon;
                        current_lon = current_lat;
                        current_lat = temp;
                    }

                    entry = g_new0(GeolocationEntry, 1);
                    entry->no = current_id;
                    g_strlcpy(entry->pu_code, current_pu, sizeof(entry->pu_code));
                    entry->longitude = current_lon;
                    entry->latitude = current_lat;
                    entry->azimuth_raw = current_azimuth;
                    entry->time_val = current_time;
                    if (current_date[0] != '\0') {
                        g_strlcpy(entry->date_str, current_date, sizeof(entry->date_str));
                    }

                    key_str = stampy_make_geo_key(current_pu, current_id);
                    g_hash_table_insert(self->geo_table, key_str, entry);
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

static gboolean file_key_equal(gconstpointer a, gconstpointer b) {
    return g_file_equal((GFile *)a, (GFile *)b);
}

/* Folder scanning runs off the main thread so large drops never freeze the UI. */
typedef struct {
    GPtrArray *sources;
    GPtrArray *images;
    GHashTable *dir_times;
    guint generation;
} ScanTaskData;

static void scan_task_data_free(gpointer data) {
    ScanTaskData *scan = data;
    if (scan) {
        if (scan->sources) g_ptr_array_unref(scan->sources);
        if (scan->images) g_ptr_array_unref(scan->images);
        if (scan->dir_times) g_hash_table_unref(scan->dir_times);
        g_free(scan);
    }
}

static gboolean is_supported_image_name(const char *name) {
    const char *ext;

    if (!name) return FALSE;
    ext = strrchr(name, '.');
    if (!ext) return FALSE;

    return (g_ascii_strcasecmp(ext, ".jpg") == 0 ||
            g_ascii_strcasecmp(ext, ".jpeg") == 0 ||
            g_ascii_strcasecmp(ext, ".png") == 0);
}

/* Directories created by Stampy's own export. Scanning them again feeds
 * previously exported copies back into the list as duplicates. */
static gboolean is_export_output_dir(const char *name) {
    const char *pos;
    const char *tail;
    if (!name) return FALSE;
    pos = g_strrstr(name, "_stamped");
    if (!pos) pos = g_strrstr(name, "_Stamped");
    if (!pos) pos = g_strrstr(name, "_STAMPED");
    if (!pos) return FALSE;

    tail = pos + 8;
    if (*tail == '\0') return TRUE;
    if (*tail == '(') {
        tail++;
        if (!g_ascii_isdigit(*tail)) return FALSE;
        while (g_ascii_isdigit(*tail)) tail++;
        if (*tail == ')' && *(tail + 1) == '\0') return TRUE;
    }
    return FALSE;
}

/* Compass asset folders (e.g. "Kompas Monev", "compass") must not be scanned as photos. */
static gboolean is_compass_dir_name(const char *name) {
    const char *p;
    if (!name) return FALSE;
    for (p = name; *p; p++) {
        if (g_ascii_strncasecmp(p, "kompas", 6) == 0 ||
            g_ascii_strncasecmp(p, "compass", 7) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Reuses the type already delivered by the directory enumerator, so a rescan
 * does one readdir per directory instead of one stat per file. Directory
 * mtimes are recorded to cheaply detect later changes. */
static void collect_images(GPtrArray *out, GFile *file, GFileType type,
                           GHashTable *dir_times, GCancellable *cancel) {
    if (g_cancellable_is_cancelled(cancel)) return;

    if (type == G_FILE_TYPE_UNKNOWN)
        type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);

    if (type == G_FILE_TYPE_DIRECTORY) {
        GFileInfo *dir_info;
        GFileEnumerator *e;

        dir_info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                     G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (dir_info) {
            gint64 mtime = (gint64)g_file_info_get_attribute_uint64(
                dir_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            gint64 *stored = g_new(gint64, 1);

            *stored = mtime;
            g_hash_table_insert(dir_times, g_object_ref(file), stored);
            g_object_unref(dir_info);
        }

        e = g_file_enumerate_children(file,
                                      "standard::name,standard::type",
                                      G_FILE_QUERY_INFO_NONE,
                                      NULL, NULL);
        if (e) {
            GFileInfo *info;

            while (!g_cancellable_is_cancelled(cancel) &&
                   (info = g_file_enumerator_next_file(e, NULL, NULL)) != NULL) {
                const char *name = g_file_info_get_name(info);
                GFileType child_type = g_file_info_get_file_type(info);

                if (name && *name != '.' &&
                    !(child_type == G_FILE_TYPE_DIRECTORY &&
                      (is_export_output_dir(name) || is_compass_dir_name(name)))) {
                    GFile *child = g_file_get_child(file, name);
                    collect_images(out, child, child_type, dir_times, cancel);
                    g_object_unref(child);
                }
                g_object_unref(info);
            }
            g_file_enumerator_close(e, NULL, NULL);
            g_object_unref(e);
        }
    } else {
        char *basename = g_file_get_basename(file);

        if (is_supported_image_name(basename))
            g_ptr_array_add(out, g_object_ref(file));
        g_free(basename);
    }
}

static void scan_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    ScanTaskData *scan = task_data;
    GPtrArray *images;
    guint i;

    (void)source;

    for (i = 0; i < scan->sources->len; i++) {
        GFile *src_file = g_ptr_array_index(scan->sources, i);
        GFileType type = g_file_query_file_type(src_file, G_FILE_QUERY_INFO_NONE, NULL);

        collect_images(scan->images, src_file, type, scan->dir_times, cancel);
        if (g_cancellable_is_cancelled(cancel)) break;
    }

    if (g_cancellable_is_cancelled(cancel)) {
        g_task_return_error_if_cancelled(task);
        return;
    }

    images = scan->images;
    scan->images = NULL;
    g_ptr_array_sort(images, stampy_compare_files_naturally);
    g_task_return_pointer(task, images, (GDestroyNotify)g_ptr_array_unref);
}

static gboolean scan_pulse_cb(gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);

    if (self && self->progress_bar)
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(self->progress_bar));
    return G_SOURCE_CONTINUE;
}

static void stop_scan_ui(StampyWindow *self) {
    if (self->scan_pulse_id > 0) {
        g_source_remove(self->scan_pulse_id);
        self->scan_pulse_id = 0;
    }
    if (!self->export_active) {
        if (self->progress_bar) gtk_widget_set_visible(self->progress_bar, FALSE);
        gtk_window_set_title(GTK_WINDOW(self), "Stampy");
    }
}

static void scan_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GTask *task = G_TASK(res);
    ScanTaskData *scan = g_task_get_task_data(task);
    GError *error = NULL;
    GPtrArray *images = g_task_propagate_pointer(task, &error);
    GHashTable *known;
    guint i;
    int added_count = 0;

    if (!self) {
        if (images) g_ptr_array_unref(images);
        g_clear_error(&error);
        return;
    }

    /* Ignore results from a scan that has already been superseded. */
    if (!scan || scan->generation != self->scan_generation) {
        if (images) g_ptr_array_unref(images);
        g_clear_error(&error);
        return;
    }

    g_clear_object(&self->scan_cancel);
    stop_scan_ui(self);

    if (!images) {
        stampy_append_log(self, "[WARN] Folder scan cancelled or failed.");
        g_clear_error(&error);
        return;
    }

    if (!self->image_files) {
        self->image_files = g_ptr_array_new_with_free_func(g_object_unref);
    }

    known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (i = 0; i < self->image_files->len; i++) {
        char *path = g_file_get_path(g_ptr_array_index(self->image_files, i));
        if (path) g_hash_table_add(known, path);
    }

    for (i = 0; i < images->len; i++) {
        GFile *file = g_ptr_array_index(images, i);
        char *path = g_file_get_path(file);
        gboolean exists = path && g_hash_table_contains(known, path);

        if (!exists) {
            if (path) g_hash_table_add(known, g_strdup(path));
            g_ptr_array_add(self->image_files, g_object_ref(file));
            added_count++;
        }
        g_free(path);
    }

    g_hash_table_unref(known);
    g_ptr_array_unref(images);

    /* Remember the directory tree so an unchanged re-drop can skip scanning. */
    {
        GPtrArray *cached_sources = g_ptr_array_new_with_free_func(g_object_unref);

        for (i = 0; i < scan->sources->len; i++)
            g_ptr_array_add(cached_sources, g_object_ref(g_ptr_array_index(scan->sources, i)));

        if (self->scan_cache_sources) g_ptr_array_unref(self->scan_cache_sources);
        self->scan_cache_sources = cached_sources;

        if (self->scan_cache_dirs) g_hash_table_unref(self->scan_cache_dirs);
        self->scan_cache_dirs = scan->dir_times;
        scan->dir_times = NULL;
    }

    if (added_count > 0) {
        GSList *sources = NULL;

        for (i = 0; i < scan->sources->len; i++)
            sources = g_slist_prepend(sources, g_object_ref(g_ptr_array_index(scan->sources, i)));

        stampy_auto_detect_companion_files(self, sources);
        g_slist_free_full(sources, g_object_unref);

        if (scan->sources->len > 0)
            stampy_set_import_root(self, g_ptr_array_index(scan->sources, 0));
        stampy_prepare_default_export_folder(self);
        stampy_populate_file_listbox(self);
        stampy_append_log(self, "[INFO] Images imported successfully.");
        stampy_show_toast(self, "Images added!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
        stampy_check_missing_data(self);
    }

    g_clear_error(&error);
}

static gboolean scan_cache_sources_match(StampyWindow *self, GSList *files) {
    GSList *l;
    guint count = 0;
    guint i;

    if (!self->scan_cache_sources || !self->scan_cache_dirs) return FALSE;

    for (l = files; l != NULL; l = l->next)
        if (l->data) count++;

    if (count != self->scan_cache_sources->len) return FALSE;

    for (l = files; l != NULL; l = l->next) {
        GFile *file = G_FILE(l->data);
        gboolean found = FALSE;

        if (!file) continue;
        for (i = 0; i < self->scan_cache_sources->len; i++) {
            if (g_file_equal(file, g_ptr_array_index(self->scan_cache_sources, i))) {
                found = TRUE;
                break;
            }
        }
        if (!found) return FALSE;
    }

    return TRUE;
}

/* Every cached directory must still exist with the same mtime. A file add or
 * remove updates its parent directory's mtime, so this detects real changes
 * without walking the tree again. */
static gboolean scan_cache_is_fresh(StampyWindow *self) {
    GHashTableIter iter;
    gpointer key, value;

    if (!self->scan_cache_dirs) return FALSE;

    g_hash_table_iter_init(&iter, self->scan_cache_dirs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GFile *dir = G_FILE(key);
        gint64 expected = *(gint64 *)value;
        GFileInfo *info = g_file_query_info(dir, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
        gint64 actual;

        if (!info) return FALSE;
        actual = (gint64)g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_object_unref(info);

        if (actual != expected) return FALSE;
    }

    return TRUE;
}

/* Imports dropped image files and folders asynchronously. */
void stampy_add_image_files(StampyWindow *self, GSList *files) {
    ScanTaskData *scan;
    GTask *task;
    GCancellable *cancellable;
    GSList *l;

    if (!self || !files) return;

    /* Same roots, same directory mtimes: nothing to scan or add. */
    if (scan_cache_sources_match(self, files) && scan_cache_is_fresh(self)) {
        stampy_show_toast(self, "Folder already imported");
        stampy_append_log(self, "[INFO] Folder unchanged; skipped rescan.");
        return;
    }

    if (self->scan_cancel) {
        g_cancellable_cancel(self->scan_cancel);
        g_clear_object(&self->scan_cancel);
    }
    self->scan_generation++;

    scan = g_new0(ScanTaskData, 1);
    scan->sources = g_ptr_array_new_with_free_func(g_object_unref);
    scan->images = g_ptr_array_new_with_free_func(g_object_unref);
    scan->dir_times = g_hash_table_new_full(g_file_hash, file_key_equal,
                                            g_object_unref, g_free);
    scan->generation = self->scan_generation;

    for (l = files; l != NULL; l = l->next) {
        if (l->data) g_ptr_array_add(scan->sources, g_object_ref(l->data));
    }

    cancellable = g_cancellable_new();
    task = g_task_new(self, cancellable, scan_done, self);
    g_task_set_task_data(task, scan, scan_task_data_free);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
    self->scan_cancel = cancellable;

    if (self->progress_bar) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), 0.0);
        gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(self->progress_bar), 0.05);
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(self->progress_bar));
        gtk_widget_set_visible(self->progress_bar, TRUE);
    }
    gtk_window_set_title(GTK_WINDOW(self), "Scanning…");

    if (self->scan_pulse_id > 0) g_source_remove(self->scan_pulse_id);
    self->scan_pulse_id = g_timeout_add(100, scan_pulse_cb, self);

    stampy_append_log(self, "[INFO] Scanning folder for images…");
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

                    parse_docx_file(self, file);

                    basename = g_file_get_basename(file);
                    if (self->lbl_watermark && basename) {
                        gtk_label_set_text(GTK_LABEL(self->lbl_watermark), basename);
                    }
                    g_free(basename);

                    stampy_show_toast(self, "Watermark DOCX set!");
                    stampy_append_log(self, "[INFO] Watermark file updated via Drag & Drop.");
                    stampy_render_live_preview(self);
                    stampy_update_clear_button_visibility(self);
                    stampy_check_missing_data(self);
                    stampy_prepare_default_export_folder(self);
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

                if (path && (g_str_has_suffix(path, ".xlsx") || g_str_has_suffix(path, ".xls") || g_str_has_suffix(path, ".csv"))) {
                    char *basename = NULL;

                    if (self->file_geolocation) g_object_unref(self->file_geolocation);
                    self->file_geolocation = g_object_ref(file);

                    parse_excel_file(self, file);

                    basename = g_file_get_basename(file);
                    if (self->lbl_geolocation && basename) {
                        gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), basename);
                    }
                    g_free(basename);

                    stampy_show_toast(self, "Geolocation file loaded!");
                    stampy_append_log(self, "[INFO] Geolocation file loaded via Drag & Drop.");
                    stampy_render_live_preview(self);
                    stampy_update_clear_button_visibility(self);
                    stampy_check_missing_data(self);
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

        parse_docx_file(self, file);

        basename = g_file_get_basename(file);
        if (self->lbl_watermark && basename) {
            gtk_label_set_text(GTK_LABEL(self->lbl_watermark), basename);
        }
        g_free(basename);

        stampy_show_toast(self, "Watermark file selected!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
        stampy_check_missing_data(self);
        stampy_prepare_default_export_folder(self);
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

        stampy_show_toast(self, "Geolocation file selected!");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
        stampy_check_missing_data(self);
    }
}

void stampy_open_geolocation_picker(StampyWindow *self) {
    GtkFileDialog *dialog;

    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Geolocation File (XLSX, CSV)");
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

    if (self->banner_landscape) {
        g_signal_connect(self->banner_landscape, "button-clicked",
                         G_CALLBACK(stampy_on_landscape_banner_clicked), self);
    }

    if (self->banner_missing_data) {
        g_signal_connect(self->banner_missing_data, "button-clicked",
                         G_CALLBACK(stampy_on_missing_data_banner_clicked), self);
    }

    if (self->banner_critical_coords) {
        g_signal_connect(self->banner_critical_coords, "button-clicked",
                         G_CALLBACK(stampy_on_critical_coords_banner_clicked), self);
    }

    if (self->banner_critical_docx) {
        g_signal_connect(self->banner_critical_docx, "button-clicked",
                         G_CALLBACK(stampy_on_critical_docx_banner_clicked), self);
    }

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
