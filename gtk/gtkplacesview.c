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
#include "gtkmarshalers.h"
#include "gtkplacesviewprivate.h"
#include "gtkplacesviewrowprivate.h"
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

  GCancellable                  *cancellable;

  GtkWidget                     *actionbar;
  GtkWidget                     *address_entry;
  GtkWidget                     *connect_button;
  GtkWidget                     *drives_listbox;
  GtkWidget                     *network_grid;
  GtkWidget                     *network_listbox;
  GtkWidget                     *popup_menu;
  GtkWidget                     *recent_servers_listbox;
  GtkWidget                     *recent_servers_popover;

  GtkEntryCompletion            *address_entry_completion;
  GtkListStore                  *completion_store;

  guint                          local_only : 1;
  guint                          should_open_location : 1;
};

static void        mount_volume                                  (GtkPlacesView *view,
                                                                  GVolume       *volume);

static gboolean    on_button_release_event                       (GtkWidget        *widget,
                                                                  GdkEventButton   *event,
                                                                  GtkPlacesViewRow *sidebar);

G_DEFINE_TYPE_WITH_PRIVATE (GtkPlacesView, gtk_places_view, GTK_TYPE_BOX)

/* GtkPlacesView properties & signals */
enum {
  PROP_0,
  PROP_LOCAL_ONLY,
  PROP_OPEN_FLAGS,
  LAST_PROP
};

enum {
  OPEN_LOCATION,
  SHOW_ERROR_MESSAGE,
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

  priv = gtk_places_view_get_instance_private (view);

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
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          /* only warn if the file exists */
          g_warning ("Unable to open server bookmarks: %s", error->message);
          g_clear_pointer (&bookmarks, g_bookmark_file_free);
        }

      g_error_free (error);
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

/* Returns a toplevel GtkWindow, or NULL if none */
static GtkWindow *
get_toplevel (GtkWidget *widget)
{
  GtkWidget *toplevel;

  toplevel = gtk_widget_get_toplevel (widget);
  if (!gtk_widget_is_toplevel (toplevel))
    return NULL;
  else
    return GTK_WINDOW (toplevel);
}

/* Activates the given row, with the given flags as parameter */
static void
activate_row (GtkPlacesView      *view,
              GtkPlacesViewRow   *row,
              GtkPlacesOpenFlags  flags)
{
  GtkPlacesViewPrivate *priv;
  GVolume *volume;
  GMount *mount;

  priv = gtk_places_view_get_instance_private (view);
  mount = gtk_places_view_row_get_mount (row);
  volume = gtk_places_view_row_get_volume (row);

  if (mount)
    {
      GFile *location = g_mount_get_root (mount);

      emit_open_location (view, location, GTK_PLACES_OPEN_NORMAL);

      g_object_unref (location);
    }
  else if (volume && g_volume_can_mount (volume))
    {
      /*
       * When the row is activated, the unmounted volume shall
       * be mounted and opened right after.
       */
      priv->should_open_location = TRUE;

      mount_volume (view, volume);
    }
}

static gboolean
on_key_press_event (GtkWidget     *widget,
                    GdkEventKey   *event,
                    GtkPlacesView *view)
{
  if (event)
    {
      guint modifiers;

      modifiers = gtk_accelerator_get_default_mod_mask ();

      if (event->keyval == GDK_KEY_Return ||
          event->keyval == GDK_KEY_KP_Enter ||
          event->keyval == GDK_KEY_ISO_Enter ||
          event->keyval == GDK_KEY_space)
        {
          GtkPlacesOpenFlags open_flags;
          GtkWidget *focus_widget;
          GtkWindow *toplevel;

          open_flags = GTK_PLACES_OPEN_NORMAL;
          toplevel = get_toplevel (GTK_WIDGET (view));

          if (!toplevel)
            return FALSE;

          focus_widget = gtk_window_get_focus (toplevel);

          if (!focus_widget)
            return FALSE;

          if ((event->state & modifiers) == GDK_SHIFT_MASK)
            open_flags = GTK_PLACES_OPEN_NEW_TAB;
          else if ((event->state & modifiers) == GDK_CONTROL_MASK)
            open_flags = GTK_PLACES_OPEN_NEW_WINDOW;

          if (GTK_IS_PLACES_VIEW_ROW (focus_widget))
            activate_row (view, GTK_PLACES_VIEW_ROW (focus_widget), open_flags);

          return TRUE;
        }
    }

  return FALSE;
}

