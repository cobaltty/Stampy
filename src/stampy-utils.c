#include "stampy-window-private.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Smart ID extractor from filename ("11.jpg", "PU.245 (11).jpg", "PU.245_11.jpg" -> 11). */
int stampy_extract_id_from_filename(const char *filename) {
    char *basename;
    char *dot;
    const char *p;
    int id_val = -1;
    gboolean all_digits = TRUE;

    if (!filename || *filename == '\0') return -1;

    basename = g_path_get_basename(filename);
    if (!basename) return -1;

    dot = strrchr(basename, '.');
    if (dot) *dot = '\0';

    /* 1. Check if the stem is purely digits: "11" */
    for (p = basename; *p; p++) {
        if (!g_ascii_isdigit(*p)) {
            all_digits = FALSE;
            break;
        }
    }
    if (all_digits && *basename != '\0') {
        id_val = atoi(basename);
        g_free(basename);
        return id_val;
    }

    /* 2. Check for number inside parentheses: "PU.245 (11)" */
    {
        char *open_p = strrchr(basename, '(');
        if (open_p) {
            char *close_p = strchr(open_p, ')');
            if (close_p && close_p > open_p + 1) {
                int cand = atoi(open_p + 1);
                if (cand > 0) {
                    g_free(basename);
                    return cand;
                }
            }
        }
    }

    /* 3. Check for number at end after separator: "PU.245_11", "PU.245-11", "PU.245 11" */
    p = basename + strlen(basename) - 1;
    while (p >= basename && g_ascii_isdigit(*p)) p--;
    if (p >= basename && (*p == '_' || *p == '-' || *p == ' ') && g_ascii_isdigit(*(p + 1))) {
        id_val = atoi(p + 1);
        if (id_val > 0) {
            g_free(basename);
            return id_val;
        }
    }

    /* 4. Check for number at start before separator: "11_PU.245", "11-PU.245", "11 PU.245" */
    p = basename;
    while (*p && g_ascii_isdigit(*p)) p++;
    if (p > basename && (*p == '_' || *p == '-' || *p == ' ')) {
        id_val = atoi(basename);
        if (id_val > 0) {
            g_free(basename);
            return id_val;
        }
    }

    /* 5. Fallback: concatenate all digits */
    {
        GString *digits = g_string_new("");
        for (p = basename; *p; p++) {
            if (g_ascii_isdigit(*p)) {
                g_string_append_c(digits, *p);
            }
        }
        if (digits->len > 0) {
            id_val = atoi(digits->str);
        }
        g_string_free(digits, TRUE);
    }

    g_free(basename);
    return id_val;
}

/* Extracts the pure ID string ("PU.245 (11).jpg" -> "11"). */
static char *extract_pure_id(const char *input) {
    int id_val;

    if (!input) return NULL;

    id_val = stampy_extract_id_from_filename(input);
    if (id_val > 0) {
        return g_strdup_printf("%d", id_val);
    }

    return g_strdup(input);
}

/* Trims and case-folds a PU code for safe matching. */
static char *normalize_pu_code(const char *pu_code) {
    char *trimmed;
    char *folded;
    GString *out;
    gboolean pending_space = FALSE;
    const char *p;

    if (!pu_code) return g_strdup("");

    trimmed = g_strdup(pu_code);
    g_strstrip(trimmed);
    folded = g_utf8_casefold(trimmed, -1);
    g_free(trimmed);

    out = g_string_new("");
    for (p = folded; *p; p++) {
        if (g_ascii_isspace(*p)) {
            if (out->len > 0) pending_space = TRUE;
        } else {
            if (pending_space) g_string_append_c(out, ' ');
            g_string_append_c(out, *p);
            pending_space = FALSE;
        }
    }
    g_free(folded);

    return g_string_free(out, FALSE);
}

/* Builds the composite (PU, No) key used by the geolocation table. */
char *stampy_make_geo_key(const char *pu_code, int photo_id) {
    char *norm_pu = normalize_pu_code(pu_code);
    char *key;

    if (norm_pu && *norm_pu) {
        key = g_strdup_printf("%s|%d", norm_pu, photo_id);
    } else {
        key = g_strdup_printf("%d", photo_id);
    }

    g_free(norm_pu);
    return key;
}

static gboolean is_pu_folder_name(const char *name) {
    const char *p;
    if (!name) return FALSE;
    p = name;
    while (*p == ' ' || *p == '\t') p++;
    if (g_ascii_strncasecmp(p, "pu", 2) == 0) {
        char next = p[2];
        if (next == ' ' || next == '.' || next == '_' || next == '-' ||
            g_ascii_isdigit(next) || next == '\0') {
            return TRUE;
        }
    }
    return FALSE;
}

/* Reads the PU group name from the image's parent or ancestor folder.
 * Fast, allocation-free path scanning. */
char *stampy_get_pu_code_for_file(GFile *file) {
    const char *path;
    const char *last_slash;
    const char *prev_slash;

    if (!file) return NULL;

    path = g_file_peek_path(file);
    if (!path) {
        GFile *p = g_file_get_parent(file);
        char *pu = NULL;
        if (p) {
            char *b = g_file_get_basename(p);
            g_object_unref(p);
            if (b) {
                pu = g_strdup(b);
                g_strstrip(pu);
                g_free(b);
            }
        }
        return (pu && *pu != '\0') ? pu : NULL;
    }

    last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return NULL;

    /* Scan backwards to extract the immediate parent folder name */
    prev_slash = last_slash - 1;
    while (prev_slash > path && *prev_slash != '/') prev_slash--;
    if (*prev_slash == '/') prev_slash++;

    if (last_slash > prev_slash) {
        char *parent_name = g_strndup(prev_slash, last_slash - prev_slash);
        g_strstrip(parent_name);

        if (is_pu_folder_name(parent_name)) {
            return parent_name;
        }

        /* Check grandparent if immediate parent does not start with PU */
        if (prev_slash > path + 1) {
            const char *gp_end = prev_slash - 1;
            const char *gp_slash = gp_end - 1;
            while (gp_slash > path && *gp_slash != '/') gp_slash--;
            if (*gp_slash == '/') gp_slash++;
            if (gp_end > gp_slash) {
                char *gp_name = g_strndup(gp_slash, gp_end - gp_slash);
                g_strstrip(gp_name);
                if (is_pu_folder_name(gp_name)) {
                    g_free(parent_name);
                    return gp_name;
                }
                g_free(gp_name);
            }
        }

        if (*parent_name != '\0') return parent_name;
        g_free(parent_name);
    }

    return NULL;
}

/* Extracts the relative export subpath from the PU folder down to the file's directory.
 * E.g.:
 *   ".../PU III.15/1.jpg"                -> "PU III.15"
 *   ".../PU III.15/Camera A/1.jpg"       -> "PU III.15/Camera A"
 *   ".../PU III.15/Sub1/Sub2/1.jpg"      -> "PU III.15/Sub1/Sub2"
 */
