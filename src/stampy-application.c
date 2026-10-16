#include "stampy-application.h"
#include "stampy-window.h"

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

/* --- ABOUT DIALOG CALLBACK --- */
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
                         "version", "1.4.001.beta",
                         "developers", developers,
                         "copyright", "© 2026 cobaltty",
                         "website", "https://github.com/cobaltty/stampy",
                         "issue-url", "https://github.com/cobaltty/stampy/issues",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         NULL);
}

/* --- SHORTCUTS DIALOG CALLBACK --- */
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
  GtkBuilder *builder;
  GtkWidget *dialog;

  (void)action;
  (void)parameter;

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window == NULL)
    return;

  builder = gtk_builder_new_from_resource ("/com/cobaltty/stampy/shortcuts-dialog.ui");
  dialog = GTK_WIDGET (gtk_builder_get_object (builder, "shortcuts_dialog"));
  g_object_ref_sink (dialog);
  g_object_unref (builder);

  g_signal_connect (dialog, "closed", G_CALLBACK (on_shortcuts_closed), NULL);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
}

static void
stampy_application_startup (GApplication *app)
{
  GtkIconTheme *icon_theme;
  g_autoptr(GSimpleAction) about_action = NULL;
  g_autoptr(GSimpleAction) shortcuts_action = NULL;

  G_APPLICATION_CLASS (stampy_application_parent_class)->startup (app);

  /* Register the custom GResource icons directory with GTK Icon Theme */
  icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  gtk_icon_theme_add_resource_path (icon_theme, "/com/cobaltty/stampy/icons");

  /* --- REGISTER ACTIONS --- */
  about_action = g_simple_action_new ("about", NULL);
  g_signal_connect (about_action, "activate", G_CALLBACK (stampy_application_show_about), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (about_action));

  shortcuts_action = g_simple_action_new ("shortcuts", NULL);
  g_signal_connect (shortcuts_action, "activate", G_CALLBACK (stampy_application_show_shortcuts), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (shortcuts_action));

  /* --- KEYBOARD SHORTCUTS --- */
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

static void
stampy_application_activate (GApplication *app)
{
  GtkWindow *window;

  g_assert (STAMPY_IS_APPLICATION (app));

  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL)
    window = g_object_new (STAMPY_TYPE_WINDOW,
                           "application", app,
                           NULL);

  gtk_window_present (window);
}

static void
stampy_application_class_init (StampyApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  app_class->startup = stampy_application_startup;
  app_class->activate = stampy_application_activate;
}

static void
stampy_application_init (StampyApplication *self)
{
}
