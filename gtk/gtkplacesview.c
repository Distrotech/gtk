/* gtkplacesview.c
 *
 * Copyright (C) 2015 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "gtkintl.h"
#include "gtkplacesview.h"
#include "gtkplacesviewrow.h"
#include "gtktypebuiltins.h"

/**
 * SECTION:gtkplacesview
 * @Short_description: Widget that displays permanent drives and manages mounted networks
 * @Title: GtkPlacesView
 * @See_also: #GtkFileChooser
 *
 * #GtkPlacesView is a stock widget that displays a list permanent drives such
 * as harddisk partitions and networks.  #GtkPlacesView does not monitor
 * removable devices.
 *
 * The places view displays drives and networks, and will automatically mount
 * them when the user selects them. Network addresses are stored even if they
 * fail to connect. When the connection is successfull, the connected network
 * is shown at the network list.
 *
 * To make use of the places view, an application at least needs to connect
 * to the #GtkPlacesView::open-location signal.  This is emitted when the user
 * selects a location to open in the view.
 */

struct _GtkPlacesViewPrivate
{
  GVolumeMonitor                *volume_monitor;
  GtkPlacesOpenFlags             open_flags;

  GCancellable                  *connection_cancellable;

  GtkWidget                     *actionbar;
  GtkWidget                     *address_entry;
  GtkWidget                     *connect_button;
  GtkWidget                     *drives_listbox;
  GtkWidget                     *network_grid;
  GtkWidget                     *network_listbox;
  GtkWidget                     *recent_servers_listbox;

  guint local_only             : 1;
};

G_DEFINE_TYPE_WITH_PRIVATE (GtkPlacesView, gtk_places_view, GTK_TYPE_BOX)

/* GtkPlacesView properties & signals */
enum {
  PROP_0,
  PROP_LOCAL_ONLY,
  LAST_PROP
};

enum {
  OPEN_LOCATION,
  LAST_SIGNAL
};

const char *unsupported_protocols [] =
{
  "file", "afc", "obex", "http",
  "trash", "burn", "computer",
  "archive", "recent", "localtest",
  NULL
};

static guint places_view_signals [LAST_SIGNAL] = { 0 };
static GParamSpec *properties [LAST_PROP];

static void
emit_open_location (GtkPlacesView      *view,
                    GFile              *location,
                    GtkPlacesOpenFlags  open_flags)
{
  GtkPlacesViewPrivate *priv;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;

  if ((open_flags & priv->open_flags) == 0)
    open_flags = GTK_PLACES_OPEN_NORMAL;

  g_signal_emit (view, places_view_signals[OPEN_LOCATION], 0, location, open_flags);
}

static GBookmarkFile *
server_list_load (void)
{
	GBookmarkFile *bookmarks;
	GError *error = NULL;
	char *datadir;
	char *filename;

	bookmarks = g_bookmark_file_new ();
	datadir = g_build_filename (g_get_user_config_dir (), "gtk-3.0", NULL);
	filename = g_build_filename (datadir, "servers", NULL);

  g_mkdir_with_parents (datadir, 0700);
	g_bookmark_file_load_from_file (bookmarks, filename, &error);

	if (error != NULL)
    {
      if (! g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          /* only warn if the file exists */
          g_warning ("Unable to open server bookmarks: %s", error->message);
        }

      g_error_free (error);
      g_bookmark_file_free (bookmarks);
      bookmarks = NULL;
    }

  g_free (datadir);
  g_free (filename);

	return bookmarks;
}

static void
server_list_save (GBookmarkFile *bookmarks)
{
	char *filename;

	filename = g_build_filename (g_get_user_config_dir (), "gtk-3.0", "servers", NULL);
	g_bookmark_file_to_file (bookmarks, filename, NULL);
	g_free (filename);
}

static void
server_list_add_server (GFile *file)
{
  GBookmarkFile *bookmarks;
  GFileInfo *info;
  GError *error;
  char *title;
  char *uri;

  g_return_if_fail (G_IS_FILE (file));

  error = NULL;
  bookmarks = server_list_load ();

  if (!bookmarks)
    return;

  uri = g_file_get_uri (file);

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &error);
  title = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);

  g_bookmark_file_set_title (bookmarks, uri, title);
	g_bookmark_file_set_visited (bookmarks, uri, -1);
	g_bookmark_file_add_application (bookmarks, uri, NULL, NULL);

  server_list_save (bookmarks);

  g_bookmark_file_free (bookmarks);
  g_clear_object (&info);
  g_free (title);
  g_free (uri);
}