char *stampy_get_pu_rel_subpath_for_file(GFile *file) {
    const char *path;
    char *dyn_path = NULL;
    const char *last_slash;
    const char *seg_end;
    const char *seg_start;
    const char *pu_start = NULL;
    char *rel_path = NULL;

    if (!file) return NULL;

    path = g_file_peek_path(file);
    if (!path) {
        dyn_path = g_file_get_path(file);
        path = dyn_path;
    }
    if (!path) return NULL;

    last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        g_free(dyn_path);
        return NULL;
    }

    /* Scan backwards segment by segment through directory components */
    seg_end = last_slash;
    while (seg_end > path) {
        seg_start = seg_end - 1;
        while (seg_start > path && *seg_start != '/') seg_start--;
        if (*seg_start == '/') seg_start++;

        if (seg_end > seg_start) {
            char *seg_name = g_strndup(seg_start, seg_end - seg_start);
            g_strstrip(seg_name);
            if (is_pu_folder_name(seg_name)) {
                pu_start = seg_start;
                g_free(seg_name);
                break;
            }
            g_free(seg_name);
        }

        if (seg_start <= path + 1) break;
        seg_end = seg_start - 1;
    }

    if (pu_start && last_slash > pu_start) {
        rel_path = g_strndup(pu_start, last_slash - pu_start);
    } else {
        rel_path = stampy_get_pu_code_for_file(file);
    }

    g_free(dyn_path);
    return rel_path;
}

/* Fast natural compare of two strings without heap allocations.
 * Correctly sorts: "PU III.3" < "PU III.12", "2.jpg" < "10.jpg". */