static void
gtk_places_view_finalize (GObject *object)
{
  GtkPlacesView *self = (GtkPlacesView *)object;
  GtkPlacesViewPrivate *priv = gtk_places_view_get_instance_private (self);

  g_cancellable_cancel (priv->cancellable);

  g_clear_object (&priv->volume_monitor);
  g_clear_object (&priv->cancellable);

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

  priv = gtk_places_view_get_instance_private (view);
  server_list = server_list_load ();

  if (!server_list)
    return;

  uris = g_bookmark_file_get_uris (server_list, &num_uris);

  if (!uris)
    {
      g_bookmark_file_free (server_list);
      return;
    }

  gtk_list_store_clear (priv->completion_store);

  for (i = 0; i < num_uris; i++)
    {
      GtkTreeIter iter;
      GtkWidget *row;
      GtkWidget *grid;
      GtkWidget *label;
      char *name;

      name = g_bookmark_file_get_title (server_list, uris[i], NULL);

      /* add to the completion list */
      gtk_list_store_append (priv->completion_store, &iter);
      gtk_list_store_set (priv->completion_store,
                          &iter,
                          0, name,
                          1, uris[i],
                          -1);

      /* add to the recent servers listbox */
      row = gtk_list_box_row_new ();
      g_object_set_data_full (G_OBJECT (row), "uri", g_strdup (uris[i]), g_free);

      grid = g_object_new (GTK_TYPE_GRID,
                           "orientation", GTK_ORIENTATION_VERTICAL,
                           "border-width", 6,
                           NULL);

      /* name of the connected uri, if any */
      label = gtk_label_new (name);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_container_add (GTK_CONTAINER (grid), label);

      /* the uri itself */
      label = gtk_label_new (uris[i]);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
      gtk_container_add (GTK_CONTAINER (grid), label);

      gtk_container_add (GTK_CONTAINER (row), grid);
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

  priv = gtk_places_view_get_instance_private (view);

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

      g_signal_connect (gtk_places_view_row_get_event_box (GTK_PLACES_VIEW_ROW (row)),
                        "button-release-event",
                        G_CALLBACK (on_button_release_event),
                        row);

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
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *path;

  priv = gtk_places_view_get_instance_private (view);

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

      g_signal_connect (gtk_places_view_row_get_event_box (GTK_PLACES_VIEW_ROW (row)),
                        "button-release-event",
                        G_CALLBACK (on_button_release_event),
                        row);

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

  priv = gtk_places_view_get_instance_private (view);

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

  /*
   * Since all volumes with an associated GDrive were already added with
   * add_drive before, add all volumes that aren't associated with a
   * drive.
   */
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

  /*
   * Now that all necessary drives and volumes were already added, add mounts
   * that has no volume, such as /etc/mtab mounts, ftp, sftp, etc.
   */
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

static void
server_mount_ready_cb (GObject      *source_file,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GtkPlacesViewPrivate *priv;
  gboolean should_show;
  GError *error;
  GFile *location;

  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (user_data));
  location = G_FILE (source_file);
  should_show = TRUE;
  error = NULL;

  g_file_mount_enclosing_volume_finish (location, res, &error);

  if (error)
    {
      should_show = FALSE;

      if (error->code == G_IO_ERROR_ALREADY_MOUNTED)
        {
          /*
           * If the volume was already mounted, it's not a hard error
           * and we can still continue with the operation.
           */
          should_show = TRUE;
        }
      else if (error->domain != G_IO_ERROR ||
               (error->code != G_IO_ERROR_CANCELLED &&
                error->code != G_IO_ERROR_FAILED_HANDLED))
        {
          /* if it wasn't cancelled show a dialog */
          g_signal_emit (user_data, places_view_signals[SHOW_ERROR_MESSAGE],
                         0, _("Unable to access location"), error->message);
          should_show = FALSE;
        }

      g_clear_error (&error);
    }

  if (should_show)
    {
      server_list_add_server (location);
      update_places (GTK_PLACES_VIEW (user_data));

      if (priv->should_open_location)
        {
          emit_open_location (GTK_PLACES_VIEW (user_data), location, priv->open_flags);
        }
    }
}

static void
volume_mount_ready_cb (GObject      *source_volume,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GtkPlacesViewPrivate *priv;
  gboolean should_show;
  GVolume *volume;
  GError *error;

  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (user_data));
  volume = G_VOLUME (source_volume);
  should_show = TRUE;
  error = NULL;

  g_volume_mount_finish (volume, res, &error);

  g_clear_object (&priv->cancellable);

  if (error)
    {
      should_show = FALSE;

      if (error->code == G_IO_ERROR_ALREADY_MOUNTED)
        {
          /*
           * If the volume was already mounted, it's not a hard error
           * and we can still continue with the operation.
           */
          should_show = TRUE;
        }
      else if (error->domain != G_IO_ERROR ||
               (error->code != G_IO_ERROR_CANCELLED &&
                error->code != G_IO_ERROR_FAILED_HANDLED))
        {
          /* if it wasn't cancelled show a dialog */
          g_signal_emit (user_data, places_view_signals[SHOW_ERROR_MESSAGE],
                         0, _("Unable to access location"), error->message);
          should_show = FALSE;
        }

      g_clear_error (&error);
    }

  if (should_show)
    {
      GMount *mount;
      GFile *root;

      mount = g_volume_get_mount (volume);
      root = g_mount_get_root (mount);

      if (priv->should_open_location)
        {
          emit_open_location (GTK_PLACES_VIEW (user_data), root, priv->open_flags);
        }

      g_object_unref (mount);
      g_object_unref (root);
    }
}

