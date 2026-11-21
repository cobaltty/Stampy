#include "stampy-application.h"
#include "stampy-window.h"
#include "stampy-quick-window.h"
#include "config.h"

struct _StampyApplication
{
  AdwApplication parent_instance;
};

G_DEFINE_TYPE (StampyApplication, stampy_application, ADW_TYPE_APPLICATION)

StampyApplication *
stampy_application_new (const char        *application_id,
                         GApplicationFlags  flags)
{
  g_return_val_if_fail (application_id != NULL, NULL);

  return g_object_new (STAMPY_TYPE_APPLICATION,
                       "application-id", application_id,
                       "flags", flags,
                       NULL);
}

/* About dialog. */
static void
stampy_application_show_about (GSimpleAction *action,
                                GVariant      *parameter,
                                gpointer       user_data)
{
  StampyApplication *self = STAMPY_APPLICATION (user_data);
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (self));

  const char *developers[] = {
    "Cobaltty",
    NULL
  };

  (void)action;
  (void)parameter;

  adw_show_about_dialog (GTK_WIDGET (window),
                         "application-name", "Stampy",
                         "application-icon", "com.cobaltty.stampy",
                         "developer-name", "cobaltty",
                          "version", PACKAGE_VERSION,
                         "developers", developers,
                         "copyright", "© 2026 cobaltty",
                         "website", "https://github.com/cobaltty/Stampy",
                         "issue-url", "https://github.com/cobaltty/Stampy/issues",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         NULL);
}

/* Shortcuts dialog. */
static gboolean
on_shortcuts_close_idle (gpointer user_data)
{
  g_object_unref (user_data);
  return G_SOURCE_REMOVE;
}

static void
on_shortcuts_closed (AdwDialog *dialog,
                     gpointer   user_data)
{
  (void)user_data;
  g_idle_add (on_shortcuts_close_idle, dialog);
}

static void
stampy_application_show_shortcuts (GSimpleAction *action,
                                   GVariant      *parameter,
                                   gpointer       user_data)
{
  StampyApplication *self = STAMPY_APPLICATION (user_data);
  GtkWindow *window;
  g_autoptr(GtkBuilder) builder = NULL;
  GtkWidget *dialog;

  (void)action;
  (void)parameter;

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window == NULL)
    return;

  builder = gtk_builder_new_from_resource ("/com/cobaltty/stampy/shortcuts-dialog.ui");
  dialog = GTK_WIDGET (gtk_builder_get_object (builder, "shortcuts_dialog"));
  g_object_ref_sink (dialog);

  g_signal_connect (dialog, "closed", G_CALLBACK (on_shortcuts_closed), NULL);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
}

static void
stampy_application_quit (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  (void)action;
  (void)parameter;
  g_application_quit (G_APPLICATION (user_data));
}

static void
stampy_application_startup (GApplication *app)
{
  GtkIconTheme *icon_theme;
  g_autoptr(GSimpleAction) about_action = NULL;
  g_autoptr(GSimpleAction) shortcuts_action = NULL;
  g_autoptr(GSimpleAction) quit_action = NULL;

  G_APPLICATION_CLASS (stampy_application_parent_class)->startup (app);

  /* Expose bundled icons to the icon theme. */
  icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  gtk_icon_theme_add_resource_path (icon_theme, "/com/cobaltty/stampy/icons");

  /* Application actions. */
  about_action = g_simple_action_new ("about", NULL);
  g_signal_connect (about_action, "activate", G_CALLBACK (stampy_application_show_about), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (about_action));

  shortcuts_action = g_simple_action_new ("shortcuts", NULL);
  g_signal_connect (shortcuts_action, "activate", G_CALLBACK (stampy_application_show_shortcuts), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (shortcuts_action));

  quit_action = g_simple_action_new ("quit", NULL);
  g_signal_connect (quit_action, "activate", G_CALLBACK (stampy_application_quit), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (quit_action));

  /* Keyboard shortcuts. */
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "app.quit",
      (const char *[]){ "<Control>q", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "window.close",
      (const char *[]){ "<Control>w", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "app.shortcuts",
      (const char *[]){ "<Control>question", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.add-images",
      (const char *[]){ "<Alt>i", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.watermark",
      (const char *[]){ "<Alt>w", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.geolocation",
      (const char *[]){ "<Alt>g", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.compass",
      (const char *[]){ "<Alt>c", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.compass-lock",
      (const char *[]){ "<Alt><Shift>c", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.export",
      (const char *[]){ "<Ctrl>e", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.cancel-export",
      (const char *[]){ "<Ctrl>c", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.remove-all",
      (const char *[]){ "<Ctrl>d", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.select-preset",
      (const char *[]){ "<Ctrl>s", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
      "win.preferences",
      (const char *[]){ "<Control>comma", NULL });
}

static GtkWindow *
find_main_window (GtkApplication *app)
{
  GList *windows = gtk_application_get_windows (app);
  GList *l;

  for (l = windows; l != NULL; l = l->next)
    {
      GtkWindow *w = GTK_WINDOW (l->data);
      if (STAMPY_IS_WINDOW (w) && !stampy_window_is_quick_mode (STAMPY_WINDOW (w)))
        return w;
    }
  return NULL;
}

static void
stampy_application_activate (GApplication *app)
{
  GtkWindow *window;

  g_assert (STAMPY_IS_APPLICATION (app));

  window = find_main_window (GTK_APPLICATION (app));
  if (window == NULL)
    window = GTK_WINDOW (stampy_window_new (GTK_APPLICATION (app)));

  gtk_window_present (window);
}

static void
stampy_application_open (GApplication  *app,
                         GFile        **files,
                         gint           n_files,
                         const gchar   *hint)
{
  GSList *file_list = NULL;
  StampyQuickWindow *quick_win;
  gint i;

  (void)hint;
  g_assert (STAMPY_IS_APPLICATION (app));

  for (i = 0; i < n_files; i++)
    {
      if (files[i] != NULL)
        file_list = g_slist_prepend (file_list, files[i]);
    }
  file_list = g_slist_reverse (file_list);

  if (file_list == NULL)
    {
      stampy_application_activate (app);
      return;
    }

  quick_win = stampy_quick_window_new (GTK_APPLICATION (app), file_list);
  g_slist_free (file_list);
  gtk_window_present (GTK_WINDOW (quick_win));
}

static void
stampy_application_class_init (StampyApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  app_class->startup = stampy_application_startup;
  app_class->activate = stampy_application_activate;
  app_class->open = stampy_application_open;
}

static void
stampy_application_init (StampyApplication *self)
{
}
