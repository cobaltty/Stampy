#include "stampy-window-private.h"

#define STAMPY_JPEG_QUALITY_DEFAULT 80
#define STAMPY_JPEG_QUALITY_MIN 1
#define STAMPY_JPEG_QUALITY_MAX 100

int
stampy_get_jpeg_quality (StampyWindow *self)
{
  (void) self;
  return stampy_config_get_int (STAMPY_CONFIG_KEY_JPEG_QUALITY,
                                STAMPY_JPEG_QUALITY_DEFAULT);
}

void
stampy_set_jpeg_quality (StampyWindow *self, int quality)
{
  (void) self;
  if (quality < STAMPY_JPEG_QUALITY_MIN)
    quality = STAMPY_JPEG_QUALITY_MIN;
  if (quality > STAMPY_JPEG_QUALITY_MAX)
    quality = STAMPY_JPEG_QUALITY_MAX;
  stampy_config_set_int (STAMPY_CONFIG_KEY_JPEG_QUALITY, quality);
}

/* Get/set adapters so rows can share one implementation. */
static int
get_jpeg_quality_value (void)
{
  return stampy_get_jpeg_quality (NULL);
}

static void
set_jpeg_quality_value (int value)
{
  stampy_set_jpeg_quality (NULL, value);
}

static int
get_thread_count_value (void)
{
  if (stampy_config_get_bool (STAMPY_CONFIG_KEY_EXPORT_USE_ALL_CORES, TRUE))
    {
      int n_cpus = g_get_num_processors ();
      return n_cpus < 1 ? 1 : n_cpus;
    }

  return stampy_config_get_export_threads ();
}

static void
set_thread_count_value (int value)
{
  stampy_config_set_export_threads (value);
}

/* Slider kept in two-way sync with a typeable spin button. */
typedef struct {
  GtkWidget *scale;
  GtkWidget *spin;
  int min_value;
  int max_value;
  int (*get_value) (void);
  void (*set_value) (int value);
} IntRowData;

static void on_int_spin_changed (GtkSpinButton *spin, gpointer user_data);

static void
int_row_apply (IntRowData *row, int value)
{
  if (row == NULL || row->set_value == NULL)
    return;

  /* Persisted via the config store; no explicit save needed. */
  row->set_value (value);
}

static void
on_int_scale_changed (GtkRange *range, gpointer user_data)
{
  IntRowData *row = (IntRowData *) user_data;
  int value;

  if (row == NULL || row->spin == NULL)
    return;

  value = (int) (gtk_range_get_value (range) + 0.5);

  /* Mirror into the spin button without re-triggering it. */
  if (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (row->spin)) != value)
    {
      g_signal_handlers_block_by_func (row->spin, on_int_spin_changed, row);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (row->spin), (double) value);
      g_signal_handlers_unblock_by_func (row->spin, on_int_spin_changed, row);
    }

  int_row_apply (row, value);
}

static void
on_int_spin_changed (GtkSpinButton *spin, gpointer user_data)
{
  IntRowData *row = (IntRowData *) user_data;
  int value;

  if (row == NULL || row->scale == NULL)
    return;

  value = gtk_spin_button_get_value_as_int (spin);

  /* Mirror into the slider without re-triggering it. */
  if ((int) (gtk_range_get_value (GTK_RANGE (row->scale)) + 0.5) != value)
    {
      g_signal_handlers_block_by_func (row->scale, on_int_scale_changed, row);
      gtk_range_set_value (GTK_RANGE (row->scale), (double) value);
      g_signal_handlers_unblock_by_func (row->scale, on_int_scale_changed, row);
    }

  int_row_apply (row, value);
}

/* Builds a row with a slider plus typeable spin in its suffix.
 * Returns the row (owned by @group). */
