#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define STAMPY_TYPE_WINDOW (stampy_window_get_type())
G_DECLARE_FINAL_TYPE(StampyWindow, stampy_window, STAMPY, WINDOW, AdwApplicationWindow)

typedef struct {
    int no;
    char pu_code[64];  /* Group code from XLSX column B / DOCX block */
    double longitude;
    double latitude;
    double azimuth_raw;
    double time_val;  /* Excel time fraction from XLSX column F */
} GeolocationEntry;

struct _StampyWindow {
    AdwApplicationWindow parent_instance;

    /* Widgets */
    GtkWidget *toast_overlay;
    GtkWidget *left_stack;
    GtkWidget *drop_area_frame;
    GtkWidget *box_pu_selector;
    GtkWidget *drop_pu_folder;
    GtkWidget *file_listbox;

    GtkWidget *btn_compass;
    GtkWidget *btn_compass_lock;
    gboolean compass_locked;
    GtkWidget *btn_watermark;
    GtkWidget *btn_geolocation;
    GtkWidget *lbl_compass;
    GtkWidget *lbl_watermark;
    GtkWidget *lbl_geolocation;

    GtkWidget *box_right_panel;
    GtkWidget *drop_compass_pos;
    GtkWidget *val_compass_size;
    GtkWidget *val_compass_margin_x;
    GtkWidget *val_compass_margin_y;
    GtkWidget *val_compass_opacity;
    GtkWidget *btn_c_shadow;
    GtkWidget *btn_c_stroke;
    GtkWidget *btn_c_stroke_color;

    GtkWidget *drop_text_pos;
    GtkWidget *drop_text_font;
    GtkWidget *val_text_size;
    GtkWidget *val_text_spacing;
    GtkWidget *val_text_margin_x;
    GtkWidget *val_text_margin_y;
    GtkWidget *val_text_opacity;
    GtkWidget *btn_t_align_left;
    GtkWidget *btn_t_align_center;
    GtkWidget *btn_t_align_right;
    GtkWidget *btn_t_bold;
    GtkWidget *btn_t_italic;
    GtkWidget *btn_t_underline;
    GtkWidget *btn_t_shadow;
    GtkWidget *btn_t_stroke;
    GtkWidget *btn_t_stroke_color;
    GtkWidget *btn_t_color;

    /* Preset Widgets */
    GtkWidget *drop_presets;
    GtkWidget *btn_add_preset;
    GtkWidget *btn_remove_preset;
    GtkWidget *btn_edit_preset;
    GtkWidget *btn_apply_preset;
    GtkWidget *btn_import_preset;
    GtkWidget *btn_export_preset;

    GtkWidget *btn_metadata_inject;
    GtkWidget *btn_xlsx_time_exif;

    GtkWidget *btn_menu;
    GtkWidget *btn_header_clear;
    GtkWidget *btn_header_export;
    GtkWidget *preview_stack;
    GtkWidget *img_preview;
    GtkWidget *txt_terminal_output;
    GtkWidget *progress_bar;

    /* Data Storage */
    GPtrArray *image_files;
    GHashTable *geo_table;
    GFile *file_compass;
    GFile *file_watermark;
    GFile *file_geolocation;
    GtkWidget *btn_header_cancel;
    GtkWidget *img_decoration;
    gint export_cancel_flag;
    gboolean loading_state;

    /* Live preview cache & debounce */
    char *preview_cached_path;
    GdkPixbuf *preview_cached_pixbuf;
    guint save_state_timeout_id;

    /* QoL features */
    GtkWidget *banner_landscape;
    GPtrArray *horizontal_photos;
    GCancellable *landscape_cancel;
    guint landscape_generation;
    GtkWidget *banner_missing_data;
    GPtrArray *missing_data_items;
    GtkWidget *banner_critical_coords;
    char *critical_coords_details;
    char *docx_cached_path;
    char **docx_cached_lines;
    GHashTable *docx_table;
    GCancellable *scan_cancel;
    guint scan_pulse_id;
    guint scan_generation;
    guint populate_source_id;
    guint populate_generation;
    gboolean export_active;
    GFile *default_export_folder;
    GFile *import_root;
    GPtrArray *scan_cache_sources;
    GHashTable *scan_cache_dirs;
    GtkWidget *listbox_popover;
    gboolean has_active_status_line;
    GPtrArray *pu_folders;
    char *selected_pu_folder;
};

/* Window setup. */
StampyWindow *stampy_window_new(GtkApplication *app);
void stampy_show_toast(StampyWindow *self, const char *title);
void stampy_setup_actions_and_menus(StampyWindow *self);
void stampy_setup_file_handlers_and_dnd(StampyWindow *self);
void stampy_connect_export_logic(StampyWindow *self);
void stampy_connect_panel_signals(StampyWindow *self);
void stampy_render_live_preview(StampyWindow *self);
void on_add_images_action_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data);

/* Shortcut and menu actions. */
void stampy_open_compass_picker(StampyWindow *self);
void stampy_open_watermark_picker(StampyWindow *self);
void stampy_open_geolocation_picker(StampyWindow *self);
void stampy_start_export(StampyWindow *self);
void stampy_cancel_export(StampyWindow *self);
void stampy_toggle_compass_lock(StampyWindow *self);

