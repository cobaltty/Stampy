#ifndef STAMPY_METADATA_H
#define STAMPY_METADATA_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Struktur Data Geolocation / Compass */
typedef struct {
    int photo_id;
    double lat;      /* Latitude (Y) */
    double lon;      /* Longitude (X) */
    double heading;  /* Azimuth / Direction */
} StampyGeoData;

/* Engine Status & Capabilities */
gboolean stampy_metadata_engine_is_ready(void);

/* Generic Metadata Writers */
gboolean stampy_metadata_write_gps_and_heading(const char *source_image_path, /* <--- Parameter Baru */
                                                const char *target_image_path,
                                                double lat,
                                                double lon,
                                                double heading,
                                                char **error_msg);

/* Batch CSV GPS Writer (uses exiftool -csv for robust parsing) */
gboolean stampy_apply_csv_gps_to_folder(const char *csv_path, const char *image_folder,
                                         char **error_msg);

G_END_DECLS

#endif /* STAMPY_METADATA_H */