static void
unmount_ready_cb (GObject      *source_mount,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  GtkPlacesViewPrivate *priv;
  GMount *mount;
  GError *error;

  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (user_data));
  mount = G_MOUNT (source_mount);
  error = NULL;

  g_mount_unmount_with_operation_finish (mount, res, &error);

  g_clear_object (&priv->cancellable);

  if (error)
    {
      g_signal_emit (user_data, places_view_signals[SHOW_ERROR_MESSAGE],
                     0, _("Unable to unmount volume"), error->message);

      g_clear_error (&error);
    }
}

static void
mount_server (GtkPlacesView *view,
              GFile         *location)
{
  GtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  priv = gtk_places_view_get_instance_private (view);
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  g_cancellable_cancel (priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  g_file_mount_enclosing_volume (location,
                                 0,
                                 operation,
                                 priv->cancellable,
                                 server_mount_ready_cb,
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

  priv = gtk_places_view_get_instance_private (view);
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  g_cancellable_cancel (priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  g_volume_mount (volume,
                  0,
                  operation,
                  priv->cancellable,
                  volume_mount_ready_cb,
                  view);

  /* unref operation here - g_file_mount_enclosing_volume() does ref for itself */
  g_object_unref (operation);
}

/* Callback used when the file list's popup menu is detached */
static void
popup_menu_detach_cb (GtkWidget *attach_widget,
                      GtkMenu   *menu)
{
  GtkPlacesViewPrivate *priv;

  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (attach_widget));
  priv->popup_menu = NULL;
}

static void
get_view_and_file (GtkPlacesViewRow  *row,
                   GtkWidget        **view,
                   GFile            **file)
{
  if (view)
    *view = gtk_widget_get_ancestor (GTK_WIDGET (row), GTK_TYPE_PLACES_VIEW);

  if (file)
    {
      GVolume *volume;
      GMount *mount;

      volume = gtk_places_view_row_get_volume (row);
      mount = gtk_places_view_row_get_mount (row);

      if (mount)
        *file = g_mount_get_root (mount);
      else if (volume)
        *file = g_volume_get_activation_root (volume);
      else
        *file = NULL;
    }
}

static void
open_cb (GtkMenuItem      *item,
         GtkPlacesViewRow *row)
{
  GtkWidget *view;
  GFile *file;

  get_view_and_file (row, &view, &file);

  if (file)
    emit_open_location (GTK_PLACES_VIEW (view), file, GTK_PLACES_OPEN_NORMAL);

  g_clear_object (&file);
}

static void
open_in_new_tab_cb (GtkMenuItem      *item,
                    GtkPlacesViewRow *row)
{
  GtkWidget *view;
  GFile *file;

  get_view_and_file (row, &view, &file);

  emit_open_location (GTK_PLACES_VIEW (view), file, GTK_PLACES_OPEN_NEW_TAB);

  g_clear_object (&file);
}

static void
open_in_new_window_cb (GtkMenuItem      *item,
                       GtkPlacesViewRow *row)
{
  GtkWidget *view;
  GFile *file;

  get_view_and_file (row, &view, &file);

  emit_open_location (GTK_PLACES_VIEW (view), file, GTK_PLACES_OPEN_NEW_WINDOW);

  g_clear_object (&file);
}

static void
mount_cb (GtkMenuItem      *item,
          GtkPlacesViewRow *row)
{
  GtkPlacesViewPrivate *priv;
  GtkWidget *view;
  GVolume *volume;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), GTK_TYPE_PLACES_VIEW);
  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (view));
  volume = gtk_places_view_row_get_volume (row);

  /*
   * When the mount item is activated, it's expected that
   * the volume only gets mounted, without opening it after
   * the operation is complete.
   */
  priv->should_open_location = FALSE;

  mount_volume (GTK_PLACES_VIEW (view), volume);
}