static gboolean
gtk_places_view_real_get_local_only (GtkPlacesView *view)
{
  return view->priv->local_only;
}

static void
gtk_places_view_real_set_local_only (GtkPlacesView *view,
                                     gboolean       local_only)
{
  if (view->priv->local_only != local_only)
    {
      view->priv->local_only = local_only;

      gtk_widget_set_visible (view->priv->actionbar, !local_only);
      gtk_widget_set_visible (view->priv->network_grid, !local_only);

      g_object_notify_by_pspec (G_OBJECT (view), properties [PROP_LOCAL_ONLY]);
    }
}

static void
gtk_places_view_finalize (GObject *object)
{
  GtkPlacesView *self = (GtkPlacesView *)object;
  GtkPlacesViewPrivate *priv = gtk_places_view_get_instance_private (self);

  g_clear_object (&priv->volume_monitor);

  G_OBJECT_CLASS (gtk_places_view_parent_class)->finalize (object);
}

static void
gtk_places_view_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GtkPlacesView *self = GTK_PLACES_VIEW (object);

  switch (prop_id)
    {
    case PROP_LOCAL_ONLY:
      g_value_set_boolean (value, gtk_places_view_get_local_only (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_places_view_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  GtkPlacesView *self = GTK_PLACES_VIEW (object);

  switch (prop_id)
    {
    case PROP_LOCAL_ONLY:
      gtk_places_view_set_local_only (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
is_external_device (GVolume *volume)
{
  gboolean external;
  GDrive *drive;
  GMount *mount;

  external = FALSE;
  drive = g_volume_get_drive (volume);
  mount = g_volume_get_mount (volume);

  if (drive)
    {
      external = g_drive_can_eject (drive);

      if (volume)
        external |= g_volume_can_eject (volume);

      if (mount)
        external |= g_mount_can_eject (mount) && !g_mount_can_unmount (mount);
    }
  else
    {
      /*
       * If no GDrive is associated with the given volume, it is assured
       * this is not an external device (e.g. USB sticks or external hard
       * drives).
       */
      external = FALSE;
    }

  g_clear_object (&drive);
  g_clear_object (&mount);

  return external;
}

static void
populate_servers (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  GBookmarkFile *server_list;
  gchar **uris;
  gsize num_uris;
  gint i;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;
  server_list = server_list_load ();

  if (!server_list)
    return;

  uris = g_bookmark_file_get_uris (server_list, &num_uris);

  if (!uris)
    {
      g_bookmark_file_free (server_list);
      return;
    }

  for (i = 0; i < num_uris; i++)
    {
      GtkWidget *row;
      GtkWidget *label;
      char *name;

      name = g_bookmark_file_get_title (server_list, uris[i], NULL);

      row = gtk_list_box_row_new ();

      label = gtk_label_new (name);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);

      gtk_container_add (GTK_CONTAINER (row), label);
      gtk_container_add (GTK_CONTAINER (priv->recent_servers_listbox), row);

      gtk_widget_show_all (row);

			g_free (name);
    }

  g_strfreev (uris);
  g_bookmark_file_free (server_list);
}

static void
add_volume (GtkPlacesView *view,
            GVolume       *volume)
{
  GtkPlacesViewPrivate *priv;
  gboolean is_network;
  GDrive *drive;
  GMount *mount;
  GFile *root;
  GIcon *icon;
  gchar *identifier;
  gchar *name;
  gchar *path;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));
  g_return_if_fail (G_IS_VOLUME (volume));

  priv = view->priv;

  if (is_external_device (volume))
    return;

  drive = g_volume_get_drive (volume);

  if (drive)
    {
      gboolean is_removable;

      is_removable = g_drive_is_media_removable (drive);
      g_object_unref (drive);

      if (is_removable)
        return;
    }

  identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);
  is_network = g_strcmp0 (identifier, "network") == 0;

  mount = g_volume_get_mount (volume);
  root = mount ? g_mount_get_root (mount) : NULL;
  icon = g_volume_get_icon (volume);
  name = g_volume_get_name (volume);

  if (root)
    path = is_network ? g_file_get_uri (root) : g_file_get_path (root);
  else
    path = NULL;

  if (!mount ||
      (mount && !g_mount_is_shadowed (mount)))
    {
      GtkWidget *row;

      row = g_object_new (GTK_TYPE_PLACES_VIEW_ROW,
                      "icon", icon,
                      "name", name,
                      "path", path ? path : "",
                      "volume", volume,
                      "mount", mount,
                      NULL);

      if (is_network)
        gtk_container_add (GTK_CONTAINER (priv->network_listbox), row);
      else
        gtk_container_add (GTK_CONTAINER (priv->drives_listbox), row);
    }

  g_clear_object (&root);
  g_clear_object (&icon);
  g_clear_object (&mount);
  g_free (identifier);
  g_free (name);
  g_free (path);
}

static void
add_mount (GtkPlacesView *view,
           GMount        *mount)
{
  GtkPlacesViewPrivate *priv;
  gboolean is_network;
  GVolume *volume;
  GDrive *drive;
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *path;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));
  g_return_if_fail (G_IS_MOUNT (mount));

  priv = view->priv;

  /* Don't add mounts with removable drives, sidebar will handle them */
  drive = g_mount_get_drive (mount);
  if (drive)
    {
      gboolean is_removable = g_drive_is_media_removable (drive);

      g_object_unref (drive);

      if (is_removable)
        return;
    }

  /* Don't add mounts with a volume, as they'll be already added by add_volume */
  volume = g_mount_get_volume (mount);
  if (volume)
    {
      g_object_unref (volume);
      return;
    }

  icon = g_mount_get_icon (mount);
  name = g_mount_get_name (mount);
  root = g_mount_get_root (mount);
  is_network = root ? g_file_is_native (root) : FALSE;

  if (root)
    path = is_network ? g_file_get_uri (root) : g_file_get_path (root);
  else
    path = NULL;

  if (!g_mount_is_shadowed (mount))
    {
      GtkWidget *row;

      row = g_object_new (GTK_TYPE_PLACES_VIEW_ROW,
                      "icon", icon,
                      "name", name,
                      "path", path ? path : "",
                      "volume", NULL,
                      "mount", mount,
                      NULL);

      if (is_network)
        gtk_container_add (GTK_CONTAINER (priv->network_listbox), row);
      else
        gtk_container_add (GTK_CONTAINER (priv->drives_listbox), row);
    }

  g_clear_object (&root);
  g_clear_object (&icon);
  g_free (name);
  g_free (path);
}

