#include "stampy-window-private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Extracts the digit sequence from a string ("2501.jpg" -> "2501"). */
static char *extract_pure_id(const char *input) {
    GString *str;
    char *dup;
    char *dot;
    const char *p;

    if (!input) return NULL;

    str = g_string_new("");
    for (p = input; *p; p++) {
        if (g_ascii_isdigit(*p)) {
            g_string_append_c(str, *p);
        }
    }

    if (str->len > 0) {
        return g_string_free(str, FALSE);
    }
    g_string_free(str, TRUE);

    dup = g_strdup(input);
    dot = strrchr(dup, '.');
    if (dot) *dot = '\0';
    return dup;
}

/* Normalizes a raw azimuth value into the [0, 360) range. */
static double parse_clean_azimuth_from_excel(double raw_azh) {
    double val = fabs(raw_azh);
    return fmod(val, 360.0);
}

/* Formats coordinates with comma decimals plus the compass heading. */
void format_geolocation_text(const GeolocationEntry *entry, char *lat_str, char *lon_str, double *compass_deg) {
    double abs_lat;
    double abs_lon;
    char lat_ref;
    char lon_ref;
    char *p;

    if (!entry) return;

    abs_lat = fabs(entry->latitude);
    abs_lon = fabs(entry->longitude);
    lat_ref = (entry->latitude >= 0.0) ? 'N' : 'S';
    lon_ref = (entry->longitude >= 0.0) ? 'E' : 'W';

    if (lat_str) {
        snprintf(lat_str, 32, "%.6f%c", abs_lat, lat_ref);
        p = strchr(lat_str, '.');
        if (p) *p = ',';
    }
    if (lon_str) {
        snprintf(lon_str, 32, "%.6f%c", abs_lon, lon_ref);
        p = strchr(lon_str, '.');
        if (p) *p = ',';
    }

    if (compass_deg) {
        *compass_deg = parse_clean_azimuth_from_excel(entry->azimuth_raw);
    }
}

/* Looks up geolocation data. Only runs when an XLSX file is loaded. */
gboolean stampy_lookup_geolocation_data(StampyWindow *self, int photo_id, GeolocationEntry *out_entry) {
    gpointer val = NULL;
    char id_str[64];

    /* Only proceed if an XLSX file was explicitly loaded. */
    if (!self || !self->file_geolocation || !self->geo_table || photo_id <= 0) {
        return FALSE;
    }

    /* Hash keys are strings, so convert the numeric id first. */
    snprintf(id_str, sizeof(id_str), "%d", photo_id);
    val = g_hash_table_lookup(self->geo_table, id_str);

    if (!val) {
        snprintf(id_str, sizeof(id_str), "%d.0", photo_id);
        val = g_hash_table_lookup(self->geo_table, id_str);
    }

    if (val) {
        if (out_entry) *out_entry = *(GeolocationEntry *)val;
        return TRUE;
    }

    return FALSE;
}