static void
unmount_cb (GtkMenuItem      *item,
            GtkPlacesViewRow *row)
{
  GtkPlacesViewPrivate *priv;
  GMountOperation *mount_op;
  GtkWidget *view;
  GMount *mount;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), GTK_TYPE_PLACES_VIEW);
  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (view));
  mount = gtk_places_view_row_get_mount (row);

  g_cancellable_cancel (priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (row))));
  g_mount_unmount_with_operation (mount,
                                  0,
                                  mount_op,
                                  NULL,
                                  unmount_ready_cb,
                                  view);
  g_object_unref (mount_op);
}

/* Constructs the popup menu if needed */
static void
build_popup_menu (GtkPlacesView    *view,
                  GtkPlacesViewRow *row)
{
  GtkPlacesViewPrivate *priv;
  GtkWidget *item;
  GMount *mount;

  priv = gtk_places_view_get_instance_private (view);
  mount = gtk_places_view_row_get_mount (row);

  priv->popup_menu = gtk_menu_new ();
  gtk_style_context_add_class (gtk_widget_get_style_context (priv->popup_menu),
                               GTK_STYLE_CLASS_CONTEXT_MENU);

  gtk_menu_attach_to_widget (GTK_MENU (priv->popup_menu),
                             GTK_WIDGET (view),
                             popup_menu_detach_cb);

  /* Open item is always present */
  item = gtk_menu_item_new_with_mnemonic (_("_Open"));
  g_signal_connect (item,
                    "activate",
                    G_CALLBACK (open_cb),
                    row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);

  if (priv->open_flags & GTK_PLACES_OPEN_NEW_TAB)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Tab"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (open_in_new_tab_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }

  if (priv->open_flags & GTK_PLACES_OPEN_NEW_WINDOW)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Window"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (open_in_new_window_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }

  /* Separator */
  item = gtk_separator_menu_item_new ();
  gtk_widget_show (item);
  gtk_menu_shell_insert (GTK_MENU_SHELL (priv->popup_menu), item, -1);

  /* Mount/Unmount items */
  if (mount)
    {
      item = gtk_menu_item_new_with_mnemonic (_("_Unmount"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (unmount_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }
  else
    {
      item = gtk_menu_item_new_with_mnemonic (_("_Mount"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (mount_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }
}

static void
popup_menu (GtkPlacesViewRow *row,
            GdkEventButton   *event)
{
  GtkPlacesViewPrivate *priv;
  GtkWidget *view;
  gint button;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), GTK_TYPE_PLACES_VIEW);
  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (view));

  g_clear_pointer (&priv->popup_menu, gtk_widget_destroy);

  build_popup_menu (GTK_PLACES_VIEW (view), row);

  /* The event button needs to be 0 if we're popping up this menu from
   * a button release, else a 2nd click outside the menu with any button
   * other than the one that invoked the menu will be ignored (instead
   * of dismissing the menu). This is a subtle fragility of the GTK menu code.
   */
  if (event)
    {
      if (event->type == GDK_BUTTON_RELEASE)
        button = 0;
      else
        button = event->button;
    }
  else
    {
      button = 0;
    }

  gtk_menu_popup (GTK_MENU (priv->popup_menu),
                  NULL,
                  NULL,
                  NULL,
                  NULL,
                  button,
                  event ? event->time : gtk_get_current_event_time ());
}

static gboolean
on_button_release_event (GtkWidget        *widget,
                         GdkEventButton   *event,
                         GtkPlacesViewRow *row)
{
  if (row &&
      event &&
      event->button == 3)
    {
      popup_menu (row, event);
    }

  return TRUE;
}

static void
on_connect_button_clicked (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  const gchar *uri;
  GFile *file;

  priv = gtk_places_view_get_instance_private (view);
  file = NULL;

  /*
   * Since the 'Connect' button is updated whenever the typed
   * address changes, it is sufficient to check if it's sensitive
   * or not, in order to determine if the given address is valid.
   */
  if (!gtk_widget_get_sensitive (priv->connect_button))
    return;

  uri = gtk_entry_get_text (GTK_ENTRY (priv->address_entry));

  if (uri != NULL && uri[0] != '\0')
    file = g_file_new_for_commandline_arg (uri);

  if (file)
    {
      gtk_entry_set_text (GTK_ENTRY (priv->address_entry), "");
      mount_server (view, file);
    }
  else
    {
      g_signal_emit (view, places_view_signals[SHOW_ERROR_MESSAGE],
                     0, _("Unable to get remote server location"), NULL);
    }
}

static void
on_address_entry_text_changed (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;
  const gchar* const *supported_protocols;
  gchar *address, *scheme;
  gboolean supported;

  priv = gtk_places_view_get_instance_private (view);
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
                          GtkPlacesViewRow *row,
                          GtkWidget        *listbox)
{
  GtkPlacesViewPrivate *priv;

  priv = gtk_places_view_get_instance_private (view);

  if (listbox == priv->recent_servers_listbox)
    {
      gchar *uri;

      uri = g_object_get_data (G_OBJECT (row), "uri");

      gtk_entry_set_text (GTK_ENTRY (priv->address_entry), uri);

      gtk_widget_hide (priv->recent_servers_popover);
    }
  else
    {
      activate_row (view, row, GTK_PLACES_OPEN_NORMAL);
    }
}

static void
gtk_places_view_constructed (GObject *object)
{
  GtkPlacesViewPrivate *priv;

  priv = gtk_places_view_get_instance_private (GTK_PLACES_VIEW (object));

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

  /**
   * GtkPlacesView::show-error-message:
   * @view: the object which received the signal.
   * @primary: primary message with a summary of the error to show.
   * @secondary: secondary message with details of the error to show.
   *
   * The places view emits this signal when it needs the calling
   * application to present an error message.  Most of these messages
   * refer to mounting or unmounting media, for example, when a drive
   * cannot be started for some reason.
   *
   * Since: 3.18
   */
  places_view_signals [SHOW_ERROR_MESSAGE] =
          g_signal_new (I_("show-error-message"),
                        G_OBJECT_CLASS_TYPE (object_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesViewClass, show_error_message),
                        NULL, NULL,
                        _gtk_marshal_VOID__STRING_STRING,
                        G_TYPE_NONE, 2,
                        G_TYPE_STRING,
                        G_TYPE_STRING);

  properties[PROP_LOCAL_ONLY] =
          g_param_spec_boolean ("local-only",
                                P_("Local Only"),
                                P_("Whether the sidebar only includes local files"),
                                FALSE,
                                G_PARAM_READWRITE);

  properties[PROP_OPEN_FLAGS] =
          g_param_spec_flags ("open-flags",
                              P_("Open Flags"),
                              P_("Modes in which the calling application can open locations selected in the sidebar"),
                              GTK_TYPE_PLACES_OPEN_FLAGS,
                              GTK_PLACES_OPEN_NORMAL,
                              G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  /* Bind class to template */
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gtk/libgtk/ui/gtkplacesview.ui");

  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, actionbar);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, address_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, address_entry_completion);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, completion_store);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, connect_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, drives_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, network_grid);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, network_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, recent_servers_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtkPlacesView, recent_servers_popover);

  gtk_widget_class_bind_template_callback (widget_class, on_address_entry_text_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_connect_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_key_press_event);
  gtk_widget_class_bind_template_callback (widget_class, on_listbox_row_activated);
}

