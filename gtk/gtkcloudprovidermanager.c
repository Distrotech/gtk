/* gtkcloudprovidermanager.c
 *
 * Copyright (C) 2015 Carlos Soriano <csoriano@gnome.org>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkcloudprovidermanager.h"
#include "gtkcloudprovider.h"
#include <glib.h>
#include <glib/gprintf.h>

#define KEY_FILE_GROUP "Gtk Cloud Provider"

typedef struct
{
  GList *providers;
} GtkCloudProviderManagerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GtkCloudProviderManager, gtk_cloud_provider_manager, G_TYPE_OBJECT)

enum {
  PROP_0,
  LAST_PROP
};

enum
{
  CHANGED,
  LAST_SIGNAL
};

static GParamSpec *gParamSpecs [LAST_PROP];
static guint gSignals [LAST_SIGNAL];

/**
 * gtk_cloud_provider_manager_dup_singleton
 * Returns: (transfer none): A manager singleton
 */
GtkCloudProviderManager *
gtk_cloud_provider_manager_dup_singleton (void)
{
  static GObject *self = NULL;

  if (self == NULL)
    {
      self = g_object_new (GTK_TYPE_CLOUD_PROVIDER_MANAGER, NULL);
      return GTK_CLOUD_PROVIDER_MANAGER (self);
    }
  else
    {
      return g_object_ref (self);
    }
}

static void
gtk_cloud_provider_manager_finalize (GObject *object)
{
  GtkCloudProviderManager *self = (GtkCloudProviderManager *)object;

  G_OBJECT_CLASS (gtk_cloud_provider_manager_parent_class)->finalize (object);
}

static void
gtk_cloud_provider_manager_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  GtkCloudProviderManager *self = GTK_CLOUD_PROVIDER_MANAGER (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_cloud_provider_manager_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  GtkCloudProviderManager *self = GTK_CLOUD_PROVIDER_MANAGER (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_cloud_provider_manager_class_init (GtkCloudProviderManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gtk_cloud_provider_manager_finalize;
  object_class->get_property = gtk_cloud_provider_manager_get_property;
  object_class->set_property = gtk_cloud_provider_manager_set_property;

  gSignals [CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL,
                  NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  0);
}

static void
gtk_cloud_provider_manager_init (GtkCloudProviderManager *self)
{
}

/**
 * gtk_cloud_provider_manager_get_providers
 * @manager: A GtkCloudProviderManager
 * Returns: (transfer none): The list of providers.
 */
GList*
gtk_cloud_provider_manager_get_providers (GtkCloudProviderManager *manager)
{
  GtkCloudProviderManagerPrivate *priv = gtk_cloud_provider_manager_get_instance_private (manager);

  return priv->providers;
}

static void
load_cloud_provider (GtkCloudProviderManager *self,
                     GFile                   *file)
{
  GtkCloudProviderManagerPrivate *priv = gtk_cloud_provider_manager_get_instance_private (self);
  GKeyFile *key_file;
  gchar *path;
  GError *error = NULL;
  gchar *bus_name;
  gchar *object_path;
  gboolean success = FALSE;
  GtkCloudProvider *cloud_provider;

  g_print ("load cloud provider %s\n", g_file_get_path (file));
  key_file = g_key_file_new ();
  path = g_file_get_path (file);
  g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error);
  if (error != NULL)
    goto out;

  if (!g_key_file_has_group (key_file, KEY_FILE_GROUP))
    goto out;

  bus_name = g_key_file_get_string (key_file, KEY_FILE_GROUP, "BusName", &error);
  if (error != NULL)
    goto out;
  object_path = g_key_file_get_string (key_file, KEY_FILE_GROUP, "ObjectPath", &error);
  if (error != NULL)
    goto out;

  g_print ("cloud provider found %s %s\n", bus_name, object_path);
  cloud_provider = gtk_cloud_provider_new (bus_name, object_path);
  priv->providers = g_list_append (priv->providers, cloud_provider);

  success = TRUE;
out:
  if (!success)
    g_warning ("Error while loading cloud provider key file at %s", path);
  g_key_file_free (key_file);
}

/**
 * gtk_cloud_provider_manager_update
 * @manager: A GtkCloudProviderManager
 */
void
gtk_cloud_provider_manager_update (GtkCloudProviderManager *manager)
{
  GtkCloudProviderManagerPrivate *priv = gtk_cloud_provider_manager_get_instance_private (manager);
  const gchar* const *data_dirs;
  gint i;
  gint len;
  gchar *key_files_directory_path;
  GFile *key_files_directory_file;
  GError *error = NULL;
  GFileEnumerator *file_enumerator;


  g_list_free_full (priv->providers, g_object_unref);

  data_dirs = g_get_system_data_dirs ();
  len = g_strv_length ((gchar **)data_dirs);

  g_print ("updating manager with %d providers\n", len);
  for (i = 0; i < len; i++)
    {
      GFileInfo *info;

      key_files_directory_path = g_build_filename (data_dirs[i], "gtk+", "cloud-providers", NULL);
      key_files_directory_file = g_file_new_for_path (key_files_directory_path);
      file_enumerator = g_file_enumerate_children (key_files_directory_file,
                                                   "standard::name,standard::type",
                                                   G_FILE_QUERY_INFO_NONE,
                                                   NULL,
                                                   &error);
      if (error)
        {
          g_warning ("Error while updating manager %s error: %s\n", key_files_directory_path, error->message);
          error = NULL;
          continue;
        }

      info = g_file_enumerator_next_file (file_enumerator, NULL, &error);
      if (error)
        {
          g_warning ("Error while enumerating file %s error: %s\n", key_files_directory_path, error->message);
          error = NULL;
          continue;
        }
      if (info == NULL)
        {
          g_print ("no info\n");
        }
      while (info != NULL && error == NULL)
        {
           load_cloud_provider (manager, g_file_enumerator_get_child (file_enumerator, info));
           info = g_file_enumerator_next_file (file_enumerator, NULL, &error);
        }
    }
}