/* Looks up watermark text and azimuth for a photo id in the loaded DOCX. */
gboolean stampy_lookup_docx_data(StampyWindow *self, const char *photo_id, char **out_text, char **out_azimuth) {
    char *docx_path;
    char cmd[1024];
    FILE *fp;
    GString *xml_content;
    GString *plain_text;
    char buffer[1024];
    char *ptr;
    char **raw_lines;
    char **lines;
    gboolean found = FALSE;
    int i = 0, valid_count = 0, curr = 0;
    char *target_id = NULL;
    gboolean in_w_t = FALSE;

    if (!self || !self->file_watermark || !photo_id) return FALSE;

    target_id = extract_pure_id(photo_id);
    if (!target_id) return FALSE;

    docx_path = g_file_get_path(self->file_watermark);
    if (!docx_path) {
        g_free(target_id);
        return FALSE;
    }

    snprintf(cmd, sizeof(cmd), "unzip -p \"%s\" word/document.xml 2>/dev/null", docx_path);
    g_free(docx_path);

    fp = popen(cmd, "r");
    if (!fp) {
        g_free(target_id);
        return FALSE;
    }

    xml_content = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp)) {
        g_string_append(xml_content, buffer);
    }
    pclose(fp);

    if (xml_content->len == 0) {
        g_string_free(xml_content, TRUE);
        g_free(target_id);
        return FALSE;
    }

    /* Extract plain text: <w:t> runs are content, <w:br/> and </w:p> are line breaks. */
    plain_text = g_string_new("");
    ptr = xml_content->str;

    while (*ptr) {
        if (*ptr == '<') {
            if (strncmp(ptr, "<w:br/>", 7) == 0 || strncmp(ptr, "<w:br", 5) == 0) {
                g_string_append_c(plain_text, '\n');
            } else if (strncmp(ptr, "</w:p>", 6) == 0) {
                g_string_append_c(plain_text, '\n');
            } else if (strncmp(ptr, "<w:t>", 5) == 0 || strncmp(ptr, "<w:t ", 5) == 0) {
                in_w_t = TRUE;
            } else if (strncmp(ptr, "</w:t>", 6) == 0) {
                in_w_t = FALSE;
            }
            while (*ptr && *ptr != '>') ptr++;
            if (!*ptr) break;
        } else {
            if (in_w_t) {
                g_string_append_c(plain_text, *ptr);
            }
        }
        ptr++;
    }
    g_string_free(xml_content, TRUE);

    /* Drop empty lines. */
    raw_lines = g_strsplit(plain_text->str, "\n", -1);
    g_string_free(plain_text, TRUE);

    for (int k = 0; raw_lines[k] != NULL; k++) {
        g_strstrip(raw_lines[k]);
        if (strlen(raw_lines[k]) > 0) valid_count++;
    }

    lines = g_new0(char *, valid_count + 1);
    for (int k = 0; raw_lines[k] != NULL; k++) {
        if (strlen(raw_lines[k]) > 0) {
            lines[curr++] = g_strdup(raw_lines[k]);
        }
    }
    g_strfreev(raw_lines);

    /* Find the id, then collect following lines until the next id block. */
    while (lines[i] != NULL) {
        char *line_id = extract_pure_id(lines[i]);

        if (line_id && g_strcmp0(line_id, target_id) == 0) {
            /* Skip description lines that merely contain the id digits. */
            gboolean is_pure_id = TRUE;
            for (const char *p = lines[i]; *p; p++) {
                if (!g_ascii_isdigit(*p) && !g_ascii_isspace(*p)) {
                    is_pure_id = FALSE;
                    break;
                }
            }

            if (!is_pure_id) {
                g_free(line_id);
                i++;
                continue;
            }

            /* Collect following lines until the next photo id block. */
            GString *result_text = g_string_new("");
            int j = i + 1;
            int count = 0;

            /* Collect at most 6 data lines as a safeguard. */
            while (lines[j] != NULL && count < 6) {

                /* A digits-only line starts the next photo block. */
                gboolean has_digit = FALSE;
                gboolean only_digits_and_spaces = TRUE;
                for (const char *p = lines[j]; *p; p++) {
                    if (g_ascii_isdigit(*p)) {
                        has_digit = TRUE;
                    } else if (!g_ascii_isspace(*p)) {
                        only_digits_and_spaces = FALSE;
                        break;
                    }
                }

                if (has_digit && only_digits_and_spaces) {
                    break;
                }

                if (count > 0) {
                    g_string_append_c(result_text, '\n');
                }
                g_string_append(result_text, lines[j]);
                count++;
                j++;
            }

            if (out_text) {
                *out_text = g_string_free(result_text, FALSE);
            } else {
                g_string_free(result_text, TRUE);
            }

            /* Compass azimuth is always the second line below the id. */
            if (out_azimuth) {
                *out_azimuth = (count >= 2 && lines[i+2]) ? g_strdup(lines[i+2]) : g_strdup("");
            }

            found = TRUE;
            g_free(line_id);
            break;
        }
        g_free(line_id);
        i++;
    }

    g_strfreev(lines);
    g_free(target_id);
    return found;
}

/* Appends a line to the terminal log view. */
void stampy_append_log(StampyWindow *self, const char *message) {
    GtkTextBuffer *buffer;
    GtkTextIter end;
    GtkTextMark *mark;

    if (!self || !self->txt_terminal_output) return;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->txt_terminal_output));
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, message, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->txt_terminal_output), mark, 0.0, FALSE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(buffer, mark);
}

/* Fills the font dropdown with installed system fonts. */
void stampy_populate_system_fonts(StampyWindow *self) {
    PangoFontMap *font_map;
    PangoFontFamily **families;
    int n_families, i;
    GtkStringList *string_list;

    if (!self || !self->drop_text_font) return;

    font_map = pango_cairo_font_map_get_default();
    pango_font_map_list_families(font_map, &families, &n_families);

    string_list = gtk_string_list_new(NULL);
    for (i = 0; i < n_families; i++) {
        const char *font_name = pango_font_family_get_name(families[i]);
        gtk_string_list_append(string_list, font_name);
    }
    g_free(families);

    gtk_drop_down_set_model(GTK_DROP_DOWN(self->drop_text_font), G_LIST_MODEL(string_list));
}

