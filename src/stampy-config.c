/* stampy-config.c
 *
 * Persistent config store for Stampy preferences.
 *
 * Single source of truth for ALL preferences (current and future):
 *   ~/.config/stampy_prefs.json
 *
 * Contrast with stampy_last.json, which only keeps per-session UI state
 * (panel values, last selected preset). Anything the user expects to
 * survive an app restart AND that is not per-session belongs here.
 *
 * How to add a future preference:
 *   1. Pick a stable kebab-case key, document it in stampy-window-private.h
 *      next to the other STAMPY_CONFIG_KEY_* defines.
 *   2. Read it with stampy_config_get_int/bool/string (pass a fallback).
 *   3. Write it with stampy_config_set_int/bool/string (saves immediately).
 * No other plumbing needed. Main-thread use only.
 */

#include "stampy-window-private.h"

#include <string.h>
#include <json-glib/json-glib.h>

#define STAMPY_PREFS_FILENAME "stampy_prefs.json"
#define STAMPY_LAST_FILENAME "stampy_last.json"

/* In-memory cache, loaded lazily on first access. Main thread only. */
static JsonObject *prefs_obj = NULL;
static char *prefs_path = NULL;
static gboolean prefs_loaded = FALSE;

/* ------------------------------------------------------------------ */
/* Save                                                                */
/* ------------------------------------------------------------------ */

static void
prefs_save (void)
{
  JsonGenerator *gen;
  JsonNode *root;
  GError *error = NULL;

  if (!prefs_obj || !prefs_path)
    return;

  gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);

  root = json_node_new (JSON_NODE_OBJECT);
  json_node_set_object (root, prefs_obj);
  json_generator_set_root (gen, root);

  if (!json_generator_to_file (gen, prefs_path, &error))
    {
      g_warning ("[Config] Failed to save %s: %s",
                 prefs_path, error ? error->message : "unknown error");
      g_clear_error (&error);
    }

  json_node_unref (root);
  g_object_unref (gen);
}

/* ------------------------------------------------------------------ */
/* One-time migration from the legacy stampy_last.json session file    */
/* ------------------------------------------------------------------ */