/* List and panel helpers. */
void stampy_populate_system_fonts(StampyWindow *self);
void stampy_populate_pu_dropdown(StampyWindow *self);
void stampy_populate_file_listbox(StampyWindow *self);
void stampy_add_image_files(StampyWindow *self, GSList *files);
void stampy_add_file_or_directory(StampyWindow *self, GFile *file, int *added_count);
void stampy_append_log(StampyWindow *self, const char *message);
void stampy_update_status_line(StampyWindow *self, const char *message);
void stampy_check_horizontal_photos(StampyWindow *self);
void stampy_check_missing_data(StampyWindow *self);
void stampy_on_missing_data_banner_clicked(AdwBanner *banner, gpointer user_data);
void stampy_on_critical_coords_banner_clicked(AdwBanner *banner, gpointer user_data);
void stampy_invalidate_docx_cache(StampyWindow *self);
void stampy_auto_detect_companion_files(StampyWindow *self, GSList *files);
void stampy_prepare_default_export_folder(StampyWindow *self);
void stampy_set_import_root(StampyWindow *self, GFile *file);
void stampy_on_landscape_banner_clicked(AdwBanner *banner, gpointer user_data);
void parse_excel_file(StampyWindow *self, GFile *file);
void stampy_open_image_picker(StampyWindow *self);


/* Session state. */
void stampy_scan_and_populate_presets(StampyWindow *self);
void stampy_save_preset_to_file(StampyWindow *self, const char *filepath);
void stampy_load_preset_from_file(StampyWindow *self, const char *filepath);
void stampy_save_last_state(StampyWindow *self);
void stampy_schedule_save_last_state(StampyWindow *self);
void stampy_load_last_state(StampyWindow *self);
void stampy_set_progress(StampyWindow *self, double fraction, gboolean visible);

/* Header clear-button visibility. */
void stampy_update_clear_button_visibility(StampyWindow *self);

/* Preferences dialog. */
void stampy_show_preferences(StampyWindow *self);
int stampy_get_jpeg_quality(StampyWindow *self);
void stampy_set_jpeg_quality(StampyWindow *self, int quality);

/* Preferences toggle callbacks. */
void on_title_exp_toggle_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
void on_xlsx_time_exif_toggled(GObject *object, GParamSpec *pspec, gpointer user_data);

/* Persistent config store (~/.config/stampy_prefs.json). Single source of
 * truth for all preferences; values are written through on every set call.
 * Main-thread use only. */
#define STAMPY_CONFIG_KEY_JPEG_QUALITY   "jpeg-quality"   /* int 1..100, default 90 */
#define STAMPY_CONFIG_KEY_COMPASS_LOCKED "compass-locked"  /* bool, default FALSE */
#define STAMPY_CONFIG_KEY_COMPASS_FOLDER "compass-folder"  /* string path, default "" */
#define STAMPY_CONFIG_KEY_EXPORT_USE_ALL_CORES "export-use-all-cores" /* bool, default TRUE */
#define STAMPY_CONFIG_KEY_EXPORT_THREAD_COUNT  "export-thread-count"  /* int, 0 = auto (all cores) */
#define STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL "title-experimental" /* bool, default FALSE */
#define STAMPY_CONFIG_KEY_XLSX_TIME_EXIF "xlsx-time-exif" /* bool, default FALSE */
#define STAMPY_CONFIG_KEY_USE_NATIVE_METADATA "use-native-metadata" /* bool, default FALSE */
#define STAMPY_CONFIG_KEY_BATCH_EXIFTOOL "batch-exiftool" /* bool, default FALSE */
#define STAMPY_CONFIG_KEY_EXPORT_OVERSUBSCRIBE "export-oversubscribe" /* bool, default TRUE */
#define STAMPY_CONFIG_KEY_WARN_LANDSCAPE "warn-landscape" /* bool, default TRUE */
#define STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER "auto-stamped-folder" /* bool, default TRUE */
/* New preferences only need a key here plus the get/set helpers below. */
int stampy_config_get_int(const char *key, int fallback);
void stampy_config_set_int(const char *key, int value);
gboolean stampy_config_get_bool(const char *key, gboolean fallback);
void stampy_config_set_bool(const char *key, gboolean value);
char *stampy_config_get_string(const char *key, const char *fallback); /* transfer full */
void stampy_config_set_string(const char *key, const char *value);
/* Worker-thread count for export: all cores when automatic mode is on,
 * otherwise the stored count clamped to [1, n_cpus]. Always >= 1. */
int stampy_config_get_export_threads(void);
/* Stores a worker-thread count clamped to [1, n_cpus]. */
void stampy_config_set_export_threads(int value);

/* CSV helpers for batch GPS export via exiftool. */
FILE *stampy_geo_csv_open(const char *csv_path, char **error_msg);
void stampy_geo_csv_append_row(FILE *fp, const char *exported_path, const GeolocationEntry *entry);
void stampy_geo_csv_close(FILE *fp);

/* DOCX/XLSX data lookups. */
char *stampy_make_geo_key(const char *pu_code, int photo_id);
gboolean stampy_lookup_docx_data(StampyWindow *self, const char *pu_code, const char *photo_id, char **out_text, char **out_azimuth);
gboolean stampy_lookup_geolocation_data(StampyWindow *self, const char *pu_code, int photo_id, GeolocationEntry *out_entry);
void format_geolocation_text(const GeolocationEntry *entry, char *lat_str, char *lon_str, double *compass_deg);
char *stampy_get_pu_code_for_file(GFile *file);
char *stampy_get_pu_rel_subpath_for_file(GFile *file);
int stampy_natural_strcasecmp(const char *a, const char *b);
int stampy_compare_files_naturally(gconstpointer a, gconstpointer b);
/* Extracts project description text (between azimuth and "Index number:") from DOCX watermark text. */
gboolean stampy_extract_project_description(const char *full_text, char **out_text);
/* Extracts date "Kamis, 23 Juli 2026" → "2026:07:23" from DOCX watermark text. */
gboolean stampy_extract_docx_date(const char *full_text, char **out_date);
/* Smart ID extractor from filename ("11.jpg", "PU.245 (11).jpg", "PU.245_11.jpg" -> 11). */
int stampy_extract_id_from_filename(const char *filename);

G_END_DECLS