static void
add_drive (GtkPlacesView *view,
           GDrive        *drive)
{
  GList *volumes;
  GList *l;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));
  g_return_if_fail (G_IS_DRIVE (drive));

  /* Removable devices won't appear here */
  if (g_drive_can_eject (drive))
    return;

  volumes = g_drive_get_volumes (drive);

  for (l = volumes; l != NULL; l = l->next)
    add_volume (view, l->data);

  g_list_free_full (volumes, g_object_unref);
}

static void
update_places (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  GList *children;
  GList *mounts;
  GList *volumes;
  GList *drives;
  GList *l;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;

  /* Clear all previously added items */
  children = gtk_container_get_children (GTK_CONTAINER (priv->drives_listbox));
  g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);

  children = gtk_container_get_children (GTK_CONTAINER (priv->network_listbox));
  g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);

  children = gtk_container_get_children (GTK_CONTAINER (priv->recent_servers_listbox));
  g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);

  /* Add currently connected drives */
  drives = g_volume_monitor_get_connected_drives (priv->volume_monitor);

  for (l = drives; l != NULL; l = l->next)
    add_drive (view, l->data);

  g_list_free_full (drives, g_object_unref);

  /* Add all volumes that aren't associated with a drive */
  volumes = g_volume_monitor_get_volumes (priv->volume_monitor);

  for (l = volumes; l != NULL; l = l->next)
    {
      GVolume *volume;
      GDrive *drive;

      volume = l->data;
      drive = g_volume_get_drive (volume);

      if (drive)
        {
          g_object_unref (drive);
          continue;
        }

      add_volume (view, volume);
    }

  g_list_free_full (volumes, g_object_unref);

  /* Add mounts that has no volume (/etc/mtab mounts, ftp, sftp,...) */
  mounts = g_volume_monitor_get_mounts (priv->volume_monitor);

  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount;
      GVolume *volume;

      mount = l->data;
      volume = g_mount_get_volume (mount);

      if (volume)
        {
          g_object_unref (volume);
          continue;
        }

      add_mount (view, mount);
    }

  g_list_free_full (mounts, g_object_unref);

  /* load saved servers */
  populate_servers (view);
}

static gboolean
parse_error (GError *error)
{
  if (error->domain == G_IO_ERROR &&
      error->code == G_IO_ERROR_ALREADY_MOUNTED)
    {
      return TRUE;
    }
  else if (error->domain != G_IO_ERROR ||
           (error->code != G_IO_ERROR_CANCELLED &&
            error->code != G_IO_ERROR_FAILED_HANDLED))
    {
	    /* if it wasn't cancelled show a dialog */
	    g_warning ("Unable to access location: %s", error->message);
      return FALSE;
    }

  return FALSE;
}