int stampy_natural_strcasecmp(const char *a, const char *b) {
    if (!a) return b ? -1 : 0;
    if (!b) return 1;

    while (*a && *b) {
        if (g_ascii_isdigit(*a) && g_ascii_isdigit(*b)) {
            guint64 na = 0, nb = 0;
            while (g_ascii_isdigit(*a)) {
                na = na * 10 + (*a - '0');
                a++;
            }
            while (g_ascii_isdigit(*b)) {
                nb = nb * 10 + (*b - '0');
                b++;
            }
            if (na != nb) return na < nb ? -1 : 1;
        } else {
            char ca = g_ascii_tolower(*a);
            char cb = g_ascii_tolower(*b);
            if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Natural sort for GPtrArray<GFile *>; keeps PU folders and their images in
 * human order ("PU III.3" before "PU III.12", "2.jpg" before "10.jpg"). */
int stampy_compare_files_naturally(gconstpointer a, gconstpointer b) {
    GFile *file_a = *(GFile * const *)a;
    GFile *file_b = *(GFile * const *)b;
    const char *path_a, *path_b;

    if (!file_a) return file_b ? -1 : 0;
    if (!file_b) return 1;

    path_a = g_file_peek_path(file_a);
    path_b = g_file_peek_path(file_b);

    return stampy_natural_strcasecmp(path_a ? path_a : "", path_b ? path_b : "");
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

/* Looks up geolocation data by composite (PU, No) key. Only runs when an XLSX
 * file is loaded. Falls back to the legacy No-only key for older sheets. */
gboolean stampy_lookup_geolocation_data(StampyWindow *self, const char *pu_code, int photo_id, GeolocationEntry *out_entry) {
    gpointer val = NULL;
    char *key;
    char id_str[64];

    /* Only proceed if an XLSX file was explicitly loaded. */
    if (!self || !self->file_geolocation || !self->geo_table || photo_id <= 0) {
        return FALSE;
    }

    key = stampy_make_geo_key(pu_code, photo_id);
    val = g_hash_table_lookup(self->geo_table, key);
    g_free(key);

    /* Backward compatibility: id-only keys from legacy spreadsheets. */
    if (!val) {
        snprintf(id_str, sizeof(id_str), "%d", photo_id);
        val = g_hash_table_lookup(self->geo_table, id_str);
    }

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

/* Invalidates the parsed in-memory DOCX plain text cache. */
void stampy_invalidate_docx_cache(StampyWindow *self) {
    if (!self) return;
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
}

static char **load_docx_lines(StampyWindow *self) {
    char *docx_path;
    char cmd[1024];
    FILE *fp;
    GString *xml_content;
    GString *plain_text;
    char buffer[1024];
    char *ptr;
    char **raw_lines;
    char **lines;
    int valid_count = 0, curr = 0;
    int k;
    gboolean in_w_t = FALSE;

    if (!self || !self->file_watermark) return NULL;

    docx_path = g_file_get_path(self->file_watermark);
    if (!docx_path) return NULL;

    if (self->docx_cached_lines && self->docx_cached_path &&
        g_strcmp0(self->docx_cached_path, docx_path) == 0) {
        g_free(docx_path);
        return self->docx_cached_lines;
    }

    stampy_invalidate_docx_cache(self);

    snprintf(cmd, sizeof(cmd), "unzip -p \"%s\" word/document.xml 2>/dev/null", docx_path);

    fp = popen(cmd, "r");
    if (!fp) {
        g_free(docx_path);
        return NULL;
    }

    xml_content = g_string_new("");
    while (fgets(buffer, sizeof(buffer), fp)) {
        g_string_append(xml_content, buffer);
    }
    pclose(fp);

    if (xml_content->len == 0) {
        g_string_free(xml_content, TRUE);
        g_free(docx_path);
        return NULL;
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

    raw_lines = g_strsplit(plain_text->str, "\n", -1);
    g_string_free(plain_text, TRUE);

    for (k = 0; raw_lines[k] != NULL; k++) {
        g_strstrip(raw_lines[k]);
        if (strlen(raw_lines[k]) > 0) valid_count++;
    }

    lines = g_new0(char *, valid_count + 1);
    for (k = 0; raw_lines[k] != NULL; k++) {
        if (strlen(raw_lines[k]) > 0) {
            lines[curr++] = g_strdup(raw_lines[k]);
        }
    }
    g_strfreev(raw_lines);

    self->docx_cached_path = docx_path;
    self->docx_cached_lines = lines;
    return self->docx_cached_lines;
}

static gboolean docx_line_is_pure_id(const char *line) {
    gboolean has_digit = FALSE;
    const char *p;

    if (!line) return FALSE;
    for (p = line; *p; p++) {
        if (g_ascii_isdigit(*p)) {
            has_digit = TRUE;
        } else if (!g_ascii_isspace(*p)) {
            return FALSE;
        }
    }
    return has_digit;
}

static gboolean docx_line_is_index_number(const char *line) {
    if (!line) return FALSE;
    return g_ascii_strncasecmp(line, "Index number:", 13) == 0 ||
           g_ascii_strncasecmp(line, "Index:", 6) == 0;
}

/* PU code is the line immediately above "Index number:" inside a block. */
static char *docx_block_pu_code(char **lines, int id_index) {
    int j;

    for (j = id_index + 1; lines[j] != NULL; j++) {
        if (docx_line_is_pure_id(lines[j])) break;
        if (lines[j + 1] && docx_line_is_index_number(lines[j + 1])) {
            return g_strdup(lines[j]);
        }
    }

    return NULL;
}

static void docx_collect_block(char **lines, int id_index, char **out_text, char **out_azimuth) {
    GString *result_text = g_string_new("");
    int j = id_index + 1;
    int count = 0;

    while (lines[j] != NULL && count < 32) {
        if (docx_line_is_pure_id(lines[j])) break;

        if (count > 0) g_string_append_c(result_text, '\n');
        g_string_append(result_text, lines[j]);
        count++;
        j++;
    }

    if (out_text) {
        *out_text = g_string_free(result_text, FALSE);
    } else {
        g_string_free(result_text, TRUE);
    }

    if (out_azimuth) {
        *out_azimuth = (count >= 2 && lines[id_index + 2]) ? g_strdup(lines[id_index + 2]) : g_strdup("");
    }
}

typedef struct {
    char *text;
    char *azimuth;
} DocxBlockEntry;

static void docx_block_entry_free(gpointer data) {
    DocxBlockEntry *e = (DocxBlockEntry *)data;
    if (e) {
        g_free(e->text);
        g_free(e->azimuth);
        g_free(e);
    }
}

/* Builds an in-memory hash index of the DOCX once, enabling O(1) lookups. */
static void ensure_docx_index(StampyWindow *self) {
    char **lines;
    int i;

    if (!self || self->docx_table) return;

    lines = load_docx_lines(self);
    if (!lines) return;

    self->docx_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, docx_block_entry_free);

    for (i = 0; lines[i] != NULL; i++) {
        char *line_id;
        int photo_id;
        char *block_pu;
        char *custom_text = NULL;
        char *azimuth_str = NULL;
        DocxBlockEntry *entry;
        char *key;
        char id_str[64];

        if (!docx_line_is_pure_id(lines[i])) continue;

        line_id = extract_pure_id(lines[i]);
        if (!line_id) continue;
        photo_id = atoi(line_id);
        g_free(line_id);

        if (photo_id <= 0) continue;

        block_pu = docx_block_pu_code(lines, i);
        docx_collect_block(lines, i, &custom_text, &azimuth_str);

        entry = g_new0(DocxBlockEntry, 1);
        entry->text = custom_text;
        entry->azimuth = azimuth_str;

        /* Insert primary composite key */
        key = stampy_make_geo_key(block_pu, photo_id);
        g_hash_table_insert(self->docx_table, key, entry);

        /* Insert legacy id-only fallback if not already present */
        snprintf(id_str, sizeof(id_str), "%d", photo_id);
        if (!g_hash_table_contains(self->docx_table, id_str)) {
            DocxBlockEntry *fallback_entry = g_new0(DocxBlockEntry, 1);
            fallback_entry->text = g_strdup(entry->text);
            fallback_entry->azimuth = g_strdup(entry->azimuth);
            g_hash_table_insert(self->docx_table, g_strdup(id_str), fallback_entry);
        }

        g_free(block_pu);
    }
}

/* Looks up watermark text and azimuth for a (PU, No) photo via O(1) hash table. */
gboolean stampy_lookup_docx_data(StampyWindow *self, const char *pu_code, const char *photo_id, char **out_text, char **out_azimuth) {
    DocxBlockEntry *entry = NULL;
    char *target_id_str = NULL;
    int photo_id_val;
    char *key;
    char id_str[64];

    if (!self || !self->file_watermark || !photo_id) return FALSE;

    ensure_docx_index(self);
    if (!self->docx_table) return FALSE;

    target_id_str = extract_pure_id(photo_id);
    if (!target_id_str) return FALSE;
    photo_id_val = atoi(target_id_str);
    g_free(target_id_str);

    if (photo_id_val <= 0) return FALSE;

    /* 1. Try composite (PU, No) key */
    key = stampy_make_geo_key(pu_code, photo_id_val);
    entry = (DocxBlockEntry *)g_hash_table_lookup(self->docx_table, key);
    g_free(key);

    /* 2. Fallback to No-only key for legacy documents */
    if (!entry) {
        snprintf(id_str, sizeof(id_str), "%d", photo_id_val);
        entry = (DocxBlockEntry *)g_hash_table_lookup(self->docx_table, id_str);
    }

    if (entry) {
        if (out_text) *out_text = g_strdup(entry->text);
        if (out_azimuth) *out_azimuth = g_strdup(entry->azimuth);
        return TRUE;
    }

    return FALSE;
}

/* Extracts project description text from full DOCX watermark text.
   Returns lines between the azimuth line and the "Index number:" line. */
gboolean stampy_extract_project_description(const char *full_text, char **out_text) {
    char **lines;
    char **raw_lines;
    GString *result;
    gboolean started = FALSE;
    gboolean found_azimuth = FALSE;
    int i;

    if (!full_text || !out_text) return FALSE;

    raw_lines = g_strsplit(full_text, "\n", -1);
    if (!raw_lines) return FALSE;

    lines = g_new0(char *, g_strv_length(raw_lines) + 1);
    for (i = 0; raw_lines[i] != NULL; i++) {
        if (strlen(raw_lines[i]) > 0) lines[i] = g_strdup(raw_lines[i]);
    }
    g_strfreev(raw_lines);

    result = g_string_new("");

    for (i = 0; lines[i] != NULL; i++) {
        /* Detect azimuth line: contains degree symbol (either ° or º) or compass direction */
        if (!started) {
            gboolean has_deg = (strstr(lines[i], "°") != NULL || strstr(lines[i], "º") != NULL);
            gboolean has_dir = (strchr(lines[i], 'N') != NULL || strchr(lines[i], 'S') != NULL ||
                                strchr(lines[i], 'E') != NULL || strchr(lines[i], 'W') != NULL);
            /* Skip coordinate lines which have commas and coordinates */
            gboolean is_coord_line = (strchr(lines[i], ',') != NULL && has_dir);

            if ((has_deg && has_dir) || (!is_coord_line && has_dir && i == 1)) {
                found_azimuth = TRUE;
                started = TRUE;
            }
            continue;
        }

        /* Stop at "Index number:" line or date line */
        if (g_ascii_strncasecmp(lines[i], "Index number:", 13) == 0 ||
            g_ascii_strncasecmp(lines[i], "Index:", 6) == 0) {
            break;
        }

        if (result->len > 0) g_string_append_c(result, ' ');
        g_string_append(result, lines[i]);
    }

    for (i = 0; lines[i] != NULL; i++) g_free(lines[i]);
    g_free(lines);

    if (found_azimuth && result->len > 0) {
        *out_text = g_string_free(result, FALSE);
        return TRUE;
    }

    g_string_free(result, TRUE);
    *out_text = NULL;
    return FALSE;
}

/* Extracts date "Kamis, 23 Juli 2026" → "2026:07:23" from DOCX watermark text. */
gboolean stampy_extract_docx_date(const char *full_text, char **out_date) {
    static const char *months[] = {
        "januari", "februari", "maret", "april", "mei", "juni",
        "juli", "agustus", "september", "oktober", "november", "desember"
    };
    char **raw_lines;
    char **lines;
    int i, j;
    int day = 0, year = 0, month = 0;
    const char *p;
    const char *yp;

    if (!full_text || !out_date) return FALSE;

    raw_lines = g_strsplit(full_text, "\n", -1);
    if (!raw_lines) return FALSE;

    lines = g_new0(char *, g_strv_length(raw_lines) + 1);
    for (i = 0; raw_lines[i] != NULL; i++) {
        if (strlen(raw_lines[i]) > 0) lines[i] = g_strdup(raw_lines[i]);
    }
    g_strfreev(raw_lines);

    for (i = 0; lines[i] != NULL; i++) {
        char *lower_line = g_utf8_strdown(lines[i], -1);
        int month_idx = -1;
        const char *m_pos = NULL;

        for (j = 0; j < 12; j++) {
            m_pos = strstr(lower_line, months[j]);
            if (m_pos) {
                month_idx = j;
                break;
            }
        }

        if (month_idx >= 0 && m_pos) {
            /* 1. Extract day: scan backwards from month position for digits */
            p = m_pos;
            while (p > lower_line && !g_ascii_isdigit(*(p - 1))) p--;
            if (p > lower_line && g_ascii_isdigit(*(p - 1))) {
                while (p > lower_line && g_ascii_isdigit(*(p - 1))) p--;
                day = atoi(p);
            }

            /* 2. Extract year: scan forwards after month for 4 consecutive digits */
            yp = m_pos + strlen(months[month_idx]);
            while (*yp) {
                if (g_ascii_isdigit(yp[0]) && g_ascii_isdigit(yp[1]) &&
                    g_ascii_isdigit(yp[2]) && g_ascii_isdigit(yp[3])) {
                    year = (yp[0]-'0')*1000 + (yp[1]-'0')*100 + (yp[2]-'0')*10 + (yp[3]-'0');
                    break;
                }
                yp++;
            }

            month = month_idx + 1;
            g_free(lower_line);
            if (day > 0 && day <= 31 && year >= 1970 && year <= 2100) {
                break;
            }
            day = 0;
            year = 0;
            month = 0;
            continue;
        }

        g_free(lower_line);
    }

    for (i = 0; lines[i] != NULL; i++) g_free(lines[i]);
    g_free(lines);

    if (day > 0 && month > 0 && year > 0) {
        *out_date = g_strdup_printf("%04d:%02d:%02d", year, month, day);
        return TRUE;
    }

    *out_date = NULL;
    return FALSE;
}

/* Appends a line to the terminal log view. */
void stampy_append_log(StampyWindow *self, const char *message) {
    GtkTextBuffer *buffer;
    GtkTextIter end;
    GtkTextMark *mark;

    if (!self || !self->txt_terminal_output) return;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->txt_terminal_output));

    /* Terminate an in-place status line before starting a normal log line. */
    if (self->has_active_status_line) {
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    }
    self->has_active_status_line = FALSE;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, message, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->txt_terminal_output), mark, 0.0, FALSE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(buffer, mark);
}

/* Updates the active progress/status line in-place in the terminal log view. */
void stampy_update_status_line(StampyWindow *self, const char *message) {
    GtkTextBuffer *buffer;
    GtkTextIter start, end;
    GtkTextMark *mark;

    if (!self || !self->txt_terminal_output || !message) return;

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->txt_terminal_output));

    /* Overwrite the previous status line (kept without a trailing newline). */
    if (self->has_active_status_line) {
        gtk_text_buffer_get_end_iter(buffer, &end);
        start = end;
        if (!gtk_text_iter_backward_line(&start)) {
            gtk_text_buffer_get_start_iter(buffer, &start);
        }
        gtk_text_buffer_delete(buffer, &start, &end);
    }

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, message, -1);
    self->has_active_status_line = TRUE;

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