static void
prefs_migrate_from_last_state (void)
{
  g_autofree char *last_path = NULL;
  g_autofree char *content = NULL;
  gsize len = 0;

  last_path = g_build_filename (g_get_user_config_dir (), STAMPY_LAST_FILENAME, NULL);
  if (!g_file_get_contents (last_path, &content, &len, NULL) || !content)
    return;

  /* jpeg_quality (was appended to stampy_last.json by older versions). */
  if (!json_object_has_member (prefs_obj, STAMPY_CONFIG_KEY_JPEG_QUALITY) &&
      strstr (content, "\"jpeg_quality\":") != NULL)
    {
      int q = 90;
      if (sscanf (strstr (content, "\"jpeg_quality\":"),
                  "\"jpeg_quality\": %d", &q) == 1)
        {
          if (q < 1) q = 1;
          if (q > 100) q = 100;
          json_object_set_int_member (prefs_obj, STAMPY_CONFIG_KEY_JPEG_QUALITY, q);
        }
    }

  /* compass_locked. */
  if (!json_object_has_member (prefs_obj, STAMPY_CONFIG_KEY_COMPASS_LOCKED))
    {
      json_object_set_boolean_member (prefs_obj, STAMPY_CONFIG_KEY_COMPASS_LOCKED,
                                      strstr (content, "\"compass_locked\": true") != NULL);
    }

  /* compass_folder. */
  if (!json_object_has_member (prefs_obj, STAMPY_CONFIG_KEY_COMPASS_FOLDER) &&
      strstr (content, "\"compass_folder\": \"") != NULL)
    {
      char folder[1024] = { 0 };
      if (sscanf (strstr (content, "\"compass_folder\": \""),
                  "\"compass_folder\": \"%1023[^\"]\"", folder) == 1 &&
          folder[0] != '\0')
        {
          json_object_set_string_member (prefs_obj, STAMPY_CONFIG_KEY_COMPASS_FOLDER, folder);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lazy load                                                           */
/* ------------------------------------------------------------------ */

static void
prefs_ensure_loaded (void)
{
  JsonParser *parser;
  GError *error = NULL;
  gboolean have_file;

  if (prefs_loaded)
    return;
  prefs_loaded = TRUE;

  prefs_obj = json_object_new ();
  prefs_path = g_build_filename (g_get_user_config_dir (), STAMPY_PREFS_FILENAME, NULL);

  have_file = g_file_test (prefs_path, G_FILE_TEST_EXISTS);

  if (have_file)
    {
      parser = json_parser_new ();
      if (json_parser_load_from_file (parser, prefs_path, &error))
        {
          JsonNode *root = json_parser_get_root (parser);
          if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            {
              JsonObject *loaded = json_node_get_object (root);
              if (loaded != NULL)
                {
                  /* NOTE: JsonObject is a BOXED type, not a GObject.
                   * It must be managed with json_object_ref/unref only;
                   * g_object_unref() on it segfaults inside libgobject
                   * (reads boxed memory as a GTypeClass pointer). */
                  json_object_unref (prefs_obj);
                  prefs_obj = json_object_ref (loaded);
                }
            }
        }
      else
        {
          g_warning ("[Config] Failed to load %s: %s (using defaults)",
                     prefs_path, error ? error->message : "unknown error");
          g_clear_error (&error);
        }
      g_object_unref (parser);
    }
  else
    {
      /* First run with the config store: carry over anything the old
       * session file already knew, then persist it. */
      prefs_migrate_from_last_state ();
      prefs_save ();
    }
}

/* ------------------------------------------------------------------ */
/* Public typed API                                                    */
/* ------------------------------------------------------------------ */

int
stampy_config_get_int (const char *key, int fallback)
{
  JsonNode *node;

  g_return_val_if_fail (key != NULL, fallback);
  prefs_ensure_loaded ();

  if (!json_object_has_member (prefs_obj, key))
    return fallback;

  node = json_object_get_member (prefs_obj, key);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return fallback;

  if (json_node_get_value_type (node) == G_TYPE_INT64)
    return (int) json_node_get_int (node);
  if (json_node_get_value_type (node) == G_TYPE_DOUBLE)
    return (int) json_node_get_double (node);

  return fallback;
}

void
stampy_config_set_int (const char *key, int value)
{
  g_return_if_fail (key != NULL);
  prefs_ensure_loaded ();

  json_object_set_int_member (prefs_obj, key, (gint64) value);
  prefs_save ();
}

gboolean
stampy_config_get_bool (const char *key, gboolean fallback)
{
  JsonNode *node;

  g_return_val_if_fail (key != NULL, fallback);
  prefs_ensure_loaded ();

  if (!json_object_has_member (prefs_obj, key))
    return fallback;

  node = json_object_get_member (prefs_obj, key);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return fallback;

  if (json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    return json_node_get_boolean (node);

  return fallback;
}

void
stampy_config_set_bool (const char *key, gboolean value)
{
  g_return_if_fail (key != NULL);
  prefs_ensure_loaded ();

  json_object_set_boolean_member (prefs_obj, key, value);
  prefs_save ();
}

char *
stampy_config_get_string (const char *key, const char *fallback)
{
  JsonNode *node;
  const char *val;

  g_return_val_if_fail (key != NULL, g_strdup (fallback ? fallback : ""));
  prefs_ensure_loaded ();

  if (!json_object_has_member (prefs_obj, key))
    goto out_fallback;

  node = json_object_get_member (prefs_obj, key);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    goto out_fallback;

  if (json_node_get_value_type (node) != G_TYPE_STRING)
    goto out_fallback;

  val = json_node_get_string (node);
  if (val == NULL)
    goto out_fallback;

  return g_strdup (val);

out_fallback:
  return g_strdup (fallback ? fallback : "");
}

void
stampy_config_set_string (const char *key, const char *value)
{
  g_return_if_fail (key != NULL);
  prefs_ensure_loaded ();

  json_object_set_string_member (prefs_obj, key, value ? value : "");
  prefs_save ();
}

/* ------------------------------------------------------------------ */
/* Export worker-thread count                                          */
/* ------------------------------------------------------------------ */

static int
system_cpu_count (void)
{
  int n = g_get_num_processors ();
  return n < 1 ? 1 : n;
}

int
stampy_config_get_export_threads (void)
{
  int n_cpus = system_cpu_count ();
  int stored;

  if (stampy_config_get_bool (STAMPY_CONFIG_KEY_EXPORT_USE_ALL_CORES, TRUE))
    return n_cpus;

  stored = stampy_config_get_int (STAMPY_CONFIG_KEY_EXPORT_THREAD_COUNT, 0);
  if (stored < 1)
    return 1;
  if (stored > n_cpus)
    return n_cpus;
  return stored;
}

void
stampy_config_set_export_threads (int value)
{
  int n_cpus = system_cpu_count ();

  if (value < 1)
    value = 1;
  if (value > n_cpus)
    value = n_cpus;

  stampy_config_set_int (STAMPY_CONFIG_KEY_EXPORT_THREAD_COUNT, value);
}