static void
gtk_places_view_init (GtkPlacesView *self)
{
  GtkPlacesViewPrivate *priv;

  priv = gtk_places_view_get_instance_private (self);

  priv->volume_monitor = g_volume_monitor_get ();
  priv->open_flags = GTK_PLACES_OPEN_NORMAL;

  gtk_widget_init_template (GTK_WIDGET (self));
}

/**
 * gtk_places_view_new:
 *
 * Creates a new #GtkPlacesView widget.
 *
 * The application should connect to at least the
 * #GtkPlacesView::open-location signal to be notified
 * when the user makes a selection in the view.
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
 * gtk_places_view_set_open_flags:
 * @view: a #GtkPlacesView
 * @flags: Bitmask of modes in which the calling application can open locations
 *
 * Sets the way in which the calling application can open new locations from
 * the places view.  For example, some applications only open locations
 * “directly” into their main view, while others may support opening locations
 * in a new notebook tab or a new window.
 *
 * This function is used to tell the places @view about the ways in which the
 * application can open new locations, so that the view can display (or not)
 * the “Open in new tab” and “Open in new window” menu items as appropriate.
 *
 * When the #GtkPlacesView::open-location signal is emitted, its flags
 * argument will be set to one of the @flags that was passed in
 * gtk_places_view_set_open_flags().
 *
 * Passing 0 for @flags will cause #GTK_PLACES_OPEN_NORMAL to always be sent
 * to callbacks for the “open-location” signal.
 *
 * Since: 3.18
 */