static gboolean is_export_output_dir_name(const char *name) {
    size_t len;
    if (!name) return FALSE;
    len = strlen(name);
    if (len < 8) return FALSE;
    return (g_ascii_strcasecmp(name + len - 8, "_stamped") == 0);
}

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
                GFileType child_type = g_file_info_get_file_type(info);
                if (name && *name != '.' &&
                    !(child_type == G_FILE_TYPE_DIRECTORY &&
                      (is_export_output_dir_name(name) || is_compass_dir_name(name)))) {
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

/* Auto-loads a .docx watermark and .xlsx geolocation file found next to the
 * dropped images or inside dropped folders. Existing assets are kept. */
void stampy_auto_detect_companion_files(StampyWindow *self, GSList *files) {
    GHashTable *dirs;
    GHashTableIter iter;
    gpointer key, value;
    GSList *l;
    gboolean changed = FALSE;

    if (!self) return;

    dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (!files && self->image_files) {
        guint i;
        for (i = 0; i < self->image_files->len; i++) {
            char *path = g_file_get_path(g_ptr_array_index(self->image_files, i));
            if (path) {
                char *parent = g_path_get_dirname(path);
                if (parent) g_hash_table_add(dirs, parent);
                g_free(path);
            }
        }
    }

    for (l = files; l != NULL; l = l->next) {
        GFile *file = G_FILE(l->data);
        GFileType type;
        char *path;

        if (!file) continue;
        type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
        path = g_file_get_path(file);
        if (!path) continue;

        if (type == G_FILE_TYPE_DIRECTORY) {
            g_hash_table_add(dirs, path);
        } else {
            char *parent = g_path_get_dirname(path);
            if (parent) g_hash_table_add(dirs, parent);
            g_free(path);
        }
    }

    g_hash_table_iter_init(&iter, dirs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *dir_path = (const char *)key;
        GFile *dir = g_file_new_for_path(dir_path);
        GFileEnumerator *e;
        GFileInfo *info;

        (void)value;
        e = g_file_enumerate_children(dir, "standard::name,standard::type",
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (!e) {
            g_object_unref(dir);
            continue;
        }

        while ((info = g_file_enumerator_next_file(e, NULL, NULL)) != NULL) {
            const char *name = g_file_info_get_name(info);

            if (name) {
                char *lower = g_utf8_strdown(name, -1);
                GFileType child_type = g_file_info_get_file_type(info);

                if (child_type == G_FILE_TYPE_DIRECTORY &&
                    !self->file_compass &&
                    is_compass_dir_name(name)) {
                    self->file_compass = g_file_get_child(dir, name);
                    if (self->lbl_compass)
                        gtk_label_set_text(GTK_LABEL(self->lbl_compass), name);
                    stampy_append_log(self, "[INFO] Compass folder auto-detected.");
                    changed = TRUE;
                } else if (g_str_has_suffix(lower, ".docx") && !self->file_watermark) {
                    self->file_watermark = g_file_get_child(dir, name);
                    if (self->lbl_watermark)
                        gtk_label_set_text(GTK_LABEL(self->lbl_watermark), name);
                    stampy_append_log(self, "[INFO] Watermark DOCX auto-detected.");
                    changed = TRUE;
                } else if ((g_str_has_suffix(lower, ".xlsx") ||
                            g_str_has_suffix(lower, ".xls") ||
                            g_str_has_suffix(lower, ".csv")) && !self->file_geolocation) {
                    self->file_geolocation = g_file_get_child(dir, name);
                    if (self->lbl_geolocation)
                        gtk_label_set_text(GTK_LABEL(self->lbl_geolocation), name);
                    parse_excel_file(self, self->file_geolocation);
                    stampy_append_log(self, "[INFO] Geolocation file auto-detected.");
                    changed = TRUE;
                }
                g_free(lower);
            }
            g_object_unref(info);
        }

        g_file_enumerator_close(e, NULL, NULL);
        g_object_unref(e);
        g_object_unref(dir);
    }

    g_hash_table_unref(dirs);

    if (changed) {
        stampy_invalidate_docx_cache(self);
        stampy_show_toast(self, "Companion files auto-loaded");
        stampy_render_live_preview(self);
        stampy_update_clear_button_visibility(self);
        stampy_check_missing_data(self);
    }
}

/* Remembers the folder the user dropped/picked; falls back to the first image. */
void stampy_set_import_root(StampyWindow *self, GFile *file) {
    GFileType type;
    char *path;
    char *dir;

    if (!self || !file) return;

    type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
        g_set_object(&self->import_root, file);
        return;
    }

    path = g_file_get_path(file);
    if (!path) return;
    dir = g_path_get_dirname(path);
    if (dir) {
        GFile *root = g_file_new_for_path(dir);
        g_set_object(&self->import_root, root);
        g_object_unref(root);
    }
    g_free(dir);
    g_free(path);
}

/* Creates <parent>/<import folder>_stamped as a sibling and remembers it. */
void stampy_prepare_default_export_folder(StampyWindow *self) {
    GFile *owned_root = NULL;
    GFile *root;
    char *root_path;
    char *parent;
    char *folder_name;
    char *stamped_path;
    GFile *out;

    if (!self) return;

    /* Disabled: export falls back to the folder picker. */
    if (!stampy_config_get_bool(STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER, TRUE)) {
        g_clear_object(&self->default_export_folder);
        if (self->btn_header_export)
            gtk_widget_set_tooltip_text(self->btn_header_export, "Export Final Output");
        return;
    }

    if (!self->import_root && self->image_files && self->image_files->len > 0) {
        GFile *first = g_ptr_array_index(self->image_files, 0);
        char *first_path = first ? g_file_get_path(first) : NULL;

        if (first_path) {
            char *first_dir = g_path_get_dirname(first_path);
            if (first_dir) owned_root = g_file_new_for_path(first_dir);
            g_free(first_dir);
        }
        g_free(first_path);
    }

    root = self->import_root ? self->import_root : owned_root;
    if (!root) return;

    root_path = g_file_get_path(root);
    if (!root_path) {
        if (owned_root) g_object_unref(owned_root);
        return;
    }

    parent = g_path_get_dirname(root_path);
    folder_name = g_path_get_basename(root_path);
    stamped_path = g_strdup_printf("%s/%s_stamped", parent, folder_name);
    out = g_file_new_for_path(stamped_path);

    if (g_file_query_exists(out, NULL) ||
        g_file_make_directory_with_parents(out, NULL, NULL)) {
        char *log_msg = g_strdup_printf("[INFO] Export folder: %s", stamped_path);
        char *toast_msg = g_strdup_printf("Export folder: %s_stamped", folder_name);

        g_set_object(&self->default_export_folder, out);
        stampy_append_log(self, log_msg);
        stampy_show_toast(self, toast_msg);
        if (self->btn_header_export)
            gtk_widget_set_tooltip_text(self->btn_header_export, stamped_path);
        g_free(toast_msg);
        g_free(log_msg);
    } else {
        stampy_append_log(self, "[WARN] Could not create the _stamped export folder.");
    }

    g_object_unref(out);
    g_free(stamped_path);
    g_free(folder_name);
    g_free(parent);
    g_free(root_path);
    if (owned_root) g_object_unref(owned_root);
}

void stampy_on_landscape_banner_clicked(AdwBanner *banner, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    AdwToolbarView *toolbar;
    AdwHeaderBar *header;
    GtkWidget *title_widget;
    GtkWidget *content;
    GtkWidget *heading;
    GtkWidget *scrolled;
    GtkWidget *list;
    char *text;
    guint i;

    (void)banner;
    if (!self || !self->horizontal_photos || self->horizontal_photos->len == 0) return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Horizontal Photos");
    adw_dialog_set_content_width(dialog, 420);
    adw_dialog_set_content_height(dialog, 480);

    toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    header = ADW_HEADER_BAR(adw_header_bar_new());

    title_widget = gtk_label_new("Horizontal Photos");
    gtk_widget_add_css_class(title_widget, "title");
    adw_header_bar_set_title_widget(header, title_widget);

    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    text = g_strdup_printf("%u photo%s need%s rotating:",
                           self->horizontal_photos->len,
                           self->horizontal_photos->len == 1 ? "" : "s",
                           self->horizontal_photos->len == 1 ? "s" : "");
    heading = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_box_append(GTK_BOX(content), heading);
    g_free(text);

    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    for (i = 0; i < self->horizontal_photos->len; i++) {
        const char *name = g_ptr_array_index(self->horizontal_photos, i);
        GtkWidget *label = gtk_label_new(name);
        GtkWidget *row = gtk_list_box_row_new();

        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_widget_set_margin_top(label, 9);
        gtk_widget_set_margin_bottom(label, 9);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(content), scrolled);

    adw_toolbar_view_set_content(toolbar, content);
    adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

typedef struct {
    char *filename;
    int photo_id;
    gboolean missing_id;
    gboolean missing_gps;
    gboolean missing_coords;
    gboolean missing_degree;
    gboolean missing_text;
    gboolean missing_index;
    gboolean missing_date;
} MissingDataItem;

static void missing_data_item_free(gpointer data) {
    MissingDataItem *item = (MissingDataItem *)data;
    if (item) {
        g_free(item->filename);
        g_free(item);
    }
}

static gboolean stampy_is_file_in_pu_mode(StampyWindow *self, GFile *file) {
    GFile *parent;
    gboolean in_pu = FALSE;

    if (!file) return FALSE;
    parent = g_file_get_parent(file);
    if (parent) {
        char *pname = g_file_get_basename(parent);
        if (pname) {
            if (is_pu_folder_name(pname)) {
                in_pu = TRUE;
            }
            g_free(pname);
        }

        if (!in_pu && self->import_root) {
            if (!g_file_equal(parent, self->import_root) &&
                g_file_has_prefix(file, self->import_root)) {
                in_pu = TRUE;
            }
        }
        g_object_unref(parent);
    }
    if (!in_pu && self->import_root) {
        char *rname = g_file_get_basename(self->import_root);
        if (rname) {
            if (is_pu_folder_name(rname)) {
                in_pu = TRUE;
            }
            g_free(rname);
        }
    }
    return in_pu;
}

static gboolean stampy_detect_is_pu_mode(StampyWindow *self) {
    guint i;
    if (!self || !self->image_files || self->image_files->len == 0)
        return FALSE;

    if (self->pu_folders && self->pu_folders->len > 0) {
        for (i = 0; i < self->pu_folders->len; i++) {
            const char *pu = g_ptr_array_index(self->pu_folders, i);
            if (is_pu_folder_name(pu)) return TRUE;
        }
    }

    if (self->import_root) {
        char *rname = g_file_get_basename(self->import_root);
        if (rname) {
            gboolean is_pu = is_pu_folder_name(rname);
            g_free(rname);
            if (is_pu) return TRUE;
        }
    }

    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        if (stampy_is_file_in_pu_mode(self, file))
            return TRUE;
    }

    return FALSE;
}

static gboolean line_has_coord_pattern(const char *l) {
    gboolean has_ns = FALSE;
    gboolean has_ew = FALSE;
    const char *p;

    if (!l) return FALSE;
    for (p = l; *p; p++) {
        char c = *p;
        if (c == 'S' || c == 'N' || c == 's' || c == 'n') {
            const char *q = p - 1;
            while (q >= l && (*q == ' ' || *q == '\t')) q--;
            if (q >= l && (g_ascii_isdigit(*q) || *q == '.' || *q == ',' || *q == '\'' || *q == '"' || (guchar)*q >= 0x80)) {
                has_ns = TRUE;
            }
        } else if (c == 'E' || c == 'W' || c == 'e' || c == 'w') {
            const char *q = p - 1;
            while (q >= l && (*q == ' ' || *q == '\t')) q--;
            if (q >= l && (g_ascii_isdigit(*q) || *q == '.' || *q == ',' || *q == '\'' || *q == '"' || (guchar)*q >= 0x80)) {
                has_ew = TRUE;
            }
        }
    }
    return (has_ns && has_ew);
}

static gboolean watermark_has_coords(const char *text) {
    char **lines;
    gboolean has = FALSE;
    int j;

    if (!text || !*text) return FALSE;
    lines = g_strsplit(text, "\n", -1);
    if (!lines) return FALSE;

    for (j = 0; lines[j] != NULL; j++) {
        const char *l = lines[j];
        gboolean has_deg = (strstr(l, "°") != NULL || strstr(l, "º") != NULL);
        gboolean has_dir = (strchr(l, 'N') != NULL || strchr(l, 'S') != NULL ||
                            strchr(l, 'E') != NULL || strchr(l, 'W') != NULL ||
                            strchr(l, 'n') != NULL || strchr(l, 's') != NULL ||
                            strchr(l, 'e') != NULL || strchr(l, 'w') != NULL);
        gboolean has_comma = (strchr(l, ',') != NULL);

        if (line_has_coord_pattern(l)) {
            has = TRUE;
            break;
        }

        if ((has_comma && has_dir) || (has_deg && (has_comma || has_dir))) {
            has = TRUE;
            break;
        }

        {
            double lat = 0.0, lon = 0.0;
            if (sscanf(l, "%lf , %lf", &lat, &lon) == 2 ||
                sscanf(l, "%lf, %lf", &lat, &lon) == 2 ||
                sscanf(l, "%lf %lf", &lat, &lon) == 2) {
                if (fabs(lat) > 0.0001 && fabs(lon) > 0.0001) {
                    has = TRUE;
                    break;
                }
            }
        }
    }
    g_strfreev(lines);
    return has;
}

static gboolean watermark_has_date(const char *text);

static gboolean watermark_has_degree(const char *text, const char *azimuth_str) {
    char **lines;
    int j;

    if (azimuth_str && *azimuth_str != '\0') {
        const char *p = azimuth_str;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && strpbrk(p, "0123456789°º") != NULL) return TRUE;
    }
    if (!text || !*text) return FALSE;
    lines = g_strsplit(text, "\n", -1);
    if (!lines) return FALSE;

    for (j = 0; lines[j] != NULL; j++) {
        const char *l = lines[j];
        if (strstr(l, "°") != NULL || strstr(l, "º") != NULL ||
            strstr(l, "deg") != NULL || strstr(l, "arah") != NULL ||
            strstr(l, "azm") != NULL || strstr(l, "azimuth") != NULL) {
            if (strchr(l, ',') == NULL) {
                g_strfreev(lines);
                return TRUE;
            }
        }
    }
    g_strfreev(lines);
    return FALSE;
}