static GtkWidget *
add_int_slider_row (AdwPreferencesGroup *group,
                    const char *title,
                    const char *subtitle,
                    int subtitle_lines,
                    int min_value,
                    int max_value,
                    const char *tooltip,
                    const char *unit_label,
                    int (*get_value) (void),
                    void (*set_value) (int value))
{
  AdwActionRow *row;
  GtkWidget *suffix_box;
  GtkWidget *scale;
  GtkWidget *spin;
  IntRowData *data;
  char *tip;
  int initial;

  g_return_val_if_fail (group != NULL, NULL);
  g_return_val_if_fail (get_value != NULL && set_value != NULL, NULL);

  if (min_value > max_value)
    min_value = max_value;
  initial = get_value ();
  if (initial < min_value)
    initial = min_value;
  if (initial > max_value)
    initial = max_value;

  row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (row, subtitle);
  adw_action_row_set_subtitle_lines (row, subtitle_lines);
  adw_preferences_group_add (group, GTK_WIDGET (row));

  suffix_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand (suffix_box, TRUE);
  gtk_widget_set_halign (suffix_box, GTK_ALIGN_FILL);

  scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                    (double) min_value,
                                    (double) max_value,
                                    1.0);
  gtk_range_set_value (GTK_RANGE (scale), (double) initial);
  gtk_scale_set_draw_value (GTK_SCALE (scale), FALSE);
  gtk_scale_add_mark (GTK_SCALE (scale), (double) min_value, GTK_POS_BOTTOM, NULL);
  gtk_scale_add_mark (GTK_SCALE (scale), (double) (min_value + max_value) / 2.0, GTK_POS_BOTTOM, NULL);
  gtk_scale_add_mark (GTK_SCALE (scale), (double) max_value, GTK_POS_BOTTOM, NULL);
  gtk_widget_set_hexpand (scale, TRUE);
  gtk_widget_set_size_request (scale, 140, -1);
  tip = g_strdup_printf ("%s (%d\u2013%d)", tooltip, min_value, max_value);
  gtk_widget_set_tooltip_text (scale, tip);
  g_free (tip);
  gtk_box_append (GTK_BOX (suffix_box), scale);

  /* Typeable field, synced with the slider. */
  spin = gtk_spin_button_new_with_range ((double) min_value,
                                         (double) max_value,
                                         1.0);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
  gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (spin), TRUE);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), (double) initial);
  gtk_editable_set_width_chars (GTK_EDITABLE (spin), 3);
  tip = g_strdup_printf ("Type %s (%d\u2013%d)", tooltip, min_value, max_value);
  gtk_widget_set_tooltip_text (spin, tip);
  g_free (tip);
  gtk_widget_set_valign (spin, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (suffix_box), spin);

  if (unit_label != NULL && *unit_label != '\0')
    {
      GtkWidget *unit = gtk_label_new (unit_label);
      gtk_widget_add_css_class (unit, "dim-label");
      gtk_widget_set_valign (unit, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (suffix_box), unit);
    }

  adw_action_row_add_suffix (row, suffix_box);

  data = g_new0 (IntRowData, 1);
  data->scale = scale;
  data->spin = spin;
  data->min_value = min_value;
  data->max_value = max_value;
  data->get_value = get_value;
  data->set_value = set_value;

  g_signal_connect_data (scale, "value-changed",
                         G_CALLBACK (on_int_scale_changed),
                         data, (GClosureNotify) g_free, 0);
  g_signal_connect (spin, "value-changed",
                    G_CALLBACK (on_int_spin_changed),
                    data);

  return GTK_WIDGET (row);
}

static void
on_use_all_cores_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));

  (void) pspec;

  stampy_config_set_bool (STAMPY_CONFIG_KEY_EXPORT_USE_ALL_CORES, active);

  /* Dim the manual row while automatic mode is on. */
  if (user_data != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (user_data), !active);
}

static void
on_use_native_metadata_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  (void) pspec;

  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_USE_NATIVE_METADATA, active);
  if (user_data != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (user_data), !active);
}

static void
on_batch_exiftool_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  (void) pspec;
  (void) user_data;

  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_BATCH_EXIFTOOL, active);
}

static void
on_oversubscribe_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  (void) pspec;
  (void) user_data;

  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_EXPORT_OVERSUBSCRIBE, active);
}

static void
on_warn_landscape_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  StampyWindow *self = STAMPY_WINDOW (user_data);

  (void) pspec;
  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_WARN_LANDSCAPE, active);
  if (self != NULL)
    stampy_check_horizontal_photos (self);
}

static void
on_auto_folder_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  StampyWindow *self = STAMPY_WINDOW (user_data);

  (void) pspec;
  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER, active);
  if (self != NULL)
    stampy_prepare_default_export_folder (self);
}

