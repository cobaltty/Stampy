#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define STAMPY_TYPE_WINDOW (stampy_window_get_type())
G_DECLARE_FINAL_TYPE(StampyWindow, stampy_window, STAMPY, WINDOW, AdwApplicationWindow)

typedef struct {
    int no;
    double longitude; /* Column X */
    double latitude;  /* Column Y */
    double azimuth_raw;  /* Column Azh */
} GeolocationEntry;

struct _StampyWindow {
    AdwApplicationWindow parent_instance;

    /* Widgets */
    GtkWidget *toast_overlay;
    GtkWidget *left_stack;
    GtkWidget *drop_area_frame;
    GtkWidget *file_listbox;

    GtkWidget *btn_compass;
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

    GtkWidget *btn_menu;
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
};

StampyWindow *stampy_window_new(GtkApplication *app);
void stampy_show_toast(StampyWindow *self, const char *title);
void stampy_setup_actions_and_menus(StampyWindow *self);
void stampy_setup_file_handlers_and_dnd(StampyWindow *self);
void stampy_connect_export_logic(StampyWindow *self);
void stampy_connect_panel_signals(StampyWindow *self);
void stampy_populate_system_fonts(StampyWindow *self);
void stampy_populate_file_listbox(StampyWindow *self);
void stampy_add_file_or_directory(StampyWindow *self, GFile *file, int *added_count);
void stampy_append_log(StampyWindow *self, const char *message);
void stampy_render_live_preview(StampyWindow *self);

void on_add_images_action_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data);

/* Session state. */
void stampy_scan_and_populate_presets(StampyWindow *self);
void stampy_save_preset_to_file(StampyWindow *self, const char *filepath);
void stampy_load_preset_from_file(StampyWindow *self, const char *filepath);
void stampy_save_last_state(StampyWindow *self);
void stampy_load_last_state(StampyWindow *self);
void stampy_set_progress(StampyWindow *self, double fraction, gboolean visible);

/* DOCX/XLSX data lookups. */
gboolean stampy_lookup_docx_data(StampyWindow *self, const char *photo_id, char **out_text, char **out_azimuth);
gboolean stampy_lookup_geolocation_data(StampyWindow *self, int photo_id, GeolocationEntry *out_entry);
void format_geolocation_text(const GeolocationEntry *entry, char *lat_str, char *lon_str, double *compass_deg);

G_END_DECLS