static gboolean watermark_has_text(const char *text) {
    char *desc = NULL;
    char **lines;
    gboolean found = FALSE;
    int j;

    if (!text || !*text) return FALSE;
    if (stampy_extract_project_description(text, &desc) && desc) {
        gboolean has = (strlen(g_strstrip(desc)) > 0);
        g_free(desc);
        if (has) return TRUE;
    }

    lines = g_strsplit(text, "\n", -1);
    if (!lines) return FALSE;
    for (j = 0; lines[j] != NULL; j++) {
        char *l = g_strstrip(g_strdup(lines[j]));
        if (strlen(l) > 3 &&
            g_ascii_strncasecmp(l, "index", 5) != 0 &&
            !watermark_has_coords(l) &&
            !watermark_has_date(l)) {
            for (const char *p = l; *p; p++) {
                if (g_ascii_isalpha(*p)) {
                    found = TRUE;
                    break;
                }
            }
        }
        g_free(l);
        if (found) break;
    }
    g_strfreev(lines);
    return found;
}

static gboolean watermark_has_index(const char *text) {
    char **lines;
    gboolean has = FALSE;
    int j;

    if (!text || !*text) return FALSE;
    lines = g_strsplit(text, "\n", -1);
    if (!lines) return FALSE;

    for (j = 0; lines[j] != NULL; j++) {
        char *l = g_strstrip(g_strdup(lines[j]));
        if (g_ascii_strncasecmp(l, "index number:", 13) == 0 ||
            g_ascii_strncasecmp(l, "index number", 12) == 0 ||
            g_ascii_strncasecmp(l, "index:", 6) == 0 ||
            g_ascii_strncasecmp(l, "index no:", 9) == 0 ||
            g_ascii_strncasecmp(l, "index no.", 9) == 0 ||
            g_ascii_strncasecmp(l, "index", 5) == 0) {
            const char *p = l;
            while (*p && !g_ascii_isdigit(*p)) p++;
            if (*p) {
                has = TRUE;
                g_free(l);
                break;
            }
        }
        g_free(l);
    }
    g_strfreev(lines);
    return has;
}