/* Adds an image file, recursing into directories and skipping duplicates. */
void stampy_add_file_or_directory(StampyWindow *self, GFile *file, int *added_count) {
    GFileType type;

    if (!self || !file || !self->image_files) return;

    type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);

    if (type == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *enumerator = g_file_enumerate_children(file,
                                                                "standard::name,standard::type",
                                                                G_FILE_QUERY_INFO_NONE,
                                                                NULL, NULL);
        if (enumerator) {
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(enumerator, NULL, NULL)) != NULL) {
                const char *name = g_file_info_get_name(info);
                if (name && *name != '.') {
                    GFile *child = g_file_get_child(file, name);
                    stampy_add_file_or_directory(self, child, added_count);
                    g_object_unref(child);
                }
                g_object_unref(info);
            }
            g_file_enumerator_close(enumerator, NULL, NULL);
            g_object_unref(enumerator);
        }
    } else {
        char *basename = g_file_get_basename(file);
        if (basename) {
            char *lowercase = g_utf8_strdown(basename, -1);
            if (g_str_has_suffix(lowercase, ".jpg") ||
                g_str_has_suffix(lowercase, ".jpeg") ||
                g_str_has_suffix(lowercase, ".png")) {

                gboolean exists = FALSE;
                guint i;
                for (i = 0; i < self->image_files->len; i++) {
                    GFile *existing = g_ptr_array_index(self->image_files, i);
                    if (existing && g_file_equal(existing, file)) {
                        exists = TRUE;
                        break;
                    }
                }

                if (!exists) {
                    g_ptr_array_add(self->image_files, g_object_ref(file));
                    if (added_count) (*added_count)++;
                }
            }
            g_free(basename);
            g_free(lowercase);
        }
    }
}

/* Async image picker. */
static void on_images_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    StampyWindow *self = STAMPY_WINDOW(user_data);
    GError *error = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(dialog, res, &error);

    if (error) {
        g_clear_error(&error);
        return;
    }

    if (files && self) {
        guint n_items = g_list_model_get_n_items(files);
        int added = 0;

        for (guint i = 0; i < n_items; i++) {
            GFile *file = g_list_model_get_item(files, i);
            stampy_add_file_or_directory(self, file, &added);
            g_object_unref(file);
        }
        g_object_unref(files);

        if (added > 0) {
            stampy_populate_file_listbox(self);
            stampy_render_live_preview(self);
            stampy_show_toast(self, "Images imported successfully!");
            stampy_append_log(self, "[INFO] Images imported successfully.");
            stampy_update_clear_button_visibility(self);
        }
    }
}

/* Opens the image picker dialog. */
void stampy_open_image_picker(StampyWindow *self) {
    GtkFileDialog *dialog;
    GtkFileFilter *filter;
    GListStore *filters;

    if (!self) return;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Images");

    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images (JPG, PNG)");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/png");

    filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL, on_images_opened, self);
}

/* Handles the win.add-images action from shortcuts and context menu. */
void on_add_images_action_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    (void)action; (void)parameter;
    stampy_open_image_picker(self);
}

/* Open CSV file for GPS batch writing. Returns FILE* or NULL on error. */
FILE *stampy_geo_csv_open(const char *csv_path, char **error_msg) {
    FILE *fp;

    if (!csv_path) {
        if (error_msg) *error_msg = g_strdup("Invalid csv_path");
        return NULL;
    }

    fp = fopen(csv_path, "w");
    if (!fp) {
        if (error_msg) *error_msg = g_strdup_printf("Failed to create CSV: %s", csv_path);
        return NULL;
    }

    fprintf(fp, "SourceFile,GPSLatitude,GPSLongitude,GPSImgDirection,GPSLatitudeRef,GPSLongitudeRef\n");
    return fp;
}

/* Append one GPS entry to the CSV with the actual exported filename. */
void stampy_geo_csv_append_row(FILE *fp, const char *exported_path, const GeolocationEntry *entry) {
    char lat_ref, lon_ref;
    double abs_lat, abs_lon;

    if (!fp || !exported_path || !entry) return;

    abs_lat = fabs(entry->latitude);
    abs_lon = fabs(entry->longitude);
    lat_ref = (entry->latitude >= 0.0) ? 'N' : 'S';
    lon_ref = (entry->longitude >= 0.0) ? 'E' : 'W';

    fprintf(fp, "\"%s\",%.8f,%.8f,%.2f,%c,%c\n",
            exported_path, abs_lat, abs_lon,
            fmod(fabs(entry->azimuth_raw), 360.0),
            lat_ref, lon_ref);
}

/* Close the CSV file. */
void stampy_geo_csv_close(FILE *fp) {
    if (fp) fclose(fp);
}