void
gtk_places_view_set_open_flags (GtkPlacesView      *view,
                                GtkPlacesOpenFlags  flags)
{
  GtkPlacesViewPrivate *priv;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = gtk_places_view_get_instance_private (view);

  if (priv->open_flags != flags)
    {
      priv->open_flags = flags;
      g_object_notify_by_pspec (G_OBJECT (view), properties[PROP_OPEN_FLAGS]);
    }
}

/**
 * gtk_places_view_get_open_flags:
 * @view: a #GtkPlacesSidebar
 *
 * Gets the open flags.
 *
 * Returns: the #GtkPlacesOpenFlags of @view
 *
 * Since: 3.18
 */
GtkPlacesOpenFlags
gtk_places_view_get_open_flags (GtkPlacesView *view)
{
  GtkPlacesViewPrivate *priv;

  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (view), 0);

  priv = gtk_places_view_get_instance_private (view);

  return priv->open_flags;
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
  GtkPlacesViewPrivate *priv;

  g_return_val_if_fail (GTK_IS_PLACES_VIEW (view), FALSE);

  priv = gtk_places_view_get_instance_private (view);

  return priv->local_only;
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
  GtkPlacesViewPrivate *priv;

  g_return_if_fail (GTK_IS_PLACES_VIEW (view));

  priv = gtk_places_view_get_instance_private (view);

  if (priv->local_only != local_only)
    {
      priv->local_only = local_only;

      gtk_widget_set_visible (priv->actionbar, !local_only);
      gtk_widget_set_visible (priv->network_grid, !local_only);

      g_object_notify_by_pspec (G_OBJECT (view), properties [PROP_LOCAL_ONLY]);
    }
}