static gboolean watermark_has_date(const char *text) {
    char *out_d = NULL;
    char **lines;
    int j;

    if (!text || !*text) return FALSE;
    if (stampy_extract_docx_date(text, &out_d) && out_d) {
        g_free(out_d);
        return TRUE;
    }

    lines = g_strsplit(text, "\n", -1);
    if (!lines) return FALSE;

    for (j = 0; lines[j] != NULL; j++) {
        const char *l = lines[j];
        int d1 = 0, d2 = 0, d3 = 0;
        int h = 0, m = 0, s = 0;
        if (sscanf(l, "%d-%d-%d", &d1, &d2, &d3) == 3 ||
            sscanf(l, "%d/%d/%d", &d1, &d2, &d3) == 3 ||
            sscanf(l, "%d.%d.%d", &d1, &d2, &d3) == 3) {
            if ((d1 > 1900 && d1 < 2100) || (d3 > 1900 && d3 < 2100)) {
                g_strfreev(lines);
                return TRUE;
            }
        }
        if (sscanf(l, "%d:%d:%d", &h, &m, &s) >= 2) {
            if (h >= 0 && h < 24 && m >= 0 && m < 60) {
                g_strfreev(lines);
                return TRUE;
            }
        }
    }
    g_strfreev(lines);
    return FALSE;
}