static void
location_mount_ready_callback (GObject      *source_file,
                               GAsyncResult *res,
                               gpointer      user_data)
{
  GtkPlacesViewPrivate *priv;
  gboolean should_show;
  GError *error;
  GFile *location;

  g_return_if_fail (GTK_IS_PLACES_VIEW (user_data));
  g_return_if_fail (G_IS_FILE (source_file));

  priv = GTK_PLACES_VIEW (user_data)->priv;
  location = G_FILE (source_file);
  should_show = TRUE;
  error = NULL;

  g_clear_object (&priv->connection_cancellable);

  g_file_mount_enclosing_volume_finish (location, res, &error);

  if (error)
    {
      should_show = parse_error (error);
      g_clear_error (&error);
	  }
  else
    {
      server_list_add_server (location);
    }

  if (should_show)
    emit_open_location (GTK_PLACES_VIEW (user_data), location, GTK_PLACES_OPEN_NORMAL);
}

static void
volume_mount_ready_callback (GObject      *source_volume,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GtkPlacesViewPrivate *priv;
  gboolean should_show;
  GVolume *volume;
  GError *error;

  g_return_if_fail (GTK_IS_PLACES_VIEW (user_data));
  g_return_if_fail (G_IS_VOLUME (source_volume));

  priv = GTK_PLACES_VIEW (user_data)->priv;
  volume = G_VOLUME (source_volume);
  should_show = TRUE;
  error = NULL;

  g_clear_object (&priv->connection_cancellable);

  g_volume_mount_finish (volume, res, &error);

  if (error)
    {
      should_show = parse_error (error);
      g_clear_error (&error);
	  }

  if (should_show)
    {
      GMount *mount;
      GFile *root;

      mount = g_volume_get_mount (volume);
      root = g_mount_get_root (mount);

      g_signal_emit (user_data, places_view_signals [OPEN_LOCATION], 0, root, GTK_PLACES_OPEN_NORMAL);

      g_object_unref (mount);
      g_object_unref (root);
    }
}

static void
mount_location (GtkPlacesView *view,
                GFile         *location)
{
  GtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  priv->connection_cancellable = g_cancellable_new ();

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  g_file_mount_enclosing_volume (location,
                                 0,
                                 operation,
                                 priv->connection_cancellable,
                                 location_mount_ready_callback,
                                 view);

  /* unref operation here - g_file_mount_enclosing_volume() does ref for itself */
  g_object_unref (operation);
}

static void
mount_volume (GtkPlacesView *view,
              GVolume       *volume)
{
  GtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  priv->connection_cancellable = g_cancellable_new ();

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  g_volume_mount (volume,
                  0,
                  operation,
                  priv->connection_cancellable,
                  volume_mount_ready_callback,
                  view);

  /* unref operation here - g_file_mount_enclosing_volume() does ref for itself */
  g_object_unref (operation);
}

static void
on_connect_button_clicked (GtkButton     *button,
                           GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  const gchar *uri;
  GFile *file;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;
  file = NULL;

	uri = gtk_entry_get_text (GTK_ENTRY (priv->address_entry));

  if (uri != NULL && uri[0] != '\0')
    file = g_file_new_for_commandline_arg (uri);

  if (!file)
    {
      g_warning ("Unable to get remote server location");
      return;
    }

  mount_location (view, file);
}

static void
on_address_entry_text_changed (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  const gchar* const *supported_protocols;
  gchar *address, *scheme;
  gboolean supported;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = view->priv;
  supported = FALSE;
  supported_protocols = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

  if (!supported_protocols)
    return;

  address = g_strdup (gtk_entry_get_text (GTK_ENTRY (priv->address_entry)));
  scheme = g_uri_parse_scheme (address);

  if (!scheme)
    goto out;

  supported = g_strv_contains (supported_protocols, scheme) &&
              !g_strv_contains (unsupported_protocols, scheme);

out:
  gtk_widget_set_sensitive (priv->connect_button, supported);
  g_free (address);
}

static void
on_listbox_row_activated (GtkPlacesView    *view,
                          GtkPlacesViewRow *row)
{
  GVolume *volume;
  GFile *location;
  GMount *mount;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));
  g_return_if_fail (GTK_IS_PLACES_VIEW_ROW (row));

  mount = gtk_places_view_row_get_mount (row);
  volume = gtk_places_view_row_get_volume (row);

  if (mount)
    {
      location = g_mount_get_root (mount);

      emit_open_location (view, location, GTK_PLACES_OPEN_NORMAL);

      g_object_unref (location);
    }
  else if (volume && g_volume_can_mount (volume))
    {
        mount_volume (view, volume);
    }
}