void
stampy_show_preferences (StampyWindow *self)
{
  AdwDialog *dialog;
  AdwPreferencesPage *page;
  AdwPreferencesGroup *comp_group;
  AdwPreferencesGroup *perf_group;
  AdwPreferencesGroup *exp_group;
  AdwPreferencesGroup *extra_group;
  AdwSwitchRow *cores_switch;
  AdwSwitchRow *oversubscribe_switch;
  AdwSwitchRow *native_meta_switch;
  AdwSwitchRow *batch_exiftool_switch;
  AdwSwitchRow *title_exp_toggle;
  AdwSwitchRow *xlsx_time_toggle;
  AdwSwitchRow *landscape_switch;
  AdwSwitchRow *auto_folder_switch;
  GtkWidget *threads_row;
  gboolean use_all;
  gboolean use_native;
  int n_cpus;
  char *subtitle;

  if (!self)
    return;

  n_cpus = g_get_num_processors ();
  if (n_cpus < 1)
    n_cpus = 1;
  use_all = stampy_config_get_bool (STAMPY_CONFIG_KEY_EXPORT_USE_ALL_CORES, TRUE);
  use_native = stampy_config_get_bool (STAMPY_CONFIG_KEY_USE_NATIVE_METADATA, FALSE);

  dialog = ADW_DIALOG (adw_preferences_dialog_new ());
  adw_dialog_set_title (dialog, "Preferences");
  adw_dialog_set_content_width (dialog, 540);
  adw_dialog_set_content_height (dialog, 600);

  page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_set_title (page, "General");
  adw_preferences_page_set_icon_name (page, "preferences-system-symbolic");
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), page);

  comp_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (comp_group, "Image Compression");
  adw_preferences_group_set_description (comp_group,
      "Control the output file size versus quality for exported photos.");
  adw_preferences_page_add (page, comp_group);

  add_int_slider_row (comp_group,
                      "Compress image",
                      "JPEG quality for export. Drag the slider or type a value. "
                      "Lower values produce smaller files. "
                      "PNG exports are lossless and unaffected.",
                      3,
                      STAMPY_JPEG_QUALITY_MIN, STAMPY_JPEG_QUALITY_MAX,
                      "JPEG quality", "%",
                      get_jpeg_quality_value, set_jpeg_quality_value);

  perf_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (perf_group, "Performance");
  adw_preferences_group_set_description (perf_group,
      "Exports run in parallel, one worker thread per image.");
  adw_preferences_page_add (page, perf_group);

  cores_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cores_switch), "Use all CPU cores");
  subtitle = g_strdup_printf ("%d logical core%s detected. "
                              "Turn off to limit the worker count manually.",
                              n_cpus, n_cpus == 1 ? "" : "s");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (cores_switch), subtitle);
  g_free (subtitle);
  adw_switch_row_set_active (cores_switch, use_all);
  adw_preferences_group_add (perf_group, GTK_WIDGET (cores_switch));

  /* Worker threads row */
  threads_row = add_int_slider_row (perf_group,
                                    "Worker threads",
                                    "Base worker threads (one per CPU core). "
                                    "Oversubscription doubles this when enabled.",
                                    2,
                                    1, n_cpus,
                                    "worker threads", NULL,
                                    get_thread_count_value, set_thread_count_value);
  gtk_widget_set_sensitive (threads_row, !use_all);

  g_signal_connect (cores_switch, "notify::active",
                    G_CALLBACK (on_use_all_cores_changed),
                    threads_row);

  oversubscribe_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (oversubscribe_switch), "Thread oversubscription (2x cores)");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (oversubscribe_switch),
                               "Runs 2 worker threads per core to keep CPU busy during storage I/O wait.");
  adw_switch_row_set_active (oversubscribe_switch,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_EXPORT_OVERSUBSCRIBE, TRUE));
  g_object_bind_property (cores_switch, "active", oversubscribe_switch, "sensitive", G_BINDING_SYNC_CREATE);
  g_signal_connect (oversubscribe_switch, "notify::active",
                    G_CALLBACK (on_oversubscribe_changed), NULL);
  adw_preferences_group_add (perf_group, GTK_WIDGET (oversubscribe_switch));

  native_meta_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (native_meta_switch), "Native metadata engine (gexiv2)");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (native_meta_switch),
                               "In-memory EXIF/XMP processing. Much faster than spawning ExifTool.");
  adw_switch_row_set_active (native_meta_switch, use_native);
  adw_preferences_group_add (perf_group, GTK_WIDGET (native_meta_switch));

  batch_exiftool_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (batch_exiftool_switch), "Batch ExifTool execution");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (batch_exiftool_switch),
                               "Execute ExifTool once for the entire batch. Much faster for 1000+ photos.");
  adw_switch_row_set_active (batch_exiftool_switch,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_BATCH_EXIFTOOL, FALSE));
  gtk_widget_set_sensitive (GTK_WIDGET (batch_exiftool_switch), !use_native);
  g_signal_connect (batch_exiftool_switch, "notify::active",
                    G_CALLBACK (on_batch_exiftool_changed), NULL);
  adw_preferences_group_add (perf_group, GTK_WIDGET (batch_exiftool_switch));

  g_signal_connect (native_meta_switch, "notify::active",
                    G_CALLBACK (on_use_native_metadata_changed), batch_exiftool_switch);

  /* Title experimental toggle */
  title_exp_toggle = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (title_exp_toggle), "Watermark injection to metadata");
  adw_switch_row_set_active (title_exp_toggle,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, FALSE));
  g_signal_connect (title_exp_toggle, "notify::active",
                    G_CALLBACK (on_title_exp_toggle_changed), self);

  /* Image properties metadata injection toggle */
  exp_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (exp_group, "Image Properties");
  adw_preferences_page_add (page, exp_group);

  adw_preferences_group_add (exp_group, GTK_WIDGET (title_exp_toggle));
  gtk_widget_set_tooltip_text (GTK_WIDGET (title_exp_toggle),
      "When enabled, DOCX watermark text will be injected as image title/description metadata. "
      "This is an experimental feature.");

  /* Additional settings */
  extra_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (extra_group, "Additional Settings");
  adw_preferences_group_set_description (extra_group,
      "Optional import and export helpers.");
  adw_preferences_page_add (page, extra_group);

  /* Horizontal (landscape) photo warning banner */
  landscape_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (landscape_switch),
      "Warn about horizontal photos");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (landscape_switch),
      "Show a banner listing photos taken in landscape orientation.");
  adw_switch_row_set_active (landscape_switch,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_WARN_LANDSCAPE, TRUE));
  g_signal_connect (landscape_switch, "notify::active",
                    G_CALLBACK (on_warn_landscape_changed), self);
  adw_preferences_group_add (extra_group, GTK_WIDGET (landscape_switch));

  /* Automatic "<folder>_stamped" export destination */
  auto_folder_switch = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (auto_folder_switch),
      "Auto \"_stamped\" export folder");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (auto_folder_switch),
      "Creates a sibling folder named after the current image folder plus \"_stamped\" "
      "and exports into it automatically. Turn off to choose a destination folder on every export.");
  adw_switch_row_set_active (auto_folder_switch,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_AUTO_STAMPED_FOLDER, TRUE));
  g_signal_connect (auto_folder_switch, "notify::active",
                    G_CALLBACK (on_auto_folder_changed), self);
  adw_preferences_group_add (extra_group, GTK_WIDGET (auto_folder_switch));

  /* XLSX clock injection toggle */
  xlsx_time_toggle = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (xlsx_time_toggle), "XLSX clock injection to EXIF");
  adw_switch_row_set_active (xlsx_time_toggle,
      stampy_config_get_bool (STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, FALSE));
  g_signal_connect (xlsx_time_toggle, "notify::active",
                    G_CALLBACK (on_xlsx_time_exif_toggled), self);
  adw_preferences_group_add (exp_group, GTK_WIDGET (xlsx_time_toggle));
  gtk_widget_set_tooltip_text (GTK_WIDGET (xlsx_time_toggle),
      "When enabled, XLSX clock/jam column time will be injected as EXIF "
      "DateTimeOriginal and DateTimeDigitized. This is an experimental feature.");

  /* Programmatic dialogs need no close-handler unref (unlike the
   * builder-based shortcuts dialog); unref'ing here double-frees. */
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

void
on_title_exp_toggle_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  StampyWindow *self = STAMPY_WINDOW (user_data);

  (void) pspec;
  (void) user_data;

  active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_TITLE_EXPERIMENTAL, active);
  if (self != NULL)
    stampy_render_live_preview (self);
}

void
on_xlsx_time_exif_toggled (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  gboolean active;
  StampyWindow *self = STAMPY_WINDOW (user_data);

  (void) pspec;
  (void) user_data;

  if (ADW_IS_SWITCH_ROW (object))
    active = adw_switch_row_get_active (ADW_SWITCH_ROW (object));
  else
    active = gtk_switch_get_active (GTK_SWITCH (object));
  stampy_config_set_bool (STAMPY_CONFIG_KEY_XLSX_TIME_EXIF, active);

  if (self != NULL)
    stampy_render_live_preview (self);
}
