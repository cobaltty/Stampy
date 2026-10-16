#include "stampy-metadata.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Formats a double with a dot decimal separator regardless of locale,
   so ExifTool parses it correctly. */
static gchar *format_double_safe(double val, int decimals) {
    char buf[128];
    char *p;
    snprintf(buf, sizeof(buf), "%.*f", decimals, val);

    /* Replace any comma with a dot. */
    for (p = buf; *p; p++) {
        if (*p == ',') *p = '.';
    }
    return g_strdup(buf);
}

static char *get_exiftool_command(void) {
    if (g_file_test("/app/bin/perl", G_FILE_TEST_IS_EXECUTABLE) &&
        g_file_test("/app/bin/exiftool", G_FILE_TEST_EXISTS)) {
        return g_strdup("/app/bin/perl /app/bin/exiftool");
    }
    return g_strdup("exiftool");
}

gboolean stampy_metadata_engine_is_ready(void) {
    char *exif_cmd;
    gchar *cmd;
    gint exit_status = 0;
    gboolean ok;

    exif_cmd = get_exiftool_command();
    cmd = g_strdup_printf("%s -ver", exif_cmd);
    ok = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, NULL);

    g_free(cmd);
    g_free(exif_cmd);
    return (ok && exit_status == 0);
}

gboolean stampy_metadata_write_gps_and_heading(const char *source_image_path,
                                                const char *target_image_path,
                                                double lat,
                                                double lon,
                                                double heading,
                                                char **error_msg) {
    char *exif_cmd;
    double abs_lat;
    double abs_lon;
    char lat_ref;
    char lon_ref;
    gchar *quoted_target;
    gchar *tags_from = NULL;
    gchar *cmd = NULL;

    /* Decimal strings, always dot-separated. */
    gchar *lat_str = NULL;
    gchar *lon_str = NULL;

    gint exit_status = 0;
    GError *error = NULL;
    gboolean success;

    if (!target_image_path || *target_image_path == '\0') {
        if (error_msg) *error_msg = g_strdup("Invalid target image path");
        return FALSE;
    }

    exif_cmd = get_exiftool_command();
    abs_lat = fabs(lat);
    abs_lon = fabs(lon);
    lat_ref = (lat >= 0.0) ? 'N' : 'S';
    lon_ref = (lon >= 0.0) ? 'E' : 'W';
    quoted_target = g_shell_quote(target_image_path);

    lat_str = format_double_safe(abs_lat, 8);
    lon_str = format_double_safe(abs_lon, 8);

    /* Copy all EXIF from the source, then drop Orientation (pixels are
       already straightened) and old GPS tags (replaced below). */
    if (source_image_path && *source_image_path != '\0') {
        gchar *quoted_source = g_shell_quote(source_image_path);
        tags_from = g_strdup_printf("-TagsFromFile %s \"-all:all\" \"-Orientation=\" \"-gps:all=\" \"-XMP:GPS:all=\"", quoted_source);
        g_free(quoted_source);
    } else {
        tags_from = g_strdup("");
    }

    if (heading >= 0.0) {
        double clean_heading = fmod(heading, 360.0);
        gchar *head_str;

        if (clean_heading < 0.0) clean_heading += 360.0;

        /* Compass azimuth with 2 decimals. */
        head_str = format_double_safe(clean_heading, 2);

        cmd = g_strdup_printf(
            "%s -overwrite_original -quiet %s "
            "-GPSLatitude=%s -GPSLatitudeRef=%c "
            "-GPSLongitude=%s -GPSLongitudeRef=%c "
            "-GPSImgDirection=%s -GPSImgDirectionRef=T %s",
            exif_cmd, tags_from, lat_str, lat_ref, lon_str, lon_ref, head_str, quoted_target
        );
        g_free(head_str);
    } else {
        cmd = g_strdup_printf(
            "%s -overwrite_original -quiet %s "
            "-GPSLatitude=%s -GPSLatitudeRef=%c "
            "-GPSLongitude=%s -GPSLongitudeRef=%c %s",
            exif_cmd, tags_from, lat_str, lat_ref, lon_str, lon_ref, quoted_target
        );
    }

    g_free(lat_str);
    g_free(lon_str);
    g_free(quoted_target);
    g_free(tags_from);

    success = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, &error);

    if (!success || exit_status != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf("ExifTool exec failed (%s, code %d): %s",
                                        exif_cmd, exit_status,
                                        error ? error->message : "Unknown error");
        }
        if (error) g_clear_error(&error);
        success = FALSE;
    } else {
        success = TRUE;
    }

    g_free(cmd);
    g_free(exif_cmd);
    return success;
}

/* Applies GPS data from a CSV file to all images in a folder via exiftool -csv. */
gboolean stampy_apply_csv_gps_to_folder(const char *csv_path, const char *image_folder,
                                         char **error_msg) {
    char *exif_cmd;
    gchar *quoted_csv;
    gchar *quoted_folder;
    gchar *cmd = NULL;
    gint exit_status = 0;
    GError *error = NULL;
    gboolean success;

    if (!csv_path || !image_folder) {
        if (error_msg) *error_msg = g_strdup("Invalid csv_path or image_folder");
        return FALSE;
    }

    exif_cmd = get_exiftool_command();
    quoted_csv = g_shell_quote(csv_path);
    quoted_folder = g_shell_quote(image_folder);

    cmd = g_strdup_printf(
        "%s -overwrite_original -quiet -csv=%s %s",
        exif_cmd, quoted_csv, quoted_folder
    );

    g_free(quoted_csv);
    g_free(quoted_folder);

    success = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, &error);

    if (!success || exit_status != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf("ExifTool CSV batch failed (code %d): %s",
                                         exit_status,
                                         error ? error->message : "Unknown error");
        }
        if (error) g_clear_error(&error);
        success = FALSE;
    } else {
        success = TRUE;
    }

    g_free(cmd);
    g_free(exif_cmd);
    return success;
}
