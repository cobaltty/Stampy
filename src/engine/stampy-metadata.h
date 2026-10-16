#ifndef STAMPY_METADATA_H
#define STAMPY_METADATA_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Geolocation / compass data for one photo. */
typedef struct {
    int photo_id;
    double lat;      /* Latitude (Y) */
    double lon;      /* Longitude (X) */
    double heading;  /* Azimuth / Direction */
} StampyGeoData;

/* Engine status. */
gboolean stampy_metadata_engine_is_ready(void);

/* Metadata writers. */
gboolean stampy_metadata_write_gps_and_heading(const char *source_image_path,
                                                const char *target_image_path,
                                                double lat,
                                                double lon,
                                                double heading,
                                                char **error_msg);

/* Batch GPS writer over a CSV file (exiftool -csv). */
gboolean stampy_apply_csv_gps_to_folder(const char *csv_path, const char *image_folder,
                                         char **error_msg);

G_END_DECLS

#endif /* STAMPY_METADATA_H */