static void
gtk_places_view_constructed (GObject *object)
{
  GtkPlacesViewPrivate *priv = GTK_PLACES_VIEW (object)->priv;

  if (G_OBJECT_CLASS (gtk_places_view_parent_class)->constructed)
    G_OBJECT_CLASS (gtk_places_view_parent_class)->constructed (object);

  /* load drives */
  update_places (GTK_PLACES_VIEW (object));

  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-added",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-changed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-removed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-added",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-changed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-removed",
                            G_CALLBACK (update_places),
                            object);
}

static void
gtk_places_view_class_init (GtkPlacesViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  klass->set_local_only = gtk_places_view_real_set_local_only;
  klass->get_local_only = gtk_places_view_real_get_local_only;

  object_class->finalize = gtk_places_view_finalize;
  object_class->constructed = gtk_places_view_constructed;
  object_class->get_property = gtk_places_view_get_property;
  object_class->set_property = gtk_places_view_set_property;

  /**
   * GtkPlacesView::open-location:
   * @view: the object which received the signal.
   * @location: (type Gio.File): #GFile to which the caller should switch.
   *
   * The places view emits this signal when the user selects a location
   * in it.  The calling application should display the contents of that
   * location; for example, a file manager should show a list of files in
   * the specified location.
   *
   * Since: 3.18
   */
  places_view_signals [OPEN_LOCATION] =
          g_signal_new (I_("open-location"),
                        G_OBJECT_CLASS_TYPE (object_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesViewClass, open_location),
                        NULL, NULL, NULL,
                        G_TYPE_NONE, 2,
                        G_TYPE_OBJECT,
                        GTK_TYPE_PLACES_OPEN_FLAGS);

  properties[PROP_LOCAL_ONLY] =
          g_param_spec_boolean ("local-only",
                                P_("Local Only"),
                                P_("Whether the sidebar only includes local files"),
                                FALSE,
                                G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  /* Bind class to template */
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gtk/libgtk/ui/gtkplacesview.ui");

  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, actionbar);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, address_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, connect_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, drives_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, network_grid);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, network_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, recent_servers_listbox);

  gtk_widget_class_bind_template_callback (widget_class, on_address_entry_text_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_connect_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_listbox_row_activated);
}

static void
gtk_places_view_init (GtkPlacesView *self)
{
  self->priv = gtk_places_view_get_instance_private (self);
  self->priv->volume_monitor = g_volume_monitor_get ();

  gtk_widget_init_template (GTK_WIDGET (self));
}

/**
 * gtk_places_view_new:
 *
 * Creates a new #GtkPlacesView widget.
 *
 * The application should connect to at least the
 * #GtkPlacesSidebar::open-location signal to be notified
 * when the user makes a selection in the sidebar.
 *
 * Returns: a newly created #GtkPlacesView
 *
 * Since: 3.18
 */
GtkWidget *
gtk_places_view_new (void)
{
  return g_object_new (GTK_TYPE_PLACES_VIEW, NULL);
}

/**
 * gtk_places_view_get_local_only:
 * @view: a #GtkPlacesView
 *
 * Returns %TRUE if only local volumes are shown, i.e. no networks
 * are displayed.
 *
 * Returns: %TRUE if only local volumes are shown, %FALSE otherwise.
 *
 * Since: 3.18
 */
gboolean
gtk_places_view_get_local_only (GtkPlacesView *view)
{
  GtkPlacesViewClass *class;

  g_return_val_if_fail (GTK_IS_PLACES_VIEW (view), FALSE);

  class = GTK_PLACES_VIEW_GET_CLASS (view);

  g_assert (class->get_local_only != NULL);
  return class->get_local_only (view);
}

/**
 * gtk_places_view_set_local_only:
 * @view: a #GtkPlacesView
 * @local_only: %TRUE to hide remote locations, %FALSE to show.
 *
 * Sets the #GtkPlacesView::local-only property to @local_only.
 *
 * Returns:
 *
 * Since: 3.18
 */
void
gtk_places_view_set_local_only (GtkPlacesView *view,
                                gboolean       local_only)
{
  GtkPlacesViewClass *class;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  class = GTK_PLACES_VIEW_GET_CLASS (view);

  g_assert (class->get_local_only != NULL);
  class->set_local_only (view, local_only);
}