void stampy_check_missing_data(StampyWindow *self) {
    guint total, i;
    gboolean is_pu_mode = FALSE;
    gboolean has_geo_expected;
    gboolean has_wm_expected;

    if (!self || !self->banner_missing_data) return;

    if (!self->image_files || self->image_files->len == 0) {
        adw_banner_set_revealed(ADW_BANNER(self->banner_missing_data), FALSE);
        if (self->missing_data_items) g_ptr_array_set_size(self->missing_data_items, 0);
        return;
    }

    has_geo_expected = (self->file_geolocation != NULL && self->geo_table != NULL);
    has_wm_expected = (self->file_watermark != NULL);

    if (!has_geo_expected && !has_wm_expected) {
        adw_banner_set_revealed(ADW_BANNER(self->banner_missing_data), FALSE);
        if (self->missing_data_items) g_ptr_array_set_size(self->missing_data_items, 0);
        return;
    }

    is_pu_mode = stampy_detect_is_pu_mode(self);
    total = self->image_files->len;

    if (self->missing_data_items) {
        g_ptr_array_set_size(self->missing_data_items, 0);
    } else {
        self->missing_data_items = g_ptr_array_new_with_free_func(missing_data_item_free);
    }

    for (i = 0; i < total; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        const char *path = g_file_peek_path(file);
        const char *name;
        char *pu_code;
        int pid;
        gboolean m_id;
        gboolean m_gps = FALSE;
        gboolean m_coords = FALSE;
        gboolean m_degree = FALSE;
        gboolean m_text = FALSE;
        gboolean m_index = FALSE;
        gboolean m_date = FALSE;
        char *custom_text = NULL;
        char *azimuth_str = NULL;
        GeolocationEntry geo_entry;
        gboolean has_geo = FALSE;
        gboolean file_is_pu;

        if (!path) continue;
        name = strrchr(path, '/');
        name = name ? name + 1 : path;
        pu_code = stampy_get_pu_code_for_file(file);
        pid = stampy_extract_id_from_filename(name);
        m_id = (pid <= 0);
        file_is_pu = is_pu_mode || stampy_is_file_in_pu_mode(self, file);

        if (has_geo_expected) {
            has_geo = (pid > 0 && stampy_lookup_geolocation_data(self, pu_code, pid, &geo_entry));
            if (!has_geo || m_id) {
                m_gps = TRUE;
            }
        }

        if (has_wm_expected && pid > 0) {
            stampy_lookup_docx_data(self, pu_code, name, &custom_text, &azimuth_str);
        }

        if (has_wm_expected) {
            gboolean has_coords = (custom_text && watermark_has_coords(custom_text)) ||
                (has_geo && (fabs(geo_entry.latitude) > 0.0001 || fabs(geo_entry.longitude) > 0.0001));
            gboolean has_degree = (custom_text && watermark_has_degree(custom_text, azimuth_str)) ||
                (has_geo && geo_entry.azimuth_raw > 0.0);
            gboolean has_text = (custom_text && watermark_has_text(custom_text));
            gboolean has_index = (custom_text && watermark_has_index(custom_text));
            gboolean has_date = (custom_text && watermark_has_date(custom_text)) ||
                (has_geo && (geo_entry.date_str[0] != '\0' || geo_entry.time_val > 0.0));

            m_coords = !has_coords;
            m_degree = !has_degree;
            m_text = !has_text;
            if (file_is_pu) {
                m_index = !has_index;
                m_date = !has_date;
            }
        }

        g_free(custom_text);
        g_free(azimuth_str);
        g_free(pu_code);

        if (m_gps || m_id || m_coords || m_degree || m_text || m_index || m_date) {
            MissingDataItem *item = g_new0(MissingDataItem, 1);
            item->filename = g_strdup(name);
            item->photo_id = pid;
            item->missing_id = m_id;
            item->missing_gps = m_gps;
            item->missing_coords = m_coords;
            item->missing_degree = m_degree;
            item->missing_text = m_text;
            item->missing_index = m_index;
            item->missing_date = m_date;
            g_ptr_array_add(self->missing_data_items, item);
        }
    }

    if (self->missing_data_items->len > 0) {
        guint count = self->missing_data_items->len;
        char *bmsg;

        if (is_pu_mode) {
            bmsg = (count == 1)
                ? g_strdup("Warning: 1 photo missing required PU data")
                : g_strdup_printf("Warning: %u photos missing required PU data", count);
        } else {
            bmsg = (count == 1)
                ? g_strdup("Warning: 1 photo missing required data")
                : g_strdup_printf("Warning: %u photos missing required data", count);
        }

        adw_banner_set_title(ADW_BANNER(self->banner_missing_data), bmsg);
        adw_banner_set_revealed(ADW_BANNER(self->banner_missing_data), TRUE);
        g_free(bmsg);
    } else {
        adw_banner_set_revealed(ADW_BANNER(self->banner_missing_data), FALSE);
    }
}

void stampy_on_missing_data_banner_clicked(AdwBanner *banner, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *dialog;
    AdwToolbarView *toolbar;
    AdwHeaderBar *header;
    GtkWidget *title_widget;
    GtkWidget *content;
    GtkWidget *heading;
    GtkWidget *scrolled;
    GtkWidget *list;
    char *text;
    guint i;

    (void)banner;
    if (!self || !self->missing_data_items || self->missing_data_items->len == 0) return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Expected Data Summary");
    adw_dialog_set_content_width(dialog, 520);
    adw_dialog_set_content_height(dialog, 540);

    toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    header = ADW_HEADER_BAR(adw_header_bar_new());

    title_widget = gtk_label_new("Expected Data Summary");
    gtk_widget_add_css_class(title_widget, "title");
    adw_header_bar_set_title_widget(header, title_widget);
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    text = g_strdup_printf("%u photo%s with incomplete data (%u total loaded):",
                           self->missing_data_items->len,
                           self->missing_data_items->len == 1 ? "" : "s",
                           self->image_files ? self->image_files->len : 0);
    heading = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_box_append(GTK_BOX(content), heading);
    g_free(text);

    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    for (i = 0; i < self->missing_data_items->len; i++) {
        MissingDataItem *item = g_ptr_array_index(self->missing_data_items, i);
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        GtkWidget *badge = NULL;

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->filename);
        /* ponytail: field subtitle breakdown omitted; re-add if users require per-row missing field tags */

        badge = gtk_label_new("Missing");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(badge, "missing-badge");
        gtk_widget_add_css_class(badge, "missing-badge-error");
        adw_action_row_add_suffix(row, badge);

        gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(row));
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(content), scrolled);

    adw_toolbar_view_set_content(toolbar, content);
    adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

void stampy_on_critical_coords_banner_clicked(AdwBanner *banner, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(user_data);
    AdwDialog *alert;
    const char *details;

    (void)banner;
    if (!self) return;

    details = self->critical_coords_details ? self->critical_coords_details :
              "Required coordinate columns (X, Y, or Azimuth) could not be detected from the spreadsheet headers.";

    alert = adw_alert_dialog_new("Critical: Missing Coordinates", details);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "OK");
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "ok");
    adw_dialog_present(alert, GTK_WIDGET(self));
}

/* Landscape scan runs off the main thread: adding 20k photos must not block
 * the UI while dimensions are read. ponytail: one file header read per image,
 * upgrade to a persisted index if this ever becomes hot. */
typedef struct {
    GPtrArray *files;
} LandscapeScanData;

/* Minimal header-only reader: orientation + dimensions without decoding. */

static guint16 header_u16(const guchar *p, gboolean little) {
    if (little) return (guint16)(p[0] | (p[1] << 8));
    return (guint16)((p[0] << 8) | p[1]);
}

static guint32 header_u32(const guchar *p, gboolean little) {
    if (little)
        return (guint32)p[0] | ((guint32)p[1] << 8) |
               ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
    return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | (guint32)p[3];
}

static gboolean exif_read_orientation(const guchar *tiff, gsize len, int *out) {
    gboolean little;
    guint32 ifd_off;
    guint16 count;
    guint16 i;

    if (len < 8) return FALSE;
    if (tiff[0] == 'I' && tiff[1] == 'I') little = TRUE;
    else if (tiff[0] == 'M' && tiff[1] == 'M') little = FALSE;
    else return FALSE;

    if (header_u16(tiff + 2, little) != 42) return FALSE;

    ifd_off = header_u32(tiff + 4, little);
    if (ifd_off > len - 2) return FALSE;

    count = header_u16(tiff + ifd_off, little);
    if (ifd_off + 2 + (gsize)count * 12 > len) return FALSE;

    for (i = 0; i < count; i++) {
        const guchar *entry = tiff + ifd_off + 2 + (gsize)i * 12;

        if (header_u16(entry, little) == 0x0112) {
            guint16 type = header_u16(entry + 2, little);
            guint32 n = header_u32(entry + 4, little);

            if (n >= 1 && type == 3) {
                *out = header_u16(entry + 8, little);
                return TRUE;
            }
            if (n >= 1 && type == 4) {
                *out = (int)header_u32(entry + 8, little);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean jpeg_is_sof(guint8 marker) {
    switch (marker) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Seeks to an absolute offset and reads a small slice. */
static gboolean read_at(int fd, gsize offset, guchar *buf, gsize n) {
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return FALSE;
    return read(fd, buf, n) == (ssize_t)n;
}

/* EXIF orientation lives in the first IFD; 2 KB is plenty for that probe. */
#define STAMPY_EXIF_PROBE 2048

/* Walks JPEG markers by seeking over segment lengths: only the APP1 head and
 * the SOF header are actually read, so no multi-KB buffer per photo. */
static gboolean jpeg_read_layout(int fd, int *w, int *h, int *orientation) {
    gsize off = 2;
    int guard = 0;

    *w = 0;
    *h = 0;
    *orientation = 1;

    while (guard++ < 512) {
        guchar marker_bytes[2];
        guint8 marker;
        guchar len_bytes[2];
        guint16 seg_len;

        if (!read_at(fd, off, marker_bytes, 2)) break;

        if (marker_bytes[0] != 0xFF) {
            off++;
            continue;
        }
        if (marker_bytes[1] == 0xFF) {
            off++;
            continue;
        }

        marker = marker_bytes[1];
        off += 2;

        /* Standalone markers carry no length field. */
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (marker == 0xDA) break; /* start of scan: no more metadata */

        if (!read_at(fd, off, len_bytes, 2)) break;
        seg_len = (guint16)((len_bytes[0] << 8) | len_bytes[1]);
        if (seg_len < 2) break;

        if (marker == 0xE1) {
            guchar probe[STAMPY_EXIF_PROBE];
            gsize want = MIN((gsize)seg_len - 2, (gsize)sizeof(probe));

            if (want >= 6 && read_at(fd, off + 2, probe, want) &&
                memcmp(probe, "Exif\0\0", 6) == 0) {
                int value;
                if (exif_read_orientation(probe + 6, want - 6, &value))
                    *orientation = value;
            }
        } else if (jpeg_is_sof(marker) && seg_len >= 7) {
            guchar sof[7];

            if (read_at(fd, off + 2, sof, sizeof(sof))) {
                *h = (sof[1] << 8) | sof[2];
                *w = (sof[3] << 8) | sof[4];
            }
        }

        if (*w > 0 && *h > 0 && *orientation != 1) break;
        off += seg_len;
    }

    return *w > 0 && *h > 0;
}

static gboolean png_read_layout(int fd, int *w, int *h) {
    static const guchar signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    guchar head[24];

    if (!read_at(fd, 0, head, sizeof(head))) return FALSE;
    if (memcmp(head, signature, 8) != 0) return FALSE;
    if (memcmp(head + 12, "IHDR", 4) != 0) return FALSE;

    *w = (int)header_u32(head + 16, FALSE);
    *h = (int)header_u32(head + 20, FALSE);
    return *w > 0 && *h > 0;
}

/* TRUE when the displayed image (after EXIF rotation) is wider than tall. */
static gboolean detect_landscape(int fd) {
    guchar head[2];
    int w = 0;
    int h = 0;
    int orientation = 1;
    gboolean ok;

    if (!read_at(fd, 0, head, sizeof(head))) return FALSE;

    if (head[0] == 0xFF && head[1] == 0xD8)
        ok = jpeg_read_layout(fd, &w, &h, &orientation);
    else
        ok = png_read_layout(fd, &w, &h);

    if (!ok) return FALSE;

    /* EXIF orientations 5-8 rotate the stored pixels by 90/270 degrees. */
    if (orientation >= 5 && orientation <= 8) {
        int tmp = w;
        w = h;
        h = tmp;
    }

    return w > h;
}

static void landscape_scan_data_free(gpointer data) {
    LandscapeScanData *scan = data;
    if (scan) {
        if (scan->files) g_ptr_array_unref(scan->files);
        g_free(scan);
    }
}

static void landscape_scan_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
    LandscapeScanData *scan = task_data;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    guint i;

    (void)source;

    for (i = 0; i < scan->files->len; i++) {
        GFile *file;
        char *path;

        if (g_cancellable_is_cancelled(cancellable)) break;

        file = g_ptr_array_index(scan->files, i);
        path = g_file_get_path(file);
        if (path) {
            int fd = g_open(path, O_RDONLY, 0);

            if (fd >= 0) {
                if (detect_landscape(fd)) {
                    char *name = g_file_get_basename(file);
                    if (name) g_ptr_array_add(names, name);
                }
                g_close(fd, NULL);
            }
        }
        g_free(path);
    }

    g_task_return_pointer(task, names, (GDestroyNotify)g_ptr_array_unref);
}

static void landscape_scan_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    StampyWindow *self = STAMPY_WINDOW(source);
    guint generation = GPOINTER_TO_UINT(user_data);
    GPtrArray *names;
    GError *error = NULL;
    char *title;

    names = g_task_propagate_pointer(G_TASK(res), &error);
    if (error) {
        g_clear_error(&error);
        return;
    }
    if (!names) return;

    if (!self || generation != self->landscape_generation) {
        g_ptr_array_unref(names);
        return;
    }

    if (self->horizontal_photos) g_ptr_array_unref(self->horizontal_photos);
    self->horizontal_photos = names;

    if (self->horizontal_photos->len > 0) {
        title = g_strdup_printf("Warning: %u photo%s in horizontal (landscape) position",
                                self->horizontal_photos->len,
                                self->horizontal_photos->len == 1 ? " is" : "s are");
        adw_banner_set_title(ADW_BANNER(self->banner_landscape), title);
        g_free(title);
        adw_banner_set_revealed(ADW_BANNER(self->banner_landscape), TRUE);
    } else {
        adw_banner_set_revealed(ADW_BANNER(self->banner_landscape), FALSE);
    }
}

/* Starts (or restarts) the background landscape scan. */
void stampy_check_horizontal_photos(StampyWindow *self) {
    LandscapeScanData *scan;
    GTask *task;
    GCancellable *cancellable;
    GPtrArray *files;
    guint i;

    if (!self || !self->banner_landscape) return;

    if (self->landscape_cancel) {
        g_cancellable_cancel(self->landscape_cancel);
        g_clear_object(&self->landscape_cancel);
    }
    self->landscape_generation++;

    if (!self->horizontal_photos) {
        self->horizontal_photos = g_ptr_array_new_with_free_func(g_free);
    } else {
        g_ptr_array_set_size(self->horizontal_photos, 0);
    }
    adw_banner_set_revealed(ADW_BANNER(self->banner_landscape), FALSE);

    if (!stampy_config_get_bool(STAMPY_CONFIG_KEY_WARN_LANDSCAPE, TRUE) ||
        !self->image_files || self->image_files->len == 0) {
        return;
    }

    files = g_ptr_array_new_with_free_func(g_object_unref);
    for (i = 0; i < self->image_files->len; i++) {
        GFile *file = g_ptr_array_index(self->image_files, i);
        if (file) g_ptr_array_add(files, g_object_ref(file));
    }

    scan = g_new0(LandscapeScanData, 1);
    scan->files = files;
    cancellable = g_cancellable_new();
    task = g_task_new(self, cancellable, landscape_scan_done,
                      GUINT_TO_POINTER(self->landscape_generation));
    g_task_set_task_data(task, scan, landscape_scan_data_free);
    g_task_run_in_thread(task, landscape_scan_thread);
    g_object_unref(task);
    self->landscape_cancel = cancellable;
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
        GSList *file_list = NULL;

        for (guint i = 0; i < n_items; i++) {
            GFile *file = g_list_model_get_item(files, i);
            if (file) file_list = g_slist_prepend(file_list, file);
        }
        g_object_unref(files);

        if (file_list) {
            file_list = g_slist_reverse(file_list);
            stampy_add_image_files(self, file_list);
            g_slist_free_full(file_list, g_object_unref);
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
