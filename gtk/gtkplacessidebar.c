/*
 *  GtkPlacesSidebar - sidebar widget for places in the filesystem
 *
 *  This code comes from Nautilus, GNOME’s file manager.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this library; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Authors : Mr Jamie McCracken (jamiemcc at blueyonder dot co dot uk)
 *            Cosimo Cecchi <cosimoc@gnome.org>
 *            Federico Mena Quintero <federico@gnome.org>
 *
 */

/* TODO:
 *
 * * Fix instances of "#if 0"
 *
 * * Fix FIXMEs
 *
 * * Grep for "NULL-GError" and see if they should be taken care of
 *
 * * Although we do g_mount_unmount_with_operation(), Nautilus used to do
 *   nautilus_file_operations_unmount_mount_full() to unmount a volume.  With
 *   that, Nautilus does the "volume has trash, empty it first?" dance.  Cosimo
 *   suggests that this logic should be part of GtkMountOperation, which can
 *   have Unix-specific code for emptying trash.
 *
 * * Sync nautilus commit 17a85b78acc78b573c2e1776b348ed348e19adb7
 *
 */

#include "config.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "gdk/gdkkeysyms.h"
#include "gtkbookmarksmanager.h"
#include "gtkcelllayout.h"
#include "gtkcellrenderertext.h"
#include "gtkcellrendererpixbuf.h"
#include "gtkfilesystem.h"
#include "gtkicontheme.h"
#include "gtkintl.h"
#include "gtkmain.h"
#include "gtkmarshalers.h"
#include "gtkmenuitem.h"
#include "gtkmountoperation.h"
#include "gtkplacessidebar.h"
#include "gtkscrolledwindow.h"
#include "gtkseparatormenuitem.h"
#include "gtksettings.h"
#include "gtktrashmonitor.h"
#include "gtktreeselection.h"
#include "gtktreednd.h"
#include "gtktypebuiltins.h"
#include "gtkwindow.h"
#include "gtkpopover.h"
#include "gtkgrid.h"
#include "gtklabel.h"
#include "gtkbutton.h"
#include "gtklistbox.h"

/**
 * SECTION:gtkplacessidebar
 * @Short_description: Sidebar that displays frequently-used places in the file system
 * @Title: GtkPlacesSidebar
 * @See_also: #GtkFileChooser
 *
 * #GtkPlacesSidebar is a widget that displays a list of frequently-used places in the
 * file system:  the user’s home directory, the user’s bookmarks, and volumes and drives.
 * This widget is used as a sidebar in #GtkFileChooser and may be used by file managers
 * and similar programs.
 *
 * The places sidebar displays drives and volumes, and will automatically mount
 * or unmount them when the user selects them.
 *
 * Applications can hook to various signals in the places sidebar to customize
 * its behavior.  For example, they can add extra commands to the context menu
 * of the sidebar.
 *
 * While bookmarks are completely in control of the user, the places sidebar also
 * allows individual applications to provide extra shortcut folders that are unique
 * to each application.  For example, a Paint program may want to add a shortcut
 * for a Clipart folder.  You can do this with gtk_places_sidebar_add_shortcut().
 *
 * To make use of the places sidebar, an application at least needs to connect
 * to the #GtkPlacesSidebar::open-location signal.  This is emitted when the
 * user selects in the sidebar a location to open.  The application should also
 * call gtk_places_sidebar_set_location() when it changes the currently-viewed
 * location.
 */

#define EJECT_BUTTON_XPAD 8
#define ICON_CELL_XPAD 8
#define TIMEOUT_EXPAND 500

/* These are used when a destination-side DND operation is taking place.
 * Normally, when a file is being hovered directly over a bookmark,
 * we’ll be in DROP_STATE_NORMAL.
 *
 * But when a file is being hovered between bookmarks, this means the user
 * may want to create a new bookmark for that file between those bookmarks.
 * In that case, the drop state will be *not* DROP_STATE_NORMAL.
 *
 * When the drop state is FADING_OUT, it means that the user is hovering
 * directly over an existing bookmark and an immediate drop will cause the
 * file being dragged to be dropped on the bookmark, instead of causing
 * a new bookmark to be created.
 */
typedef enum {
  DROP_STATE_NORMAL,
  DROP_STATE_NEW_BOOKMARK_FADING_IN,
  DROP_STATE_NEW_BOOKMARK_FADING_OUT,
  DROP_STATE_NEW_BOOKMARK_ARMED,
  DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT,
} DropState;

struct _GtkPlacesSidebar {
  GtkScrolledWindow parent;

  GtkWidget *list_box;

  GtkTreeView       *tree_view;
  GtkCellRenderer   *eject_icon_cell_renderer;
  GtkCellRenderer   *text_cell_renderer;
  GtkListStore      *store;
  GtkBookmarksManager     *bookmarks_manager;
  GVolumeMonitor    *volume_monitor;
  GtkTrashMonitor   *trash_monitor;
  GtkSettings       *gtk_settings;
  GFile             *current_location;

  GtkWidget *rename_popover;
  GtkWidget *rename_entry;
  GtkWidget *rename_button;
  GtkWidget *rename_error;
  gchar *rename_uri;

  gulong trash_monitor_changed_id;

  gboolean devices_header_added;
  gboolean bookmarks_header_added;

  /* DND */
  GList     *drag_list; /* list of GFile */
  gint       drag_data_info;
  gboolean   dragging_over;

  /* volume mounting - delayed open process */
  GtkPlacesOpenFlags go_to_after_mount_open_flags;
  GCancellable *cancellable;

  GtkWidget *popup_menu;
  GSList *shortcuts;

  GDBusProxy *hostnamed_proxy;
  GCancellable *hostnamed_cancellable;
  gchar *hostname;

  GtkPlacesOpenFlags open_flags;

  DropState drop_state;
  guint drag_leave_timeout_id;
  gchar *drop_target_uri;
  guint switch_location_timer;

  guint mounting               : 1;
  guint  drag_data_received    : 1;
  guint drop_occured           : 1;
  guint show_desktop_set       : 1;
  guint show_desktop           : 1;
  guint show_connect_to_server : 1;
  guint show_enter_location    : 1;
  guint local_only             : 1;
};

struct _GtkPlacesSidebarClass {
  GtkScrolledWindowClass parent;

  void    (* open_location)          (GtkPlacesSidebar   *sidebar,
                                      GFile              *location,
                                      GtkPlacesOpenFlags  open_flags);
  void    (* populate_popup)         (GtkPlacesSidebar   *sidebar,
                                      GtkMenu            *menu,
                                      GFile              *selected_item,
                                      GVolume            *selected_volume);
  void    (* show_error_message)     (GtkPlacesSidebar   *sidebar,
                                      const gchar        *primary,
                                      const gchar        *secondary);
  void    (* show_connect_to_server) (GtkPlacesSidebar   *sidebar);
  GdkDragAction (* drag_action_requested)  (GtkPlacesSidebar   *sidebar,
                                      GdkDragContext     *context,
                                      GFile              *dest_file,
                                      GList              *source_file_list);
  GdkDragAction (* drag_action_ask)  (GtkPlacesSidebar   *sidebar,
                                      GdkDragAction       actions);
  void    (* drag_perform_drop)      (GtkPlacesSidebar   *sidebar,
                                      GFile              *dest_file,
                                      GList              *source_file_list,
                                      GdkDragAction       action);
  void    (* show_enter_location)    (GtkPlacesSidebar   *sidebar);
};

enum {
  PLACES_SIDEBAR_COLUMN_ROW_TYPE,
  PLACES_SIDEBAR_COLUMN_URI,
  PLACES_SIDEBAR_COLUMN_DRIVE,
  PLACES_SIDEBAR_COLUMN_VOLUME,
  PLACES_SIDEBAR_COLUMN_MOUNT,
  PLACES_SIDEBAR_COLUMN_NAME,
  PLACES_SIDEBAR_COLUMN_GICON,
  PLACES_SIDEBAR_COLUMN_INDEX,
  PLACES_SIDEBAR_COLUMN_EJECT,
  PLACES_SIDEBAR_COLUMN_NO_EJECT,
  PLACES_SIDEBAR_COLUMN_BOOKMARK,
  PLACES_SIDEBAR_COLUMN_TOOLTIP,
  PLACES_SIDEBAR_COLUMN_SECTION_TYPE,
  PLACES_SIDEBAR_COLUMN_HEADING_TEXT,
  PLACES_SIDEBAR_COLUMN_SENSITIVE,
  PLACES_SIDEBAR_COLUMN_COUNT
};

typedef enum {
  PLACES_0,
  PLACES_BUILT_IN,
  PLACES_XDG_DIR,
  PLACES_MOUNTED_VOLUME,
  PLACES_BOOKMARK,
  PLACES_HEADING,
  PLACES_CONNECT_TO_SERVER,
  PLACES_ENTER_LOCATION,
  PLACES_DROP_FEEDBACK,
  N_PLACES
} PlaceType;

typedef enum {
  SECTION_0,
  SECTION_DEVICES,
  SECTION_BOOKMARKS,
  SECTION_COMPUTER,
  SECTION_NETWORK,
  N_SECTIONS
} SectionType;

enum {
  OPEN_LOCATION,
  POPULATE_POPUP,
  SHOW_ERROR_MESSAGE,
  SHOW_CONNECT_TO_SERVER,
  SHOW_ENTER_LOCATION,
  DRAG_ACTION_REQUESTED,
  DRAG_ACTION_ASK,
  DRAG_PERFORM_DROP,
  LAST_SIGNAL
};

enum {
  PROP_LOCATION = 1,
  PROP_OPEN_FLAGS,
  PROP_SHOW_DESKTOP,
  PROP_SHOW_CONNECT_TO_SERVER,
  PROP_SHOW_ENTER_LOCATION,
  PROP_LOCAL_ONLY,
  NUM_PROPERTIES
};

/* Names for themed icons */
#define ICON_NAME_HOME     "user-home-symbolic"
#define ICON_NAME_DESKTOP  "user-desktop-symbolic"
#define ICON_NAME_FILESYSTEM     "drive-harddisk-symbolic"
#define ICON_NAME_EJECT    "media-eject-symbolic"
#define ICON_NAME_NETWORK  "network-workgroup-symbolic"
#define ICON_NAME_NETWORK_SERVER "network-server-symbolic"
#define ICON_NAME_FOLDER_NETWORK "folder-remote-symbolic"

#define ICON_NAME_FOLDER                "folder-symbolic"
#define ICON_NAME_FOLDER_DESKTOP  "user-desktop-symbolic"
#define ICON_NAME_FOLDER_DOCUMENTS      "folder-documents-symbolic"
#define ICON_NAME_FOLDER_DOWNLOAD       "folder-download-symbolic"
#define ICON_NAME_FOLDER_MUSIC    "folder-music-symbolic"
#define ICON_NAME_FOLDER_PICTURES       "folder-pictures-symbolic"
#define ICON_NAME_FOLDER_PUBLIC_SHARE   "folder-publicshare-symbolic"
#define ICON_NAME_FOLDER_TEMPLATES      "folder-templates-symbolic"
#define ICON_NAME_FOLDER_VIDEOS   "folder-videos-symbolic"
#define ICON_NAME_FOLDER_SAVED_SEARCH   "folder-saved-search-symbolic"

static guint places_sidebar_signals [LAST_SIGNAL] = { 0 };
static GParamSpec *properties[NUM_PROPERTIES] = { NULL, };


/* Identifiers for target types */
enum {
  GTK_TREE_MODEL_ROW,
  TEXT_URI_LIST
};

/* Target types for dragging from the shortcuts list */
static const GtkTargetEntry dnd_source_targets[] = {
  { "GTK_TREE_MODEL_ROW", GTK_TARGET_SAME_WIDGET, GTK_TREE_MODEL_ROW }
};

/* Target types for dropping into the shortcuts list */
static const GtkTargetEntry dnd_drop_targets [] = {
  { "GTK_TREE_MODEL_ROW", GTK_TARGET_SAME_WIDGET, GTK_TREE_MODEL_ROW }
};

/* Drag and drop interface declarations */
typedef struct {
  GtkListStore parent;

  GtkPlacesSidebar *sidebar;
} ShortcutsModel;

typedef struct {
  GtkListStoreClass parent_class;
} ShortcutsModelClass;

#define SHORTCUTS_MODEL_TYPE (shortcuts_model_get_type ())
#define SHORTCUTS_MODEL(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), SHORTCUTS_MODEL_TYPE, ShortcutsModel))

static GType shortcuts_model_get_type (void);
static void shortcuts_model_drag_source_iface_init (GtkTreeDragSourceIface *iface);

G_DEFINE_TYPE_WITH_CODE (ShortcutsModel, shortcuts_model, GTK_TYPE_LIST_STORE,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_TREE_DRAG_SOURCE,
                                                shortcuts_model_drag_source_iface_init));

static GtkListStore *shortcuts_model_new (GtkPlacesSidebar *sidebar);

G_DEFINE_TYPE (GtkPlacesSidebar, gtk_places_sidebar, GTK_TYPE_SCROLLED_WINDOW);

#define SIDEBAR_TYPE_ROW (sidebar_row_get_type ())
G_DECLARE_DERIVABLE_TYPE (SidebarRow, sidebar_row, SIDEBAR, ROW, GtkListBoxRow)

struct _SidebarRowClass
{
  GtkListBoxRowClass parent_class;
};

typedef struct
{
  GIcon *icon;
  GtkWidget *icon_widget;
  gchar *label;
  GtkWidget *label_widget;
  gboolean ejectable;
  GtkWidget *eject_button;
  gint order_index;
  gint section_type;
  gint place_type;
  gchar *uri;
  GDrive *drive;
  GVolume *volume;
  GMount *mount;
  GtkPlacesSidebar *sidebar;
} SidebarRowPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (SidebarRow, sidebar_row, GTK_TYPE_LIST_BOX_ROW)

static gboolean eject_or_unmount_bookmark  (GtkPlacesSidebar   *sidebar,
                                            SidebarRow         *row);
static gboolean eject_or_unmount_selection (GtkPlacesSidebar   *sidebar);
static void  check_unmount_and_eject       (GMount             *mount,
                                            GVolume            *volume,
                                            GDrive             *drive,
                                            gboolean           *show_unmount,
                                            gboolean           *show_eject);
static int bookmarks_get_first_index (GtkPlacesSidebar *sidebar);

enum
{
  PROP_0,
  PROP_ICON,
  PROP_LABEL,
  PROP_EJECTABLE,
  PROP_SIDEBAR,
  PROP_ORDER_INDEX,
  PROP_SECTION_TYPE,
  PROP_PLACE_TYPE,
  PROP_URI,
  PROP_DRIVE,
  PROP_VOLUME,
  PROP_MOUNT,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

static void
sidebar_row_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  SidebarRow *self = SIDEBAR_ROW (object);
  SidebarRowPrivate *priv = sidebar_row_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SIDEBAR:
      {
        g_value_set_object (value, priv->sidebar);
        break;
      }

    case PROP_ICON:
      {
        g_value_set_object (value, priv->icon);
        break;
      }

    case PROP_LABEL:
      {
        g_value_set_string (value, priv->label);
        break;
      }

    case PROP_EJECTABLE:
      {
        g_value_set_boolean (value, priv->ejectable);
        break;
      }

    case PROP_ORDER_INDEX:
      {
        g_value_set_int (value, priv->order_index);
          break;
      }

    case PROP_SECTION_TYPE:
      {
        g_value_set_int (value, priv->section_type);
        break;
      }

    case PROP_PLACE_TYPE:
      {
        g_value_set_int (value, priv->place_type);
        break;
      }

    case PROP_URI:
      {
        g_value_set_string (value, priv->uri);
        break;
      }

    case PROP_DRIVE:
      {
        g_value_set_object (value, priv->drive);
        break;
      }

    case PROP_VOLUME:
      {
        g_value_set_object (value, priv->volume);
        break;
      }

    case PROP_MOUNT:
      {
        g_value_set_object (value, priv->mount);
        break;
      }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
sidebar_row_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  SidebarRow *self = SIDEBAR_ROW (object);
  SidebarRowPrivate *priv = sidebar_row_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SIDEBAR:
      {
        priv->sidebar = g_value_get_object (value);
        break;
      }

    case PROP_ICON:
      {
        g_clear_object (&priv->icon);
        if (value != NULL)
          {
            priv->icon = g_object_ref (g_value_get_object (value));
            gtk_image_set_from_gicon (GTK_IMAGE (priv->icon_widget), priv->icon, GTK_ICON_SIZE_MENU);
          }
        else
          {
            priv->icon = NULL;
            gtk_image_clear (GTK_IMAGE (priv->icon_widget));
          }
        break;
      }

    case PROP_LABEL:
      {
        g_free (priv->label);
        priv->label = g_strdup (g_value_get_string (value));
        gtk_label_set_text (GTK_LABEL (priv->label_widget), priv->label);
        break;
      }

    case PROP_EJECTABLE:
      {
        priv->ejectable = g_value_get_boolean (value);
        if (priv->ejectable)
          {
            gtk_widget_set_sensitive (priv->eject_button, TRUE);
            gtk_widget_set_opacity (priv->eject_button, 1);
          }
        else
          {
            gtk_widget_set_sensitive (priv->eject_button, FALSE);
            gtk_widget_set_opacity (priv->eject_button, 0);
          }
        break;
      }

    case PROP_ORDER_INDEX:
      {
        priv->order_index = g_value_get_int (value);
        break;
      }

    case PROP_SECTION_TYPE:
      {
        priv->section_type = g_value_get_int (value);
        if (priv->section_type != SECTION_COMPUTER)
          gtk_label_set_ellipsize (GTK_LABEL (priv->label_widget), PANGO_ELLIPSIZE_MIDDLE);
        else
          gtk_label_set_ellipsize (GTK_LABEL (priv->label_widget), PANGO_ELLIPSIZE_NONE);
        break;
      }

    case PROP_PLACE_TYPE:
      {
        priv->place_type = g_value_get_int (value);
        break;
      }

    case PROP_URI:
      {
        g_free (priv->uri);
        priv->uri = g_strdup (g_value_get_string (value));
        break;
      }

    case PROP_DRIVE:
      {
        gpointer *object;

        g_clear_object (&priv->drive);
        object = g_value_get_object (value);
        if (object != NULL)
          priv->drive = g_object_ref (object);
        break;
      }

    case PROP_VOLUME:
      {
        gpointer *object;

        g_clear_object (&priv->volume);
        object = g_value_get_object (value);
        if (object != NULL)
          priv->volume = g_object_ref (object);
        break;
      }

    case PROP_MOUNT:
      {
        gpointer *object;

        g_clear_object (&priv->mount);
        object = g_value_get_object (value);
        if (object != NULL)
          priv->mount = g_object_ref (object);
        break;
      }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_eject_button_clicked (GtkButton *button,
                         gpointer   user_data)
{
  SidebarRow *self = SIDEBAR_ROW (user_data);
  SidebarRowPrivate *priv = sidebar_row_get_instance_private (self);

  eject_or_unmount_bookmark (priv->sidebar, self);
}

static void
sidebar_row_init (SidebarRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

static void
sidebar_row_class_init (SidebarRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = sidebar_row_get_property;
  object_class->set_property = sidebar_row_set_property;

  gParamSpecs [PROP_SIDEBAR] =
    g_param_spec_object ("sidebar",
                         "Sidebar",
                         "Sidebar",
                         GTK_TYPE_PLACES_SIDEBAR,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SIDEBAR,
                                   gParamSpecs [PROP_SIDEBAR]);
  gParamSpecs [PROP_ICON] =
    g_param_spec_object ("icon",
                         "icon",
                         "The place icon.",
                         G_TYPE_ICON,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ICON,
                                   gParamSpecs [PROP_ICON]);

  gParamSpecs [PROP_LABEL] =
    g_param_spec_string ("label",
                         "label",
                         "The label text.",
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_LABEL,
                                   gParamSpecs [PROP_LABEL]);

  gParamSpecs [PROP_EJECTABLE] =
    g_param_spec_boolean ("ejectable",
                          "Ejectable",
                          "Ejectable",
                          TRUE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_EJECTABLE,
                                   gParamSpecs [PROP_EJECTABLE]);

  gParamSpecs [PROP_ORDER_INDEX] =
    g_param_spec_int ("order-index",
                      "OrderIndex",
                      "Order Index",
                      0, G_MAXINT, 0,
                      (G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ORDER_INDEX,
                                   gParamSpecs [PROP_ORDER_INDEX]);

  gParamSpecs [PROP_SECTION_TYPE] =
    g_param_spec_int ("section-type",
                      "section type",
                      "The section type.",
                      SECTION_0, N_SECTIONS, SECTION_0,
                      (G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS |
                       G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_SECTION_TYPE,
                                   gParamSpecs [PROP_SECTION_TYPE]);

  gParamSpecs [PROP_PLACE_TYPE] =
    g_param_spec_int ("place-type",
                      "place type",
                      "The place type.",
                      PLACES_0, N_PLACES, PLACES_0,
                      (G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS |
                       G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_PLACE_TYPE,
                                   gParamSpecs [PROP_PLACE_TYPE]);

  gParamSpecs [PROP_URI] =
    g_param_spec_string ("uri",
                         "Uri",
                         "Uri",
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_URI,
                                   gParamSpecs [PROP_URI]);
  gParamSpecs [PROP_DRIVE] =
    g_param_spec_object ("drive",
                         "Drive",
                         "Drive",
                         G_TYPE_DRIVE,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DRIVE,
                                   gParamSpecs [PROP_DRIVE]);

  gParamSpecs [PROP_VOLUME] =
    g_param_spec_object ("volume",
                         "Volume",
                         "Volume",
                         G_TYPE_VOLUME,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_VOLUME,
                                   gParamSpecs [PROP_VOLUME]);

  gParamSpecs [PROP_MOUNT] =
    g_param_spec_object ("mount",
                         "Mount",
                         "Mount",
                         G_TYPE_MOUNT,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_MOUNT,
                                   gParamSpecs [PROP_MOUNT]);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gtk/libgtk/ui/sidebarrow.ui");

  gtk_widget_class_bind_template_child_private (widget_class, SidebarRow, icon_widget);
  gtk_widget_class_bind_template_child_private (widget_class, SidebarRow, label_widget);
  gtk_widget_class_bind_template_child_private (widget_class, SidebarRow, eject_button);

  gtk_widget_class_bind_template_callback (widget_class, on_eject_button_clicked);
}

static void
emit_open_location (GtkPlacesSidebar   *sidebar,
                    GFile              *location,
                    GtkPlacesOpenFlags  open_flags)
{
  if ((open_flags & sidebar->open_flags) == 0)
    open_flags = GTK_PLACES_OPEN_NORMAL;

  g_signal_emit (sidebar, places_sidebar_signals[OPEN_LOCATION], 0,
                 location, open_flags);
}

static void
emit_populate_popup (GtkPlacesSidebar *sidebar, 
                     GtkMenu          *menu,
                     GFile            *selected_item,
                     GVolume          *selected_volume)
{
  g_signal_emit (sidebar, places_sidebar_signals[POPULATE_POPUP], 0,
                 menu, selected_item, selected_volume);
}

static void
emit_show_error_message (GtkPlacesSidebar *sidebar,
                         const gchar      *primary,
                         const gchar      *secondary)
{
  g_signal_emit (sidebar, places_sidebar_signals[SHOW_ERROR_MESSAGE], 0,
                 primary, secondary);
}

static void
emit_show_connect_to_server (GtkPlacesSidebar *sidebar)
{
  g_signal_emit (sidebar, places_sidebar_signals[SHOW_CONNECT_TO_SERVER], 0);
}

static void
emit_show_enter_location (GtkPlacesSidebar *sidebar)
{
  g_signal_emit (sidebar, places_sidebar_signals[SHOW_ENTER_LOCATION], 0);
}

static GdkDragAction
emit_drag_action_requested (GtkPlacesSidebar *sidebar,
                            GdkDragContext   *context,
                            GFile            *dest_file,
                            GList            *source_file_list)
{
  GdkDragAction ret_action = 0;

  g_signal_emit (sidebar, places_sidebar_signals[DRAG_ACTION_REQUESTED], 0,
                 context, dest_file, source_file_list, &ret_action);

  return ret_action;
}

static GdkDragAction
emit_drag_action_ask (GtkPlacesSidebar *sidebar,
                      GdkDragAction     actions)
{
  GdkDragAction ret_action = 0;

  g_signal_emit (sidebar, places_sidebar_signals[DRAG_ACTION_ASK], 0,
                 actions, &ret_action);

  return ret_action;
}

static void
emit_drag_perform_drop (GtkPlacesSidebar *sidebar,
                        GFile            *dest_file,
                        GList            *source_file_list,
                        GdkDragAction     action)
{
  g_signal_emit (sidebar, places_sidebar_signals[DRAG_PERFORM_DROP], 0,
                 dest_file, source_file_list, action);
}

static GtkTreeIter
add_heading (GtkPlacesSidebar *sidebar,
             SectionType       section_type,
             const gchar      *title)
{
  GtkTreeIter iter;

  gtk_list_store_append (sidebar->store, &iter);
  gtk_list_store_set (sidebar->store, &iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, PLACES_HEADING,
                      PLACES_SIDEBAR_COLUMN_SECTION_TYPE, section_type,
                      PLACES_SIDEBAR_COLUMN_HEADING_TEXT, title,
                      PLACES_SIDEBAR_COLUMN_EJECT, FALSE,
                      PLACES_SIDEBAR_COLUMN_NO_EJECT, TRUE,
                      -1);

  return iter;
}

static void
add_separator (GtkListBoxRow *row,
               GtkListBoxRow *before,
               gpointer       user_data)
{
  SectionType row_section_type;
  SectionType before_section_type;
  gchar *name, *name2 = NULL;
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (user_data);
  GtkWidget *separator;

  g_object_get (row, "section-type", &row_section_type, "label", &name, NULL);
  if (before)
    g_object_get (before, "section-type", &before_section_type, "label", &name2, NULL);
  else
    before_section_type = SECTION_0;

  switch (row_section_type)
    {
    case SECTION_DEVICES:
      if (!sidebar->devices_header_added && before && before_section_type != row_section_type)
        {
          separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
          gtk_widget_set_margin_top (separator, 4);
          gtk_widget_set_margin_bottom (separator, 4);
          gtk_list_box_row_set_header (row, separator);
          sidebar->devices_header_added = TRUE;
        }
      break;

    case SECTION_BOOKMARKS:
      if (!sidebar->bookmarks_header_added && before && before_section_type != row_section_type)
        {
          separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
          gtk_widget_set_margin_top (separator, 4);
          gtk_widget_set_margin_bottom (separator, 4);
          gtk_list_box_row_set_header (row, separator);
          sidebar->bookmarks_header_added = TRUE;
        }
      break;

    case SECTION_0:
      {
        if (before == NULL)
          gtk_widget_set_margin_top (GTK_WIDGET (row), 4);
      }

    default:
      break;
  }
}

static void
add_place (GtkPlacesSidebar *sidebar,
           PlaceType         place_type,
           SectionType       section_type,
           const gchar      *name,
           GIcon            *icon,
           const gchar      *uri,
           GDrive           *drive,
           GVolume          *volume,
           GMount           *mount,
           const gint        index,
           const gchar      *tooltip)
{
  gboolean show_eject, show_unmount;
  gboolean show_eject_button;
  SidebarRow *row;

  check_unmount_and_eject (mount, volume, drive,
                           &show_unmount, &show_eject);

  if (show_unmount || show_eject)
    g_assert (place_type != PLACES_BOOKMARK);

  if (mount == NULL)
    show_eject_button = FALSE;
  else
    show_eject_button = (show_unmount || show_eject);

  row = g_object_new (SIDEBAR_TYPE_ROW,
                      "sidebar", sidebar,
                      "icon", icon,
                      "label", name,
                      "ejectable", show_eject_button,
                      "order-index", index,
                      "section-type", section_type,
                      "place-type", place_type,
                      "uri", uri,
                      "drive", drive,
                      "volume", volume,
                      "mount", mount,
                      NULL);

  gtk_container_add (GTK_CONTAINER (sidebar->list_box), GTK_WIDGET (row));
  gtk_widget_show_all (GTK_WIDGET (row));
}

static GIcon *
special_directory_get_gicon (GUserDirectory directory)
{
#define ICON_CASE(x)                      \
  case G_USER_DIRECTORY_ ## x:                                    \
          return g_themed_icon_new_with_default_fallbacks (ICON_NAME_FOLDER_ ## x);

  switch (directory)
    {

    ICON_CASE (DESKTOP);
    ICON_CASE (DOCUMENTS);
    ICON_CASE (DOWNLOAD);
    ICON_CASE (MUSIC);
    ICON_CASE (PICTURES);
    ICON_CASE (PUBLIC_SHARE);
    ICON_CASE (TEMPLATES);
    ICON_CASE (VIDEOS);

    default:
      return g_themed_icon_new_with_default_fallbacks (ICON_NAME_FOLDER);
    }

#undef ICON_CASE
}

static gboolean
recent_files_setting_is_enabled (GtkPlacesSidebar *sidebar)
{
  GtkSettings *settings;
  gboolean enabled;

  if (gtk_widget_has_screen (GTK_WIDGET (sidebar)))
    settings = gtk_settings_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (sidebar)));
  else
    settings = gtk_settings_get_default ();

  g_object_get (settings, "gtk-recent-files-enabled", &enabled, NULL);

  return enabled;
}

static gboolean
recent_scheme_is_supported (void)
{
  const gchar * const *supported;
  gint i;

  supported = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());
  if (supported == NULL)
    return FALSE;

  for (i = 0; supported[i] != NULL; i++)
    {
      if (strcmp ("recent", supported[i]) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
should_show_recent (GtkPlacesSidebar *sidebar)
{
  return recent_files_setting_is_enabled (sidebar) && recent_scheme_is_supported ();
}

static gboolean
path_is_home_dir (const gchar *path)
{
  GFile *home_dir;
  GFile *location;
  const gchar *home_path;
  gboolean res;

  home_path = g_get_home_dir ();
  if (!home_path)
    return FALSE;

  home_dir = g_file_new_for_path (home_path);
  location = g_file_new_for_path (path);
  res = g_file_equal (home_dir, location);

  g_object_unref (home_dir);
  g_object_unref (location);

  return res;
}

static void
open_home (GtkPlacesSidebar *sidebar)
{
  const gchar *home_path;
  GFile       *home_dir;

  home_path = g_get_home_dir ();
  if (!home_path)
    return;

  home_dir = g_file_new_for_path (home_path);
  emit_open_location (sidebar, home_dir, 0);

  g_object_unref (home_dir);
}

static void
add_special_dirs (GtkPlacesSidebar *sidebar)
{
  GList *dirs;
  gint index;

  dirs = NULL;
  for (index = 0; index < G_USER_N_DIRECTORIES; index++)
    {
      const gchar *path;
      GFile *root;
      GIcon *icon;
      gchar *name;
      gchar *mount_uri;
      gchar *tooltip;

      if (!_gtk_bookmarks_manager_get_is_xdg_dir_builtin (index))
        continue;

      path = g_get_user_special_dir (index);

      /* XDG resets special dirs to the home directory in case
       * it's not finiding what it expects. We don't want the home
       * to be added multiple times in that weird configuration.
       */
      if (path == NULL ||
          path_is_home_dir (path) ||
          g_list_find_custom (dirs, path, (GCompareFunc) g_strcmp0) != NULL)
        continue;

      root = g_file_new_for_path (path);

      name = _gtk_bookmarks_manager_get_bookmark_label (sidebar->bookmarks_manager, root);
      if (!name)
        name = g_file_get_basename (root);

      icon = special_directory_get_gicon (index);
      mount_uri = g_file_get_uri (root);
      tooltip = g_file_get_parse_name (root);

      add_place (sidebar, PLACES_XDG_DIR,
                 SECTION_COMPUTER,
                 name, icon, mount_uri,
                 NULL, NULL, NULL, 0,
                 tooltip);
      g_free (name);
      g_object_unref (root);
      g_object_unref (icon);
      g_free (mount_uri);
      g_free (tooltip);

      dirs = g_list_prepend (dirs, (gchar *)path);
    }

  g_list_free (dirs);
}

static gchar *
get_home_directory_uri (void)
{
  const gchar *home;

  home = g_get_home_dir ();
  if (!home)
    return NULL;

  return g_filename_to_uri (home, NULL, NULL);
}

static gchar *
get_desktop_directory_uri (void)
{
  const gchar *name;

  name = g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP);

  /* "To disable a directory, point it to the homedir."
   * See http://freedesktop.org/wiki/Software/xdg-user-dirs
   */
  if (path_is_home_dir (name))
    return NULL;

  return g_filename_to_uri (name, NULL, NULL);
}

static gboolean
should_show_file (GtkPlacesSidebar *sidebar,
                  GFile            *file)
{
  gchar *path;

  if (!sidebar->local_only)
    return TRUE;

  path = g_file_get_path (file);
  if (path)
    {
      g_free (path);
      return TRUE;
    }

  return FALSE;
}

static gboolean
file_is_shown (GtkPlacesSidebar *sidebar,
               GFile            *file)
{
  gchar *uri;
  GList *rows;
  GList *l;
  gboolean found = FALSE;

  rows = gtk_container_get_children (GTK_CONTAINER (sidebar->list_box));
  l = rows;
  while (l != NULL && !found)
    {
      g_object_get (SIDEBAR_ROW (l->data), "uri", &uri, NULL);
      if (uri)
        {
          GFile *other;
          other = g_file_new_for_uri (uri);
          found = g_file_equal (file, other);
          g_object_unref (other);
        }

      l = l->next;
    }

  g_list_free (rows);

  return found;
}

static void
on_app_shortcuts_query_complete (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      data)
{
  GtkPlacesSidebar *sidebar = data;
  GFile *file = G_FILE (source);
  GFileInfo *info;

  info = g_file_query_info_finish (file, result, NULL);

  if (info)
    {
      gchar *uri;
      gchar *tooltip;
      const gchar *name;
      GIcon *icon;
      int pos = 0;

      name = g_file_info_get_display_name (info);
      icon = g_file_info_get_symbolic_icon (info);
      uri = g_file_get_uri (file);
      tooltip = g_file_get_parse_name (file);

      /* XXX: we could avoid this by using an ancillary closure
       * with the index coming from add_application_shortcuts(),
       * but in terms of algorithmic overhead, the application
       * shortcuts is not going to be really big
       */
      pos = g_slist_index (sidebar->shortcuts, file);

      add_place (sidebar, PLACES_BUILT_IN,
                 SECTION_COMPUTER,
                 name, icon, uri,
                 NULL, NULL, NULL,
                 pos,
                 tooltip);

      g_free (uri);
      g_free (tooltip);

      g_object_unref (info);
    }
}

static void
add_application_shortcuts (GtkPlacesSidebar *sidebar)
{
  GSList *l;

  for (l = sidebar->shortcuts; l; l = l->next)
    {
      GFile *file = l->data;

      if (!should_show_file (sidebar, file))
        continue;

      if (file_is_shown (sidebar, file))
        continue;

      g_file_query_info_async (file,
                               "standard::display-name,standard::symbolic-icon",
                               G_FILE_QUERY_INFO_NONE,
                               G_PRIORITY_DEFAULT,
                               sidebar->cancellable,
                               on_app_shortcuts_query_complete,
                               sidebar);
    }
}

static gboolean
get_selected_iter (GtkPlacesSidebar *sidebar,
                   GtkTreeIter      *iter)
{
  GtkTreeSelection *selection;

  selection = gtk_tree_view_get_selection (sidebar->tree_view);

  return gtk_tree_selection_get_selected (selection, NULL, iter);
}

typedef struct {
  GtkPlacesSidebar *sidebar;
  int index;
  gboolean is_native;
} BookmarkQueryClosure;

static void
on_bookmark_query_info_complete (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      data)
{
  BookmarkQueryClosure *clos = data;
  GtkPlacesSidebar *sidebar = clos->sidebar;
  GFile *root = G_FILE (source);
  GError *error = NULL;
  GFileInfo *info;
  gchar *bookmark_name;
  gchar *mount_uri;
  gchar *tooltip;
  GIcon *icon;

  info = g_file_query_info_finish (root, result, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    goto out;

  bookmark_name = _gtk_bookmarks_manager_get_bookmark_label (sidebar->bookmarks_manager, root);
  if (bookmark_name == NULL && info != NULL)
    bookmark_name = g_strdup (g_file_info_get_display_name (info));
  else if (bookmark_name == NULL)
    {
      /* Don't add non-UTF-8 bookmarks */
      bookmark_name = g_file_get_basename (root);
      if (!g_utf8_validate (bookmark_name, -1, NULL))
        {
          g_free (bookmark_name);
          goto out;
        }
    }

  if (info)
    icon = g_object_ref (g_file_info_get_symbolic_icon (info));
  else
    icon = g_themed_icon_new_with_default_fallbacks (clos->is_native ? ICON_NAME_FOLDER : ICON_NAME_FOLDER_NETWORK);

  mount_uri = g_file_get_uri (root);
  tooltip = g_file_get_parse_name (root);

  add_place (sidebar, PLACES_BOOKMARK,
             SECTION_BOOKMARKS,
             bookmark_name, icon, mount_uri,
             NULL, NULL, NULL, clos->index,
             tooltip);

  g_free (mount_uri);
  g_free (tooltip);
  g_free (bookmark_name);
  g_object_unref (icon);

out:
  g_clear_object (&info);
  g_clear_error (&error);
  g_slice_free (BookmarkQueryClosure, clos);
}

static void
update_places (GtkPlacesSidebar *sidebar)
{
  GList *mounts, *l, *ll;
  GMount *mount;
  GList *drives;
  GDrive *drive;
  GList *volumes;
  GVolume *volume;
  GSList *bookmarks, *sl;
  gint index;
  gchar *original_uri, *mount_uri, *name, *identifier;
  GtkListBoxRow *selected;
  gchar *home_uri;
  GIcon *icon;
  GFile *root;
  gchar *tooltip;
  GList *network_mounts, *network_volumes;

  /* save original selection */
  selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
  if (selected)
    g_object_get (SIDEBAR_ROW (selected), "uri", &original_uri, NULL);
  else
    original_uri = NULL;

  g_cancellable_cancel (sidebar->cancellable);

  g_object_unref (sidebar->cancellable);
  sidebar->cancellable = g_cancellable_new ();

  gtk_container_foreach (GTK_CONTAINER (sidebar->list_box),
                         (GtkCallback) gtk_widget_destroy,
                         NULL);

  sidebar->devices_header_added = FALSE;
  sidebar->bookmarks_header_added = FALSE;

  network_mounts = network_volumes = NULL;

  /* add built-in places */
  if (should_show_recent (sidebar))
    {
      mount_uri = "recent:///";
      icon = g_themed_icon_new_with_default_fallbacks ("document-open-recent-symbolic");
      add_place (sidebar, PLACES_BUILT_IN,
                 SECTION_COMPUTER,
                 _("Recent"), icon, mount_uri,
                 NULL, NULL, NULL, 0,
                 _("Recent files"));
      g_object_unref (icon);
    }

  /* home folder */
  home_uri = get_home_directory_uri ();
  icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_HOME);
  add_place (sidebar, PLACES_BUILT_IN,
             SECTION_COMPUTER,
             _("Home"), icon, home_uri,
             NULL, NULL, NULL, 0,
             _("Open your personal folder"));
  g_object_unref (icon);
  g_free (home_uri);

  /* desktop */
  if (sidebar->show_desktop)
    {
      mount_uri = get_desktop_directory_uri ();
      if (mount_uri)
        {
          icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_DESKTOP);
          add_place (sidebar, PLACES_BUILT_IN,
                     SECTION_COMPUTER,
                     _("Desktop"), icon, mount_uri,
                     NULL, NULL, NULL, 0,
                     _("Open the contents of your desktop in a folder"));
          g_object_unref (icon);
          g_free (mount_uri);
        }
    }

  /* XDG directories */
  add_special_dirs (sidebar);

  if (sidebar->show_enter_location)
    {
      icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_NETWORK_SERVER);
      add_place (sidebar, PLACES_ENTER_LOCATION,
                 SECTION_COMPUTER,
                 _("Enter Location"), icon, NULL,
                 NULL, NULL, NULL, 0,
                 _("Manually enter a location"));
      g_object_unref (icon);
    }

  /* Trash */
  if (!sidebar->local_only)
    {
      mount_uri = "trash:///"; /* No need to strdup */
      icon = _gtk_trash_monitor_get_icon (sidebar->trash_monitor);
      add_place (sidebar, PLACES_BUILT_IN,
                 SECTION_COMPUTER,
                 _("Trash"), icon, mount_uri,
                 NULL, NULL, NULL, 0,
                 _("Open the trash"));
      g_object_unref (icon);
    }

  /* Application-side shortcuts */
  add_application_shortcuts (sidebar);

  /* go through all connected drives */
  drives = g_volume_monitor_get_connected_drives (sidebar->volume_monitor);

  for (l = drives; l != NULL; l = l->next)
    {
      drive = l->data;

      volumes = g_drive_get_volumes (drive);
      if (volumes != NULL)
        {
          for (ll = volumes; ll != NULL; ll = ll->next)
            {
              volume = ll->data;
              identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);

              if (g_strcmp0 (identifier, "network") == 0)
                {
                  g_free (identifier);
                  network_volumes = g_list_prepend (network_volumes, volume);
                  continue;
                }
              g_free (identifier);

              mount = g_volume_get_mount (volume);
              if (mount != NULL)
                {
                  /* Show mounted volume in the sidebar */
                  icon = g_mount_get_symbolic_icon (mount);
                  root = g_mount_get_default_location (mount);
                  mount_uri = g_file_get_uri (root);
                  name = g_mount_get_name (mount);
                  tooltip = g_file_get_parse_name (root);

                  add_place (sidebar, PLACES_MOUNTED_VOLUME,
                             SECTION_DEVICES,
                             name, icon, mount_uri,
                             drive, volume, mount, 0, tooltip);
                  g_object_unref (root);
                  g_object_unref (mount);
                  g_object_unref (icon);
                  g_free (tooltip);
                  g_free (name);
                  g_free (mount_uri);
                }
              else
                {
                  /* Do show the unmounted volumes in the sidebar;
                   * this is so the user can mount it (in case automounting
                   * is off).
                   *
                   * Also, even if automounting is enabled, this gives a visual
                   * cue that the user should remember to yank out the media if
                   * he just unmounted it.
                   */
                  icon = g_volume_get_symbolic_icon (volume);
                  name = g_volume_get_name (volume);
                  tooltip = g_strdup_printf (_("Mount and open “%s”"), name);

                  add_place (sidebar, PLACES_MOUNTED_VOLUME,
                             SECTION_DEVICES,
                             name, icon, NULL,
                             drive, volume, NULL, 0, tooltip);
                  g_object_unref (icon);
                  g_free (name);
                  g_free (tooltip);
                }
              g_object_unref (volume);
            }
          g_list_free (volumes);
        }
      else
        {
          if (g_drive_is_media_removable (drive) && !g_drive_is_media_check_automatic (drive))
            {
              /* If the drive has no mountable volumes and we cannot detect media change.. we
               * display the drive in the sidebar so the user can manually poll the drive by
               * right clicking and selecting "Rescan..."
               *
               * This is mainly for drives like floppies where media detection doesn't
               * work.. but it's also for human beings who like to turn off media detection
               * in the OS to save battery juice.
               */
              icon = g_drive_get_symbolic_icon (drive);
              name = g_drive_get_name (drive);
              tooltip = g_strdup_printf (_("Mount and open “%s”"), name);

              add_place (sidebar, PLACES_BUILT_IN,
                         SECTION_DEVICES,
                         name, icon, NULL,
                         drive, NULL, NULL, 0, tooltip);
              g_object_unref (icon);
              g_free (tooltip);
              g_free (name);
            }
        }
      g_object_unref (drive);
    }
  g_list_free (drives);

  /* add all volumes that is not associated with a drive */
  volumes = g_volume_monitor_get_volumes (sidebar->volume_monitor);
  for (l = volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      drive = g_volume_get_drive (volume);
      if (drive != NULL)
        {
          g_object_unref (volume);
          g_object_unref (drive);
          continue;
        }

      identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);

      if (g_strcmp0 (identifier, "network") == 0)
        {
          g_free (identifier);
          network_volumes = g_list_prepend (network_volumes, volume);
          continue;
        }
      g_free (identifier);

      mount = g_volume_get_mount (volume);
      if (mount != NULL)
        {
          icon = g_mount_get_symbolic_icon (mount);
          root = g_mount_get_default_location (mount);
          mount_uri = g_file_get_uri (root);
          tooltip = g_file_get_parse_name (root);
          name = g_mount_get_name (mount);
          add_place (sidebar, PLACES_MOUNTED_VOLUME,
                     SECTION_DEVICES,
                     name, icon, mount_uri,
                     NULL, volume, mount, 0, tooltip);
          g_object_unref (mount);
          g_object_unref (root);
          g_object_unref (icon);
          g_free (name);
          g_free (tooltip);
          g_free (mount_uri);
        }
      else
        {
          /* see comment above in why we add an icon for an unmounted mountable volume */
          icon = g_volume_get_symbolic_icon (volume);
          name = g_volume_get_name (volume);
          add_place (sidebar, PLACES_MOUNTED_VOLUME,
                     SECTION_DEVICES,
                     name, icon, NULL,
                     NULL, volume, NULL, 0, name);
          g_object_unref (icon);
          g_free (name);
        }
      g_object_unref (volume);
    }
  g_list_free (volumes);

  /* file system root */
  mount_uri = "file:///"; /* No need to strdup */
  icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_FILESYSTEM);
  add_place (sidebar, PLACES_BUILT_IN,
             SECTION_DEVICES,
             sidebar->hostname, icon, mount_uri,
             NULL, NULL, NULL, 0,
             _("Open the contents of the file system"));
  g_object_unref (icon);

  /* add mounts that has no volume (/etc/mtab mounts, ftp, sftp,...) */
  mounts = g_volume_monitor_get_mounts (sidebar->volume_monitor);

  for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (g_mount_is_shadowed (mount))
        {
          g_object_unref (mount);
          continue;
        }
      volume = g_mount_get_volume (mount);
      if (volume != NULL)
        {
          g_object_unref (volume);
          g_object_unref (mount);
          continue;
        }
      root = g_mount_get_default_location (mount);

      if (!g_file_is_native (root))
        {
          network_mounts = g_list_prepend (network_mounts, mount);
          g_object_unref (root);
          continue;
        }

      icon = g_mount_get_symbolic_icon (mount);
      mount_uri = g_file_get_uri (root);
      name = g_mount_get_name (mount);
      tooltip = g_file_get_parse_name (root);
      add_place (sidebar, PLACES_MOUNTED_VOLUME,
                 SECTION_COMPUTER,
                 name, icon, mount_uri,
                 NULL, NULL, mount, 0, tooltip);
      g_object_unref (root);
      g_object_unref (mount);
      g_object_unref (icon);
      g_free (name);
      g_free (mount_uri);
      g_free (tooltip);
    }
  g_list_free (mounts);

  /* add bookmarks */
  bookmarks = _gtk_bookmarks_manager_list_bookmarks (sidebar->bookmarks_manager);

  for (sl = bookmarks, index = 0; sl; sl = sl->next, index++)
    {
      gboolean is_native;
      BookmarkQueryClosure *clos;

      root = sl->data;
      is_native = g_file_is_native (root);

#if 0
      /* FIXME: remove this?  If we *do* show bookmarks for nonexistent files, the user will eventually clean them up */
      if (!nautilus_bookmark_get_exists (bookmark))
        continue;
#endif

      if (_gtk_bookmarks_manager_get_is_builtin (sidebar->bookmarks_manager, root))
        continue;

      if (sidebar->local_only && !is_native)
        continue;

      clos = g_slice_new (BookmarkQueryClosure);
      clos->sidebar = sidebar;
      clos->index = index;
      clos->is_native = is_native;
      g_file_query_info_async (root,
                               "standard::display-name,standard::symbolic-icon",
                               G_FILE_QUERY_INFO_NONE,
                               G_PRIORITY_DEFAULT,
                               sidebar->cancellable,
                               on_bookmark_query_info_complete,
                               clos);
    }

  g_slist_foreach (bookmarks, (GFunc) g_object_unref, NULL);
  g_slist_free (bookmarks);

  /* network */
  if (!sidebar->local_only)
    {
      add_heading (sidebar, SECTION_NETWORK, _("Network"));

      mount_uri = "network:///";
      icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_NETWORK);
      add_place (sidebar, PLACES_BUILT_IN,
                 SECTION_NETWORK,
                 _("Browse Network"), icon, mount_uri,
                 NULL, NULL, NULL, 0,
                 _("Browse the contents of the network"));
      g_object_unref (icon);

      if (sidebar->show_connect_to_server)
        {
          icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_NETWORK_SERVER);
          add_place (sidebar, PLACES_CONNECT_TO_SERVER,
                     SECTION_NETWORK,
                     _("Connect to Server"), icon, NULL,
                     NULL, NULL, NULL, 0,
                     _("Connect to a network server address"));
          g_object_unref (icon);
        }

      network_volumes = g_list_reverse (network_volumes);
      for (l = network_volumes; l != NULL; l = l->next)
        {
          volume = l->data;
          mount = g_volume_get_mount (volume);

          if (mount != NULL)
            {
              network_mounts = g_list_prepend (network_mounts, mount);
              continue;
            }
          else
            {
              icon = g_volume_get_symbolic_icon (volume);
              name = g_volume_get_name (volume);
              tooltip = g_strdup_printf (_("Mount and open “%s”"), name);

              add_place (sidebar, PLACES_MOUNTED_VOLUME,
                         SECTION_NETWORK,
                         name, icon, NULL,
                         NULL, volume, NULL, 0, tooltip);
              g_object_unref (icon);
              g_free (name);
              g_free (tooltip);
            }
        }

      network_mounts = g_list_reverse (network_mounts);
      for (l = network_mounts; l != NULL; l = l->next)
        {
          mount = l->data;
          root = g_mount_get_default_location (mount);
          icon = g_mount_get_symbolic_icon (mount);
          mount_uri = g_file_get_uri (root);
          name = g_mount_get_name (mount);
          tooltip = g_file_get_parse_name (root);
          add_place (sidebar, PLACES_MOUNTED_VOLUME,
                     SECTION_NETWORK,
                     name, icon, mount_uri,
                     NULL, NULL, mount, 0, tooltip);
          g_object_unref (root);
          g_object_unref (icon);
          g_free (name);
          g_free (mount_uri);
          g_free (tooltip);
        }
    }

  g_list_free_full (network_volumes, g_object_unref);
  g_list_free_full (network_mounts, g_object_unref);

  gtk_widget_show_all (GTK_WIDGET (sidebar));

  /* restore original selection */
  if (original_uri)
    {
      GFile *restore;

      restore = g_file_new_for_uri (original_uri);
      gtk_places_sidebar_set_location (sidebar, restore);
      g_object_unref (restore);

      g_free (original_uri);
    }
}

static gboolean
pos_is_into_or_before (GtkTreeViewDropPosition pos)
{
  return (pos == GTK_TREE_VIEW_DROP_BEFORE || pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
}

static void
update_possible_drop_targets (GtkPlacesSidebar *sidebar,
                              gboolean          dragging)
{
  GtkTreeIter iter;
  PlaceType place_type;
  gchar *uri;
  gboolean sensitive;

  if (!gtk_tree_model_get_iter_first (GTK_TREE_MODEL (sidebar->store), &iter))
    return;

  do
    {
      sensitive = TRUE;
      gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store), &iter,
                          PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                          PLACES_SIDEBAR_COLUMN_URI, &uri,
                          -1);
      if (dragging)
        {
          switch (place_type)
            {
              case PLACES_CONNECT_TO_SERVER:
                sensitive = FALSE;
                break;
              case PLACES_BUILT_IN:
                if (g_strcmp0 (uri, "recent:///") == 0)
                  sensitive = FALSE;

                break;
              default:
                break;
           }
        }

      gtk_list_store_set (sidebar->store, &iter,
                          PLACES_SIDEBAR_COLUMN_SENSITIVE, sensitive,
                          -1);
      g_free (uri);
    }
  while (gtk_tree_model_iter_next (GTK_TREE_MODEL (sidebar->store), &iter));
}

/* Computes the appropriate row and position for dropping */
static gboolean
compute_drop_position (GtkTreeView              *tree_view,
                       gint                      x,
                       gint                      y,
                       GtkTreePath             **path,
                       GtkTreeViewDropPosition  *pos,
                       GtkPlacesSidebar         *sidebar)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  PlaceType place_type;
  SectionType section_type;
  gboolean drop_possible;

  if (!gtk_tree_view_get_dest_row_at_pos (tree_view, x, y, path, pos))
    return FALSE;

  model = gtk_tree_view_get_model (tree_view);

  gtk_tree_model_get_iter (model, &iter, *path);
  gtk_tree_model_get (model, &iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                      PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                      -1);

  drop_possible = TRUE;

  /* Normalize drops on the feedback row */
  if (place_type == PLACES_DROP_FEEDBACK)
    {
      *pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
      goto out;
    }

  /* Never drop on headings, but special case the bookmarks heading,
   * so we can drop bookmarks in between it and the first bookmark.
   */
  if (place_type == PLACES_HEADING && section_type != SECTION_BOOKMARKS)
    drop_possible = FALSE;

  /* Dragging a bookmark? */
  if (sidebar->drag_data_received &&
      sidebar->drag_data_info == GTK_TREE_MODEL_ROW)
    {
      /* Don't allow reordering bookmarks into non-bookmark areas */
      if (section_type != SECTION_BOOKMARKS)
        drop_possible = FALSE;

      /* Bookmarks can only be reordered.  Disallow dropping directly
       * into them; only allow dropping between them.
       */
      if (place_type == PLACES_HEADING)
        {
          if (pos_is_into_or_before (*pos))
            drop_possible = FALSE;
          else
            *pos = GTK_TREE_VIEW_DROP_AFTER;
        }
      else
        {
          if (pos_is_into_or_before (*pos))
            *pos = GTK_TREE_VIEW_DROP_BEFORE;
          else
            *pos = GTK_TREE_VIEW_DROP_AFTER;
        }
    }
  else
    { 
      /* Dragging a file */

      /* Outside the bookmarks section, URIs can only be dropped
       * directly into places items.  Inside the bookmarks section,
       * they can be dropped between items (to create new bookmarks)
       * or in items themselves (to request a move/copy file
       * operation).
       */
      if (section_type != SECTION_BOOKMARKS)
        *pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
      else
        {
          if (place_type == PLACES_HEADING)
            {
              if (pos_is_into_or_before (*pos))
                drop_possible = FALSE;
              else
                *pos = GTK_TREE_VIEW_DROP_AFTER;
            }
        }
    }

  /* Disallow drops on recent:/// */
  if (place_type == PLACES_BUILT_IN)
    {
      gchar *uri;

      gtk_tree_model_get (model, &iter,
                          PLACES_SIDEBAR_COLUMN_URI, &uri,
                          -1);

      if (g_strcmp0 (uri, "recent:///") == 0)
        drop_possible = FALSE;

      g_free (uri);
    }

out:

  if (!drop_possible)
    {
      gtk_tree_path_free (*path);
      *path = NULL;
      return FALSE;
    }

  return TRUE;
}

static gboolean
get_drag_data (GtkTreeView    *tree_view,
               GdkDragContext *context,
               guint           time)
{
  GdkAtom target;

  target = gtk_drag_dest_find_target (GTK_WIDGET (tree_view),
                                      context,
                                      NULL);

  if (target == GDK_NONE)
    return FALSE;

  gtk_drag_get_data (GTK_WIDGET (tree_view),
                     context, target, time);

  return TRUE;
}

static void
remove_switch_location_timer (GtkPlacesSidebar *sidebar)
{
  if (sidebar->switch_location_timer != 0)
    {
      g_source_remove (sidebar->switch_location_timer);
      sidebar->switch_location_timer = 0;
    }
}

static void
free_drag_data (GtkPlacesSidebar *sidebar)
{
  sidebar->drag_data_received = FALSE;

  if (sidebar->drag_list)
    {
      g_list_free_full (sidebar->drag_list, g_object_unref);
      sidebar->drag_list = NULL;
    }

  remove_switch_location_timer (sidebar);

  g_free (sidebar->drop_target_uri);
  sidebar->drop_target_uri = NULL;
}

static gboolean
switch_location_timer (gpointer user_data)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (user_data);
  GFile *location;

  sidebar->switch_location_timer = 0;

  location = g_file_new_for_uri (sidebar->drop_target_uri);
  emit_open_location (sidebar, location, 0);
  g_object_unref (location);

  return FALSE;
}

static void
check_switch_location_timer (GtkPlacesSidebar *sidebar,
                             const gchar      *uri)
{
  if (g_strcmp0 (uri, sidebar->drop_target_uri) == 0)
    return;

  remove_switch_location_timer (sidebar);

  g_free (sidebar->drop_target_uri);
  sidebar->drop_target_uri = NULL;

  if (uri != NULL)
    {
      sidebar->drop_target_uri = g_strdup (uri);
      sidebar->switch_location_timer = gdk_threads_add_timeout (TIMEOUT_EXPAND, switch_location_timer, sidebar);
      g_source_set_name_by_id (sidebar->switch_location_timer, "[gtk+] switch_location_timer");
    }
}

static void
remove_drop_bookmark_feedback_row (GtkPlacesSidebar *sidebar)
{
  if (sidebar->drop_state != DROP_STATE_NORMAL)
    {
      gboolean success;
      GtkTreeIter iter;
      int bookmarks_index;

      bookmarks_index = bookmarks_get_first_index (sidebar);

      success = gtk_tree_model_iter_nth_child (GTK_TREE_MODEL (sidebar->store), &iter, NULL, bookmarks_index);
      g_assert (success);
      gtk_list_store_remove (sidebar->store, &iter);

      sidebar->drop_state = DROP_STATE_NORMAL;
    }
}

static void
show_new_bookmark_row (GtkPlacesSidebar *sidebar,
                       GtkTreePath      *path)
{
  GtkTreeIter iter;
  int bookmarks_index;
  gint drop_target_index;
  GtkTreePath *new_bookmark_path;
  GIcon *new_bookmark_icon;

  bookmarks_index = bookmarks_get_first_index (sidebar);

  /* Add the row if it doesn't exists yet */
  if (sidebar->drop_state == DROP_STATE_NORMAL)
    {
      new_bookmark_icon = g_themed_icon_new ("bookmark-new-symbolic");
      gtk_list_store_insert_with_values (sidebar->store, &iter, bookmarks_index,
                                         PLACES_SIDEBAR_COLUMN_ROW_TYPE, PLACES_DROP_FEEDBACK,
                                         PLACES_SIDEBAR_COLUMN_SECTION_TYPE, SECTION_BOOKMARKS,
                                         PLACES_SIDEBAR_COLUMN_GICON, new_bookmark_icon,
                                         PLACES_SIDEBAR_COLUMN_NAME, _("New bookmark"),
                                         PLACES_SIDEBAR_COLUMN_INDEX, bookmarks_index,
                                         PLACES_SIDEBAR_COLUMN_NO_EJECT, TRUE,
                                         -1);
      g_object_unref (new_bookmark_icon);
    }

  /* If the state is permanent, don't change it. Is the application that
   * controls this */
  if (sidebar->drop_state != DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT)
    sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED;

  /* Highlight the new bookmark row */
  if (path != NULL)
    {
      drop_target_index = gtk_tree_path_get_indices (path)[0];
      if (drop_target_index == bookmarks_index)
        {
          new_bookmark_path = gtk_tree_path_new_from_indices (bookmarks_index, -1);
          gtk_tree_view_set_drag_dest_row (sidebar->tree_view, new_bookmark_path, GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
          gtk_tree_path_free (new_bookmark_path);
        }
    }
}

static void
start_drop_feedback (GtkPlacesSidebar        *sidebar,
                     GtkTreePath             *path,
                     GtkTreeViewDropPosition  pos,
                     gboolean                 drop_as_bookmarks)
{
  if (drop_as_bookmarks)
    show_new_bookmark_row (sidebar, path);
  else
    gtk_tree_view_set_drag_dest_row (sidebar->tree_view, path, pos);

  update_possible_drop_targets (sidebar, TRUE);
}

static void
stop_drop_feedback (GtkPlacesSidebar *sidebar)
{
  gtk_tree_view_set_drag_dest_row (sidebar->tree_view, NULL, 0);
  update_possible_drop_targets (sidebar, FALSE);
}

static gboolean
drag_motion_callback (GtkTreeView      *tree_view,
                      GdkDragContext   *context,
                      gint              x,
                      gint              y,
                      guint             time,
                      GtkPlacesSidebar *sidebar)
{
  GtkTreePath *path;
  GtkTreeViewDropPosition pos;
  gint action;
  GtkTreeIter iter;
  gboolean res;
  gboolean drop_as_bookmarks;
  gchar *drop_target_uri = NULL;

  sidebar->dragging_over = TRUE;

  action = 0;
  drop_as_bookmarks = FALSE;
  path = NULL;

  if (!sidebar->drag_data_received)
    {
      if (!get_drag_data (tree_view, context, time))
        goto out;
    }

  res = compute_drop_position (tree_view, x, y, &path, &pos, sidebar);
  if (!res)
    goto out;

  if (sidebar->drag_data_received &&
      sidebar->drag_data_info == GTK_TREE_MODEL_ROW)
    {
      /* Dragging bookmarks always moves them to another position in the bookmarks list */
      action = GDK_ACTION_MOVE;
    }
  else
    {
      /* URIs are being dragged.  See if the caller wants to handle a
       * file move/copy operation itself, or if we should only try to
       * create bookmarks out of the dragged URIs.
       */
      if (sidebar->drag_list != NULL)
        {
          SectionType section_type;
          PlaceType place_type;

          gtk_tree_model_get_iter (GTK_TREE_MODEL (sidebar->store), &iter, path);
          gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store),
                              &iter,
                              PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                              PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                              -1);

          if (place_type == PLACES_DROP_FEEDBACK ||
              (section_type == SECTION_BOOKMARKS &&
               (pos == GTK_TREE_VIEW_DROP_BEFORE || pos == GTK_TREE_VIEW_DROP_AFTER)))
            {
              action = GDK_ACTION_COPY;
              drop_as_bookmarks = TRUE;
            }

          if (!drop_as_bookmarks)
            {
              gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store),
                                  &iter,
                                  PLACES_SIDEBAR_COLUMN_URI, &drop_target_uri,
                                  -1);

              if (drop_target_uri != NULL)
                {
                  GFile *dest_file = g_file_new_for_uri (drop_target_uri);

                  action = emit_drag_action_requested (sidebar, context, dest_file, sidebar->drag_list);

                  g_object_unref (dest_file);
                } /* uri may be NULL for unmounted volumes, for example, so we don't allow drops there */
            }
        }
    }

 out:
  if (action != 0)
    {
      check_switch_location_timer (sidebar, drop_target_uri);
      start_drop_feedback (sidebar, path, pos, drop_as_bookmarks);
    }
  else
    {
      remove_switch_location_timer (sidebar);
      stop_drop_feedback (sidebar);
    }

  g_free (drop_target_uri);

  if (path != NULL)
          gtk_tree_path_free (path);

  g_signal_stop_emission_by_name (tree_view, "drag-motion");

  gdk_drag_status (context, action, time);

  return TRUE;
}

static void
on_drag_end (GtkPlacesSidebar *sidebar)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  free_drag_data (sidebar);
  /* we could call finalize when disposing the widget */
  if (sidebar->tree_view != NULL)
    stop_drop_feedback (sidebar);
  remove_drop_bookmark_feedback_row (sidebar);

  if (sidebar->drag_leave_timeout_id)
    g_source_remove (sidebar->drag_leave_timeout_id);

  sidebar->drag_leave_timeout_id = 0;
  sidebar->drop_state = DROP_STATE_NORMAL;
  sidebar->dragging_over = FALSE;
}

static gboolean
drag_leave_timeout_cb (gpointer data)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (data);

  if (sidebar->drop_state != DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT)
    {
      on_drag_end (sidebar);
      return FALSE;
    }
  else
    {
      sidebar->dragging_over = FALSE;
      return TRUE;
    }
}

static void
drag_leave_callback (GtkTreeView      *tree_view,
                     GdkDragContext   *context,
                     guint             time,
                     GtkPlacesSidebar *sidebar)
{
  sidebar->dragging_over = FALSE;

  if (sidebar->drag_leave_timeout_id)
    g_source_remove (sidebar->drag_leave_timeout_id);

  sidebar->drag_leave_timeout_id = gdk_threads_add_timeout (500, drag_leave_timeout_cb, sidebar);
  g_source_set_name_by_id (sidebar->drag_leave_timeout_id, "[gtk+] drag_leave_timeout_cb");

  remove_switch_location_timer (sidebar);

  g_signal_stop_emission_by_name (tree_view, "drag-leave");
}

/* Takes an array of URIs and turns it into a list of GFile */
static GList *
build_file_list_from_uris (const gchar **uris)
{
  GList *result;
  gint i;

  result = NULL;
  for (i = 0; uris[i]; i++)
    {
      GFile *file;

      file = g_file_new_for_uri (uris[i]);
      result = g_list_prepend (result, file);
    }

  return g_list_reverse (result);
}

/* Reorders the selected bookmark to the specified position */
static void
reorder_bookmarks (GtkPlacesSidebar *sidebar,
                   gint              new_position)
{
  GtkTreeIter iter;
  gchar *uri;
  GFile *file;

  if (!get_selected_iter (sidebar, &iter))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store), &iter,
                      PLACES_SIDEBAR_COLUMN_URI, &uri,
                      -1);

  file = g_file_new_for_uri (uri);
  _gtk_bookmarks_manager_reorder_bookmark (sidebar->bookmarks_manager, file, new_position, NULL); /* NULL-GError */

  g_object_unref (file);
  g_free (uri);
}

/* Creates bookmarks for the specified files at the given position in the bookmarks list */
static void
drop_files_as_bookmarks (GtkPlacesSidebar *sidebar,
                         GList            *files,
                         gint              position)
{
  GList *l;

  for (l = files; l; l = l->next)
    {
      GFile *f = G_FILE (l->data);
      GFileInfo *info = g_file_query_info (f,
                                           G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           NULL,
                                           NULL);

      if (info)
        {
          if (_gtk_file_info_consider_as_directory (info))
            _gtk_bookmarks_manager_insert_bookmark (sidebar->bookmarks_manager, f, position++, NULL); /* NULL-GError */

          g_object_unref (info);
        }
    }
}

static void
drag_data_received_callback (GtkWidget        *widget,
                             GdkDragContext   *context,
                             int               x,
                             int               y,
                             GtkSelectionData *selection_data,
                             guint             info,
                             guint             time,
                             GtkPlacesSidebar *sidebar)
{
  GtkTreeView *tree_view;
  GtkTreePath *tree_path;
  GtkTreeViewDropPosition tree_pos;
  GtkTreeIter iter;
  gint position;
  GtkTreeModel *model;
  PlaceType place_type;
  SectionType section_type;
  gboolean success;

  tree_view = GTK_TREE_VIEW (widget);

  if (!sidebar->drag_data_received)
    {
      if (gtk_selection_data_get_target (selection_data) != GDK_NONE &&
          info == TEXT_URI_LIST)
        {
          gchar **uris;

          uris = gtk_selection_data_get_uris (selection_data);
          sidebar->drag_list = build_file_list_from_uris ((const char **) uris);
          g_strfreev (uris);
        }
      else
        {
          sidebar->drag_list = NULL;
        }
      sidebar->drag_data_received = TRUE;
      sidebar->drag_data_info = info;
    }

  g_signal_stop_emission_by_name (widget, "drag-data-received");

  if (!sidebar->drop_occured)
    return;

  /* Compute position */
  success = compute_drop_position (tree_view, x, y, &tree_path, &tree_pos, sidebar);
  if (!success)
    goto out;

  success = FALSE;

  if (sidebar->drag_data_info == GTK_TREE_MODEL_ROW)
    {
      /* A bookmark got reordered */

      model = gtk_tree_view_get_model (tree_view);

      if (!gtk_tree_model_get_iter (model, &iter, tree_path))
        goto out;

      gtk_tree_model_get (model, &iter,
                          PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                          PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                          PLACES_SIDEBAR_COLUMN_INDEX, &position,
                          -1);

      if (section_type != SECTION_BOOKMARKS)
        goto out;

      if (place_type == PLACES_HEADING)
        position = 0;
      else if (tree_pos == GTK_TREE_VIEW_DROP_AFTER)
        position++;

      reorder_bookmarks (sidebar, position);
      success = TRUE;
    }
  else
    {
      /* Dropping URIs! */

      GdkDragAction real_action;
      gchar **uris;
      GList *source_file_list;

      /* file transfer requested */
      real_action = gdk_drag_context_get_selected_action (context);

      if (real_action == GDK_ACTION_ASK)
        real_action = emit_drag_action_ask (sidebar, gdk_drag_context_get_actions (context));

      if (real_action > 0)
        {
          gchar *uri;
          GFile *dest_file;
          gboolean drop_as_bookmarks;

          model = gtk_tree_view_get_model (tree_view);

          gtk_tree_model_get_iter (model, &iter, tree_path);
          gtk_tree_model_get (model, &iter,
                              PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                              PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                              PLACES_SIDEBAR_COLUMN_INDEX, &position,
                              -1);

          drop_as_bookmarks = FALSE;

          uris = gtk_selection_data_get_uris (selection_data);
          source_file_list = build_file_list_from_uris ((const gchar **) uris);

          if (section_type == SECTION_BOOKMARKS)
            {
              if (place_type == PLACES_HEADING)
                {
                  position = 0;
                  tree_pos = GTK_TREE_VIEW_DROP_BEFORE;
                }

              if (tree_pos == GTK_TREE_VIEW_DROP_AFTER)
                position++;

              if (tree_pos == GTK_TREE_VIEW_DROP_BEFORE ||
                  tree_pos == GTK_TREE_VIEW_DROP_AFTER ||
                  place_type == PLACES_DROP_FEEDBACK)
                {
                  remove_drop_bookmark_feedback_row (sidebar);
                  drop_files_as_bookmarks (sidebar, source_file_list, position);
                  success = TRUE;
                  drop_as_bookmarks = TRUE;
                }
            }

          if (!drop_as_bookmarks)
            {
              gtk_tree_model_get_iter (model, &iter, tree_path);
              gtk_tree_model_get (model, &iter,
                                  PLACES_SIDEBAR_COLUMN_URI, &uri,
                                  -1);

              dest_file = g_file_new_for_uri (uri);

              emit_drag_perform_drop (sidebar, dest_file, source_file_list, real_action);
              success = TRUE;

              g_object_unref (dest_file);
              g_free (uri);
            }

          g_list_free_full (source_file_list, g_object_unref);
          g_strfreev (uris);
        }
    }

out:
  sidebar->drop_occured = FALSE;
  gtk_drag_finish (context, success, FALSE, time);
  on_drag_end (sidebar);

  gtk_tree_path_free (tree_path);
}

static gboolean
drag_drop_callback (GtkTreeView      *tree_view,
                    GdkDragContext   *context,
                    gint              x,
                    gint              y,
                    guint             time,
                    GtkPlacesSidebar *sidebar)
{
  gboolean retval = FALSE;

  sidebar->drop_occured = TRUE;
  retval = get_drag_data (tree_view, context, time);
  g_signal_stop_emission_by_name (tree_view, "drag-drop");
  return retval;
}

/* Callback used when the file list's popup menu is detached */
static void
bookmarks_popup_menu_detach_cb (GtkWidget *attach_widget,
                                GtkMenu   *menu)
{
  GTK_PLACES_SIDEBAR (attach_widget)->popup_menu = NULL;
}

static void
check_unmount_and_eject (GMount   *mount,
                         GVolume  *volume,
                         GDrive   *drive,
                         gboolean *show_unmount,
                         gboolean *show_eject)
{
  *show_unmount = FALSE;
  *show_eject = FALSE;

  if (drive != NULL)
    *show_eject = g_drive_can_eject (drive);

  if (volume != NULL)
    *show_eject |= g_volume_can_eject (volume);

  if (mount != NULL)
    {
      *show_eject |= g_mount_can_eject (mount);
      *show_unmount = g_mount_can_unmount (mount) && !*show_eject;
    }
}

static void
check_visibility (GMount   *mount,
                  GVolume  *volume,
                  GDrive   *drive,
                  gboolean *show_mount,
                  gboolean *show_unmount,
                  gboolean *show_eject,
                  gboolean *show_rescan,
                  gboolean *show_start,
                  gboolean *show_stop)
{
  *show_mount = FALSE;
  *show_rescan = FALSE;
  *show_start = FALSE;
  *show_stop = FALSE;

  check_unmount_and_eject (mount, volume, drive, show_unmount, show_eject);

  if (drive != NULL)
    {
      if (g_drive_is_media_removable (drive) &&
          !g_drive_is_media_check_automatic (drive) &&
          g_drive_can_poll_for_media (drive))
        *show_rescan = TRUE;

      *show_start = g_drive_can_start (drive) || g_drive_can_start_degraded (drive);
      *show_stop  = g_drive_can_stop (drive);

      if (*show_stop)
        *show_unmount = FALSE;
    }

  if (volume != NULL)
    {
      if (mount == NULL)
        *show_mount = g_volume_can_mount (volume);
    }
}

typedef struct {
  GtkWidget *add_shortcut_item;
  GtkWidget *remove_item;
  GtkWidget *rename_item;
  GtkWidget *separator_item;
  GtkWidget *mount_item;
  GtkWidget *unmount_item;
  GtkWidget *eject_item;
  GtkWidget *rescan_item;
  GtkWidget *start_item;
  GtkWidget *stop_item;
} PopupMenuData;

static void
check_popup_sensitivity (SidebarRow    *row,
                         PopupMenuData *data)
{
  gboolean show_mount;
  gboolean show_unmount;
  gboolean show_eject;
  gboolean show_rescan;
  gboolean show_start;
  gboolean show_stop;
  PlaceType type;
  GDrive *drive;
  GVolume *volume;
  GMount *mount;

  g_object_get (row,
                "place-type", &type,
                "drive", &drive,
                "volume", &volume,
                "mount", &mount,
                NULL);

  gtk_widget_set_visible (data->add_shortcut_item, (type == PLACES_MOUNTED_VOLUME));

  gtk_widget_set_sensitive (data->remove_item, (type == PLACES_BOOKMARK));
  gtk_widget_set_sensitive (data->rename_item, (type == PLACES_BOOKMARK ||
                                                type == PLACES_XDG_DIR));

  check_visibility (mount, volume, drive,
                    &show_mount, &show_unmount, &show_eject, &show_rescan, &show_start, &show_stop);

  gtk_widget_set_visible (data->separator_item, show_mount || show_unmount || show_eject);
  gtk_widget_set_visible (data->mount_item, show_mount);
  gtk_widget_set_visible (data->unmount_item, show_unmount);
  gtk_widget_set_visible (data->eject_item, show_eject);
  gtk_widget_set_visible (data->rescan_item, show_rescan);
  gtk_widget_set_visible (data->start_item, show_start);
  gtk_widget_set_visible (data->stop_item, show_stop);

  /* Adjust start/stop items to reflect the type of the drive */
  gtk_menu_item_set_label (GTK_MENU_ITEM (data->start_item), _("_Start"));
  gtk_menu_item_set_label (GTK_MENU_ITEM (data->stop_item), _("_Stop"));
  if ((show_start || show_stop) && drive != NULL)
    {
      switch (g_drive_get_start_stop_type (drive))
        {
        case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
          /* start() for type G_DRIVE_START_STOP_TYPE_SHUTDOWN is normally not used */
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->start_item), _("_Power On"));
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->stop_item), _("_Safely Remove Drive"));
          break;

        case G_DRIVE_START_STOP_TYPE_NETWORK:
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->start_item), _("_Connect Drive"));
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->stop_item), _("_Disconnect Drive"));
          break;

        case G_DRIVE_START_STOP_TYPE_MULTIDISK:
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->start_item), _("_Start Multi-disk Device"));
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->stop_item), _("_Stop Multi-disk Device"));
          break;

        case G_DRIVE_START_STOP_TYPE_PASSWORD:
          /* stop() for type G_DRIVE_START_STOP_TYPE_PASSWORD is normally not used */
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->start_item), _("_Unlock Drive"));
          gtk_menu_item_set_label (GTK_MENU_ITEM (data->stop_item), _("_Lock Drive"));
          break;

        default:
        case G_DRIVE_START_STOP_TYPE_UNKNOWN:
          /* uses defaults set above */
          break;
        }
    }
}

static void
drive_start_from_bookmark_cb (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = GTK_PLACES_SIDEBAR (user_data);

  error = NULL;
  if (!g_drive_poll_for_media_finish (G_DRIVE (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_drive_get_name (G_DRIVE (source_object));
          primary = g_strdup_printf (_("Unable to start “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }
}

static void
volume_mount_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (user_data);
  GVolume *volume;
  GError *error;
  gchar *primary;
  gchar *name;
  GMount *mount;

  volume = G_VOLUME (source_object);

  error = NULL;
  if (!g_volume_mount_finish (volume, result, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED &&
          error->code != G_IO_ERROR_ALREADY_MOUNTED)
        {
          name = g_volume_get_name (G_VOLUME (source_object));
          primary = g_strdup_printf (_("Unable to access “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  sidebar->mounting = FALSE;

  mount = g_volume_get_mount (volume);
  if (mount != NULL)
    {
      GFile *location;

      location = g_mount_get_default_location (mount);
      emit_open_location (sidebar, location, sidebar->go_to_after_mount_open_flags);

      g_object_unref (G_OBJECT (location));
      g_object_unref (G_OBJECT (mount));
    }

  g_object_unref (sidebar);
}

static void
mount_volume (GtkPlacesSidebar *sidebar,
              GVolume          *volume)
{
  GMountOperation *mount_op;

  mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))));
  g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);

  g_object_ref (sidebar);
  g_volume_mount (volume, 0, mount_op, NULL, volume_mount_cb, sidebar);
}

static void
open_drive (GtkPlacesSidebar   *sidebar,
            GDrive             *drive,
            GtkPlacesOpenFlags  open_flags)
{
  if (drive != NULL &&
      (g_drive_can_start (drive) || g_drive_can_start_degraded (drive)))
    {
      GMountOperation *mount_op;

      mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))));
      g_drive_start (drive, G_DRIVE_START_NONE, mount_op, NULL, drive_start_from_bookmark_cb, NULL);
      g_object_unref (mount_op);
    }
}

static void
open_volume (GtkPlacesSidebar   *sidebar,
             GVolume            *volume,
             GtkPlacesOpenFlags  open_flags)
{
  if (volume != NULL && !sidebar->mounting)
    {
      sidebar->mounting = TRUE;
      sidebar->go_to_after_mount_open_flags = open_flags;
      mount_volume (sidebar, volume);
    }
}

static void
open_uri (GtkPlacesSidebar   *sidebar,
          const gchar        *uri,
          GtkPlacesOpenFlags  open_flags)
{
  GFile *location;

  location = g_file_new_for_uri (uri);
  emit_open_location (sidebar, location, open_flags);
  g_object_unref (location);
}

static void
open_row (SidebarRow         *row,
          GtkPlacesOpenFlags  open_flags)
{
  gchar *uri;
  GDrive *drive;
  GVolume *volume;
  PlaceType place_type;
  GtkPlacesSidebar *sidebar;

  g_object_get (row,
                "sidebar", &sidebar,
                "uri", &uri,
                "place-type", &place_type,
                "drive", &drive,
                "volume", &volume,
                NULL);

  if (uri != NULL)
    {
      open_uri (sidebar, uri, open_flags);
      g_free (uri);
    }
  else if (place_type == PLACES_CONNECT_TO_SERVER)
    {
      emit_show_connect_to_server (sidebar);
    }
  else if (place_type == PLACES_ENTER_LOCATION)
    {
      emit_show_enter_location (sidebar);
    }
  else if (volume != NULL)
    {
      open_volume (sidebar, volume, open_flags);
    }
  else if (drive != NULL)
    {
      open_drive (sidebar, drive, open_flags);
    }

  g_clear_object (&volume);
  g_clear_object (&drive);

}

/* Callback used for the "Open" menu item in the context menu */
static void
open_shortcut_cb (GtkMenuItem *item,
                  SidebarRow  *row)
{
  open_row (row, GTK_PLACES_OPEN_NORMAL);
}

/* Callback used for the "Open in new tab" menu item in the context menu */
static void
open_shortcut_in_new_tab_cb (GtkMenuItem *item,
                             SidebarRow  *row)
{
  open_row (row, GTK_PLACES_OPEN_NEW_TAB);
}

/* Callback used for the "Open in new window" menu item in the context menu */
static void
open_shortcut_in_new_window_cb (GtkMenuItem *item,
                                SidebarRow  *row)
{
  open_row (row, GTK_PLACES_OPEN_NEW_WINDOW);
}

/* Add bookmark for the selected item - just used from mount points */
static void
add_shortcut_cb (GtkMenuItem *item,
                 SidebarRow  *row)
{
  gchar *uri;
  gchar *name;
  GFile *location;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "uri", &uri, "label", &name, NULL);

  if (uri == NULL)
    return;

  location = g_file_new_for_uri (uri);
  if (_gtk_bookmarks_manager_insert_bookmark (sidebar->bookmarks_manager, location, -1, NULL))
    _gtk_bookmarks_manager_set_bookmark_label (sidebar->bookmarks_manager, location, name, NULL);

  g_object_unref (location);
}

static void
rename_entry_changed (GtkEntry         *entry,
                      GtkPlacesSidebar *sidebar)
{
  PlaceType type;
  gchar *name;
  gchar *uri;
  const gchar *new_name;
  gboolean found = FALSE;
  GList *rows;
  GList *l;

  new_name = gtk_entry_get_text (GTK_ENTRY (sidebar->rename_entry));

  if (strcmp (new_name, "") == 0)
    {
      gtk_widget_set_sensitive (sidebar->rename_button, FALSE);
      gtk_label_set_label (GTK_LABEL (sidebar->rename_error), "");
      return;
    }
  rows = gtk_container_get_children (GTK_CONTAINER (sidebar->list_box));
  l = rows;
  while (l != NULL && !found)
    {
      g_object_get (SIDEBAR_ROW (l->data), "place-type", &type, "uri", &uri, "label", &name, NULL);
      if ((type == PLACES_XDG_DIR || type == PLACES_BOOKMARK) &&
          strcmp (uri, sidebar->rename_uri) != 0 &&
          strcmp (new_name, name) == 0)
        found = TRUE;

      l = l->next;
    }

  gtk_widget_set_sensitive (sidebar->rename_button, !found);
  gtk_label_set_label (GTK_LABEL (sidebar->rename_error),
                       found ? _("This name is already taken") : "");
}

static void
do_rename (GtkButton        *button,
           GtkPlacesSidebar *sidebar)
{
  const gchar *new_text;
  GFile *file;

  new_text = gtk_entry_get_text (GTK_ENTRY (sidebar->rename_entry));

  file = g_file_new_for_uri (sidebar->rename_uri);
  if (!_gtk_bookmarks_manager_has_bookmark (sidebar->bookmarks_manager, file))
    _gtk_bookmarks_manager_insert_bookmark (sidebar->bookmarks_manager, file, -1, NULL);

  _gtk_bookmarks_manager_set_bookmark_label (sidebar->bookmarks_manager, file, new_text, NULL);

  g_object_unref (file);

  g_clear_pointer (&sidebar->rename_uri, g_free);

  gtk_widget_hide (sidebar->rename_popover);
}

static void
create_rename_popover (GtkPlacesSidebar *sidebar)
{
  GtkWidget *popover;
  GtkWidget *grid;
  GtkWidget *label;
  GtkWidget *entry;
  GtkWidget *button;
  GtkWidget *error;
  gchar *str;

  if (sidebar->rename_popover)
    return;

  popover = gtk_popover_new (GTK_WIDGET (sidebar));
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_RIGHT);
  grid = gtk_grid_new ();
  gtk_container_add (GTK_CONTAINER (popover), grid);
  g_object_set (grid,
                "margin", 10,
                "row-spacing", 6,
                "column-spacing", 6,
                NULL);
  entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  g_signal_connect (entry, "changed", G_CALLBACK (rename_entry_changed), sidebar);
  str = g_strdup_printf ("<b>%s</b>", _("Name"));
  label = gtk_label_new (str);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
  g_free (str);
  button = gtk_button_new_with_mnemonic (_("_Rename"));
  gtk_widget_set_can_default (button, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (button), "suggested-action");
  g_signal_connect (button, "clicked", G_CALLBACK (do_rename), sidebar);
  error = gtk_label_new ("");
  gtk_widget_set_halign (error, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);
  gtk_grid_attach (GTK_GRID (grid), entry, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), button,1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), error, 0, 2, 2, 1);
  gtk_widget_show_all (grid);
  gtk_popover_set_default_widget (GTK_POPOVER (popover), button);

  sidebar->rename_popover = popover;
  sidebar->rename_entry = entry;
  sidebar->rename_button = button;
  sidebar->rename_error = error;
}

static void
show_rename_popover (SidebarRow *row)
{
  gchar *name;
  gchar *uri;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "label", &name, "uri", &uri, NULL);

  create_rename_popover (sidebar);

  if (sidebar->rename_uri)
    g_free (sidebar->rename_uri);
  sidebar->rename_uri = uri;

  gtk_entry_set_text (GTK_ENTRY (sidebar->rename_entry), name);
  gtk_popover_set_relative_to (GTK_POPOVER (sidebar->rename_popover), GTK_WIDGET (row));

  gtk_widget_show (sidebar->rename_popover);
  gtk_widget_grab_focus (sidebar->rename_entry);
}

static void
rename_bookmark (SidebarRow *row)
{
  PlaceType type;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "place-type", &type, NULL);

  if (type != PLACES_BOOKMARK && type != PLACES_XDG_DIR)
    return;

  show_rename_popover (row);
}

static void
rename_shortcut_cb (GtkMenuItem *item,
                    SidebarRow  *row)
{
  rename_bookmark (row);
}

static void
remove_bookmark (SidebarRow *row)
{
  PlaceType type;
  gchar *uri;
  GFile *file;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "place-type", &type, "uri", &uri, NULL);

  if (type != PLACES_BOOKMARK)
    return;

  file = g_file_new_for_uri (uri);
  _gtk_bookmarks_manager_remove_bookmark (sidebar->bookmarks_manager, file, NULL);

  g_object_unref (file);
  g_free (uri);
}

static void
remove_shortcut_cb (GtkMenuItem *item,
                    SidebarRow  *row)
{
  remove_bookmark (row);
}

static void
mount_shortcut_cb (GtkMenuItem *item,
                   SidebarRow  *row)
{
  GVolume *volume;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "volume", &volume, NULL);
  if (volume != NULL)
    mount_volume (sidebar, volume);
}

/* Callback used from g_mount_unmount_with_operation() */
static void
unmount_mount_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (user_data);
  GMount *mount;
  GError *error;

  mount = G_MOUNT (source_object);

  error = NULL;
  if (!g_mount_unmount_with_operation_finish (mount, result, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          gchar *name;
          gchar *primary;

          name = g_mount_get_name (mount);
          primary = g_strdup_printf (_("Unable to unmount “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }

      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
notify_unmount_done (GMountOperation *op,
                     const gchar *message)
{
  GApplication *application;
  gchar *notification_id;

  /* We only can support this when a default GApplication is set */
  application = g_application_get_default ();
  if (application == NULL)
    return;

  notification_id = g_strdup_printf ("gtk-mount-operation-%p", op);
  g_application_withdraw_notification (application, notification_id);

  if (message != NULL) {
    GNotification *unplug;
    GIcon *icon;
    gchar **strings;

    strings = g_strsplit (message, "\n", 0);
    icon = g_themed_icon_new ("media-removable");
    unplug = g_notification_new (strings[0]);
    g_notification_set_body (unplug, strings[1]);
    g_notification_set_icon (unplug, icon);

    g_application_send_notification (application, notification_id, unplug);
    g_object_unref (unplug);
    g_object_unref (icon);
    g_strfreev (strings);
  }

  g_free (notification_id);
}

static void
notify_unmount_show (GMountOperation *op,
                     const gchar *message)
{
  GApplication *application;
  GNotification *unmount;
  gchar *notification_id;
  GIcon *icon;
  gchar **strings;

  /* We only can support this when a default GApplication is set */
  application = g_application_get_default ();
  if (application == NULL)
    return;

  strings = g_strsplit (message, "\n", 0);
  icon = g_themed_icon_new ("media-removable");

  unmount = g_notification_new (strings[0]);
  g_notification_set_body (unmount, strings[1]);
  g_notification_set_icon (unmount, icon);
  g_notification_set_priority (unmount, G_NOTIFICATION_PRIORITY_URGENT);

  notification_id = g_strdup_printf ("gtk-mount-operation-%p", op);
  g_application_send_notification (application, notification_id, unmount);
  g_object_unref (unmount);
  g_object_unref (icon);
  g_strfreev (strings);
  g_free (notification_id);
}

static void
show_unmount_progress_cb (GMountOperation *op,
                          const gchar     *message,
                          gint64           time_left,
                          gint64           bytes_left,
                          gpointer         user_data)
{
  if (bytes_left == 0)
    notify_unmount_done (op, message);
  else
    notify_unmount_show (op, message);
}

static void
show_unmount_progress_aborted_cb (GMountOperation *op,
                                  gpointer         user_data)
{
  notify_unmount_done (op, NULL);
}

static GMountOperation *
get_unmount_operation (GtkPlacesSidebar *sidebar)
{
  GMountOperation *mount_op;

  mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))));
  g_signal_connect (mount_op, "show-unmount-progress",
                    G_CALLBACK (show_unmount_progress_cb), sidebar);
  g_signal_connect (mount_op, "aborted",
                    G_CALLBACK (show_unmount_progress_aborted_cb), sidebar);

  return mount_op;
}

/* Returns TRUE if file1 is prefix of file2 or if both files have the
 * same path
 */
static gboolean
file_prefix_or_same (GFile *file1,
                     GFile *file2)
{
  return g_file_has_prefix (file1, file2) ||
         g_file_equal (file1, file2);
}

static gboolean
is_current_location_on_volume (GtkPlacesSidebar *sidebar,
                               GMount           *mount,
                               GVolume          *volume,
                               GDrive           *drive)
{
  gboolean  current_location_on_volume;
  GFile    *mount_default_location;
  GMount   *mount_for_volume;
  GList    *volumes_for_drive;
  GList    *volume_for_drive;

  current_location_on_volume = FALSE;

  if (sidebar->current_location != NULL)
    {
      if (mount != NULL)
        {
          mount_default_location = g_mount_get_default_location (mount);
          current_location_on_volume = file_prefix_or_same (sidebar->current_location,
                                                            mount_default_location);

          g_object_unref (mount_default_location);
        }
      /* This code path is probably never reached since mount always exists,
       * and if it doesn't exists we don't offer a way to eject a volume or
       * drive in the UI. Do it for defensive programming
       */
      else if (volume != NULL)
        {
          mount_for_volume = g_volume_get_mount (volume);
          if (mount_for_volume != NULL)
            {
              mount_default_location = g_mount_get_default_location (mount_for_volume);
              current_location_on_volume = file_prefix_or_same (sidebar->current_location,
                                                                mount_default_location);

              g_object_unref (mount_default_location);
              g_object_unref (mount_for_volume);
            }
        }
      /* This code path is probably never reached since mount always exists,
       * and if it doesn't exists we don't offer a way to eject a volume or
       * drive in the UI. Do it for defensive programming
       */
      else if (drive != NULL)
        {
          volumes_for_drive = g_drive_get_volumes (drive);
          for (volume_for_drive = volumes_for_drive; volume_for_drive != NULL; volume_for_drive = volume_for_drive->next)
            {
              mount_for_volume = g_volume_get_mount (volume_for_drive->data);
              if (mount_for_volume != NULL)
                {
                  mount_default_location = g_mount_get_default_location (mount_for_volume);
                  current_location_on_volume = file_prefix_or_same (sidebar->current_location,
                                                                    mount_default_location);

                  g_object_unref (mount_default_location);
                  g_object_unref (mount_for_volume);

                  if (current_location_on_volume)
                    break;
                }
            }
          g_list_free_full (volumes_for_drive, g_object_unref);
        }
  }

  return current_location_on_volume;
}

static void
do_unmount (GMount           *mount,
            GtkPlacesSidebar *sidebar)
{
  if (mount != NULL)
    {
      GMountOperation *mount_op;

      if (is_current_location_on_volume (sidebar, mount, NULL, NULL))
        open_home (sidebar);

      mount_op = get_unmount_operation (sidebar);
      g_mount_unmount_with_operation (mount,
                                      0,
                                      mount_op,
                                      NULL,
                                      unmount_mount_cb,
                                      g_object_ref (sidebar));
      g_object_unref (mount_op);
    }
}

static void
unmount_shortcut_cb (GtkMenuItem *item,
                     SidebarRow  *row)
{
  GtkPlacesSidebar *sidebar;
  GMount *mount;

  g_object_get (row, "sidebar", &sidebar, "mount", &mount, NULL);
  do_unmount (mount, sidebar);
}

static void
drive_stop_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = user_data;

  error = NULL;
  if (!g_drive_stop_finish (G_DRIVE (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_drive_get_name (G_DRIVE (source_object));
          primary = g_strdup_printf (_("Unable to stop “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
drive_eject_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = user_data;

  error = NULL;
  if (!g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_drive_get_name (G_DRIVE (source_object));
          primary = g_strdup_printf (_("Unable to eject “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
volume_eject_cb (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = user_data;

  error = NULL;
  if (!g_volume_eject_with_operation_finish (G_VOLUME (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_volume_get_name (G_VOLUME (source_object));
          primary = g_strdup_printf (_("Unable to eject %s"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
mount_eject_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = user_data;

  error = NULL;
  if (!g_mount_eject_with_operation_finish (G_MOUNT (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_mount_get_name (G_MOUNT (source_object));
          primary = g_strdup_printf (_("Unable to eject %s"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
do_eject (GMount           *mount,
          GVolume          *volume,
          GDrive           *drive,
          GtkPlacesSidebar *sidebar)
{
  GMountOperation *mount_op;

  mount_op = get_unmount_operation (sidebar);

  if (is_current_location_on_volume (sidebar, mount, volume, drive))
    open_home (sidebar);

  if (mount != NULL)
    g_mount_eject_with_operation (mount, 0, mount_op, NULL, mount_eject_cb,
                                  g_object_ref (sidebar));
  /* This code path is probably never reached since mount always exists,
   * and if it doesn't exists we don't offer a way to eject a volume or
   * drive in the UI. Do it for defensive programming
   */
  else if (volume != NULL)
    g_volume_eject_with_operation (volume, 0, mount_op, NULL, volume_eject_cb,
                                   g_object_ref (sidebar));
  /* This code path is probably never reached since mount always exists,
   * and if it doesn't exists we don't offer a way to eject a volume or
   * drive in the UI. Do it for defensive programming
   */
  else if (drive != NULL)
    {
      if (g_drive_can_stop (drive))
        g_drive_stop (drive, 0, mount_op, NULL, drive_stop_cb,
                      g_object_ref (sidebar));
      else
        g_drive_eject_with_operation (drive, 0, mount_op, NULL, drive_eject_cb,
                                      g_object_ref (sidebar));
    }
  g_object_unref (mount_op);
}

static void
eject_shortcut_cb (GtkMenuItem *item,
                   SidebarRow  *row)
{
  GMount *mount;
  GVolume *volume;
  GDrive *drive;
  GtkPlacesSidebar *sidebar;

  g_object_get (row,
                "sidebar", &sidebar,
                "mount", &mount,
                "volume", &volume,
                "drive", &drive,
                NULL);

  do_eject (mount, volume, drive, sidebar);
}

static gboolean
eject_or_unmount_bookmark (GtkPlacesSidebar *sidebar,
                           SidebarRow       *row)
{
  gboolean can_unmount, can_eject;
  GMount *mount;
  GVolume *volume;
  GDrive *drive;
  gboolean ret;

  g_object_get (row,
                "mount", &mount,
                "volume", &volume,
                "drive", &drive,
                NULL);
  ret = FALSE;

  check_unmount_and_eject (mount, volume, drive, &can_unmount, &can_eject);
  /* if we can eject, it has priority over unmount */
  if (can_eject)
    {
      do_eject (mount, volume, drive, sidebar);
      ret = TRUE;
    }
  else if (can_unmount)
    {
      do_unmount (mount, sidebar);
      ret = TRUE;
    }

  return ret;
}

static gboolean
eject_or_unmount_selection (GtkPlacesSidebar *sidebar)
{
  gboolean ret;
  GtkListBoxRow *row;

  row = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
  ret = eject_or_unmount_bookmark (sidebar, SIDEBAR_ROW (row));

  return ret;
}

static void
drive_poll_for_media_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = GTK_PLACES_SIDEBAR (user_data);

  error = NULL;
  if (!g_drive_poll_for_media_finish (G_DRIVE (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_drive_get_name (G_DRIVE (source_object));
          primary = g_strdup_printf (_("Unable to poll “%s” for media changes"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
rescan_shortcut_cb (GtkMenuItem *item,
                    SidebarRow  *row)
{
  GtkPlacesSidebar *sidebar;
  GDrive  *drive;

  g_object_get (row, "sidebar", &sidebar, "drive", &drive, NULL);

  if (drive != NULL)
    {
      g_drive_poll_for_media (drive, NULL, drive_poll_for_media_cb, g_object_ref (sidebar));
      g_object_unref (drive);
    }
}

static void
drive_start_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  GtkPlacesSidebar *sidebar;
  GError *error;
  gchar *primary;
  gchar *name;

  sidebar = GTK_PLACES_SIDEBAR (user_data);

  error = NULL;
  if (!g_drive_start_finish (G_DRIVE (source_object), res, &error))
    {
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          name = g_drive_get_name (G_DRIVE (source_object));
          primary = g_strdup_printf (_("Unable to start “%s”"), name);
          g_free (name);
          emit_show_error_message (sidebar, primary, error->message);
          g_free (primary);
        }
      g_error_free (error);
    }

  g_object_unref (sidebar);
}

static void
start_shortcut_cb (GtkMenuItem *item,
                   SidebarRow  *row)
{
  GtkPlacesSidebar *sidebar;
  GDrive  *drive;

  g_object_get (row, "sidebar", &sidebar, "drive", &drive, NULL);

  if (drive != NULL)
    {
      GMountOperation *mount_op;

      mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))));

      g_drive_start (drive, G_DRIVE_START_NONE, mount_op, NULL, drive_start_cb, g_object_ref (sidebar));

      g_object_unref (mount_op);
      g_object_unref (drive);
    }
}

static void
stop_shortcut_cb (GtkMenuItem *item,
                  SidebarRow  *row)
{
  GtkPlacesSidebar *sidebar;
  GDrive  *drive;

  g_object_get (row, "sidebar", &sidebar, "drive", &drive, NULL);

  if (drive != NULL)
    {
      GMountOperation *mount_op;

      mount_op = get_unmount_operation (sidebar);
      g_drive_stop (drive, G_MOUNT_UNMOUNT_NONE, mount_op, NULL, drive_stop_cb,
                    g_object_ref (sidebar));

      g_object_unref (mount_op);
      g_object_unref (drive);
    }
}

static gboolean
on_key_press_event (GtkWidget        *widget,
                    GdkEventKey      *event,
                    GtkPlacesSidebar *sidebar)
{
  guint modifiers;
  GtkListBoxRow *row;
  gchar *label;

  if (event)
    {
      row = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
      if (row)
        {
          g_object_get (row, "label", &label, NULL);
          g_print ("row %s\n", label);
          modifiers = gtk_accelerator_get_default_mod_mask ();

          if (event->keyval == GDK_KEY_Return ||
              event->keyval == GDK_KEY_KP_Enter ||
              event->keyval == GDK_KEY_ISO_Enter ||
              event->keyval == GDK_KEY_space)
            {
              GtkPlacesOpenFlags open_flags = GTK_PLACES_OPEN_NORMAL;

              if ((event->state & modifiers) == GDK_SHIFT_MASK)
                open_flags = GTK_PLACES_OPEN_NEW_TAB;
              else if ((event->state & modifiers) == GDK_CONTROL_MASK)
                open_flags = GTK_PLACES_OPEN_NEW_WINDOW;

              open_row (SIDEBAR_ROW (row), open_flags);

              return TRUE;
            }

          if (event->keyval == GDK_KEY_Down &&
              (event->state & modifiers) == GDK_MOD1_MASK)
            return eject_or_unmount_selection (sidebar);

          if (event->keyval == GDK_KEY_Up)
            {
              gint index;
              GtkListBoxRow *previous_row;

              index = gtk_list_box_row_get_index (row);
              previous_row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (sidebar->list_box),
                                                            index - 1);
              if (previous_row != NULL)
                gtk_list_box_select_row (GTK_LIST_BOX (sidebar->list_box), previous_row);

              return TRUE;
            }

          if (event->keyval == GDK_KEY_Down)
            {
              gint index;
              GtkListBoxRow *previous_row;

              index = gtk_list_box_row_get_index (row);
              previous_row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (sidebar->list_box),
                                                            index + 1);
              if (previous_row != NULL)
                gtk_list_box_select_row (GTK_LIST_BOX (sidebar->list_box), previous_row);

              return TRUE;
            }

          if ((event->keyval == GDK_KEY_Delete ||
               event->keyval == GDK_KEY_KP_Delete) &&
              (event->state & modifiers) == 0)
            {
              remove_bookmark (SIDEBAR_ROW (row));
              return TRUE;
            }

          if ((event->keyval == GDK_KEY_F2) &&
              (event->state & modifiers) == 0)
            {
              rename_bookmark (SIDEBAR_ROW (row));
              return TRUE;
            }
        }
    }

  g_print ("press event\n");


  return FALSE;
}

static GtkMenuItem *
append_menu_separator (GtkMenu *menu)
{
  GtkWidget *menu_item;

  menu_item = gtk_separator_menu_item_new ();
  gtk_widget_show (menu_item);
  gtk_menu_shell_insert (GTK_MENU_SHELL (menu), menu_item, -1);

  return GTK_MENU_ITEM (menu_item);
}

/* Constructs the popup menu for the file list if needed */
static void
bookmarks_build_popup_menu (SidebarRow *row)
{
  PopupMenuData menu_data;
  GtkWidget *item;
  GFile *file;
  const gchar *uri;
  GVolume *volume;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, "uri", &uri, "volume", &volume, NULL);

  sidebar->popup_menu = gtk_menu_new ();
  gtk_style_context_add_class (gtk_widget_get_style_context (sidebar->popup_menu),
                               GTK_STYLE_CLASS_CONTEXT_MENU);

  gtk_menu_attach_to_widget (GTK_MENU (sidebar->popup_menu),
                             GTK_WIDGET (sidebar),
                             bookmarks_popup_menu_detach_cb);

  item = gtk_menu_item_new_with_mnemonic (_("_Open"));
  g_signal_connect (item, "activate",
                    G_CALLBACK (open_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  if (sidebar->open_flags & GTK_PLACES_OPEN_NEW_TAB)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Tab"));
      g_signal_connect (item, "activate",
                        G_CALLBACK (open_shortcut_in_new_tab_cb), row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);
    }

  if (sidebar->open_flags & GTK_PLACES_OPEN_NEW_WINDOW)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Window"));
      g_signal_connect (item, "activate",
                        G_CALLBACK (open_shortcut_in_new_window_cb), row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);
    }

  append_menu_separator (GTK_MENU (sidebar->popup_menu));

  item = gtk_menu_item_new_with_mnemonic (_("_Add Bookmark"));
  menu_data.add_shortcut_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (add_shortcut_cb), row);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_label (_("Remove"));
  menu_data.remove_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (remove_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_label (_("Rename…"));
  menu_data.rename_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (rename_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  /* Mount/Unmount/Eject menu items */

  menu_data.separator_item = GTK_WIDGET (append_menu_separator (GTK_MENU (sidebar->popup_menu)));

  item = gtk_menu_item_new_with_mnemonic (_("_Mount"));
  menu_data.mount_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (mount_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_mnemonic (_("_Unmount"));
  menu_data.unmount_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (unmount_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_mnemonic (_("_Eject"));
  menu_data.eject_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (eject_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_mnemonic (_("_Detect Media"));
  menu_data.rescan_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (rescan_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_mnemonic (_("_Start"));
  menu_data.start_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (start_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  item = gtk_menu_item_new_with_mnemonic (_("_Stop"));
  menu_data.stop_item = item;
  g_signal_connect (item, "activate",
                    G_CALLBACK (stop_shortcut_cb), row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

  /* Update everything! */

  check_popup_sensitivity (row, &menu_data);

  /* And let the caller spice things up */

  if (uri)
    file = g_file_new_for_uri (uri);
  else
    file = NULL;

  emit_populate_popup (sidebar, GTK_MENU (sidebar->popup_menu), file, volume);

  if (file)
    g_object_unref (file);
}

static void
bookmarks_popup_menu (SidebarRow     *row,
                      GdkEventButton *event)
{
  gint button;
  GtkPlacesSidebar *sidebar;

  g_object_get (row, "sidebar", &sidebar, NULL);

  if (sidebar->popup_menu)
    gtk_widget_destroy (sidebar->popup_menu);

  bookmarks_build_popup_menu (row);

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

  gtk_menu_popup (GTK_MENU (sidebar->popup_menu),
                  NULL,
                  NULL,
                  NULL,
                  NULL,
                  button,
                  event ? event->time : gtk_get_current_event_time ());
}

static void
on_row_activated (GtkListBox    *list_box,
                  GtkListBoxRow *row,
                  gpointer       user_data)
{
  SidebarRow *selected_row;

  selected_row = SIDEBAR_ROW (gtk_list_box_get_selected_row (list_box));
  open_row (selected_row, 0);
}

static gboolean
on_button_release_event (GtkWidget        *widget,
                         GdkEventButton   *event,
                         GtkPlacesSidebar *sidebar)
{
  gboolean ret = FALSE;
  GtkListBoxRow *row;
  gchar *label;

  if (event)
    {
      row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (sidebar->list_box),
                                       (gint) event->y);
      if (row)
        {
          g_object_get (row, "label", &label, NULL);
          g_print ("row %s\n", label);
          if (event->button == 1)
            return FALSE;

          if (event->button == 2)
            {
              GtkPlacesOpenFlags open_flags = GTK_PLACES_OPEN_NORMAL;

              open_flags = (event->state & GDK_CONTROL_MASK) ?
                            GTK_PLACES_OPEN_NEW_WINDOW :
                            GTK_PLACES_OPEN_NEW_TAB;

              open_row (SIDEBAR_ROW (row), open_flags);
              ret = TRUE;
            }
          else if (event->button == 3)
            {
              PlaceType row_type;

              g_object_get (row, "place-type", &row_type, NULL);

              if (row_type != PLACES_CONNECT_TO_SERVER)
                bookmarks_popup_menu (SIDEBAR_ROW (row), event);
            }
        }
    }

  return ret;
}

static gboolean
tree_selection_func (GtkTreeSelection *selection,
                     GtkTreeModel     *model,
                     GtkTreePath      *path,
                     gboolean          path_currently_selected,
                     gpointer          user_data)
{
  GtkTreeIter iter;
  PlaceType row_type;

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &row_type,
                      -1);

  if (row_type == PLACES_HEADING)
    return FALSE;

  return TRUE;
}

static int
bookmarks_get_first_index (GtkPlacesSidebar *sidebar)
{
  GtkTreeIter iter;

  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), -1);

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (sidebar->store), &iter))
    {
      int bookmarks_heading_index = 0;

      do
        {
          PlaceType place_type;
          SectionType section_type;

          gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store), &iter,
                              PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                              PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                              -1);
          if (place_type == PLACES_HEADING && section_type == SECTION_BOOKMARKS)
              return ++bookmarks_heading_index;

          bookmarks_heading_index++;
        }
      while (gtk_tree_model_iter_next (GTK_TREE_MODEL (sidebar->store), &iter));
    }

  g_warning ("Index of bookmarks not calculable. No bookmarks heading");

  return -1;
}

static void
icon_cell_renderer_func (GtkTreeViewColumn *column,
                         GtkCellRenderer   *cell,
                         GtkTreeModel      *model,
                         GtkTreeIter       *iter,
                         gpointer           user_data)
{
  PlaceType type;

  gtk_tree_model_get (model, iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type,
                      -1);

  g_object_set (cell, "visible", type != PLACES_HEADING, NULL);
}

static gint
places_sidebar_sort_func (GtkTreeModel *model,
                          GtkTreeIter  *iter_a,
                          GtkTreeIter  *iter_b,
                          gpointer      user_data)
{
  SectionType section_type_a, section_type_b;
  PlaceType place_type_a, place_type_b;
  gint retval = 0;

  gtk_tree_model_get (model, iter_a,
                      PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type_a,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type_a,
                      -1);
  gtk_tree_model_get (model, iter_b,
                      PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type_b,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type_b,
                      -1);

  /* fall back to the default order if we're not in the
   * XDG part of the computer section.
   */
  if ((section_type_a == section_type_b) &&
      (section_type_a == SECTION_COMPUTER) &&
      (place_type_a == place_type_b) &&
      (place_type_a == PLACES_XDG_DIR))
    {
      gchar *name_a, *name_b;

      gtk_tree_model_get (model, iter_a,
                          PLACES_SIDEBAR_COLUMN_NAME, &name_a,
                          -1);
      gtk_tree_model_get (model, iter_b,
                          PLACES_SIDEBAR_COLUMN_NAME, &name_b,
                          -1);

      retval = g_utf8_collate (name_a, name_b);

      g_free (name_a);
      g_free (name_b);
    }
  else if ((place_type_a == place_type_b) &&
           (place_type_a == PLACES_BOOKMARK))
    {
      gint pos_a, pos_b;

      gtk_tree_model_get (model, iter_a,
                          PLACES_SIDEBAR_COLUMN_INDEX, &pos_a,
                          -1);
      gtk_tree_model_get (model, iter_b,
                          PLACES_SIDEBAR_COLUMN_INDEX, &pos_b,
                          -1);

      retval = pos_a - pos_b;
    }
  else if (place_type_a == PLACES_CONNECT_TO_SERVER)
    {
      retval = 1;
    }
  else if (place_type_b == PLACES_CONNECT_TO_SERVER)
    {
      retval = -1;
    }

  return retval;
}

static void
update_hostname (GtkPlacesSidebar *sidebar)
{
  GVariant *variant;
  gsize len;
  const gchar *hostname;

  if (sidebar->hostnamed_proxy == NULL)
    return;

  variant = g_dbus_proxy_get_cached_property (sidebar->hostnamed_proxy,
                                              "PrettyHostname");
  if (variant == NULL)
    return;

  hostname = g_variant_get_string (variant, &len);
  if (len > 0 &&
      g_strcmp0 (sidebar->hostname, hostname) != 0)
    {
      g_free (sidebar->hostname);
      sidebar->hostname = g_strdup (hostname);
      update_places (sidebar);
    }

  g_variant_unref (variant);
}

static void
hostname_proxy_new_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GtkPlacesSidebar *sidebar = user_data;
  GError *error = NULL;
  GDBusProxy *proxy;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      return;
    }

  sidebar->hostnamed_proxy = proxy;
  g_clear_object (&sidebar->hostnamed_cancellable);

  if (error != NULL)
    {
      g_debug ("Failed to create D-Bus proxy: %s", error->message);
      g_error_free (error);
      return;
    }

  g_signal_connect_swapped (sidebar->hostnamed_proxy,
                            "g-properties-changed",
                            G_CALLBACK (update_hostname),
                            sidebar);
  update_hostname (sidebar);
}

static void
create_volume_monitor (GtkPlacesSidebar *sidebar)
{
  g_assert (sidebar->volume_monitor == NULL);

  sidebar->volume_monitor = g_volume_monitor_get ();

  g_signal_connect_object (sidebar->volume_monitor, "volume_added",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "volume_removed",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "volume_changed",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "mount_added",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "mount_removed",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "mount_changed",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "drive_disconnected",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "drive_connected",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
  g_signal_connect_object (sidebar->volume_monitor, "drive_changed",
                           G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
}

static void
shell_shows_desktop_changed (GtkSettings *settings,
                             GParamSpec  *pspec,
                             gpointer     user_data)
{
  GtkPlacesSidebar *sidebar = user_data;
  gboolean b;

  g_assert (settings == sidebar->gtk_settings);

  /* Check if the user explicitly set this and, if so, don't change it. */
  if (sidebar->show_desktop_set)
    return;

  g_object_get (settings, "gtk-shell-shows-desktop", &b, NULL);

  if (b != sidebar->show_desktop)
    {
      sidebar->show_desktop = b;
      update_places (sidebar);
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_SHOW_DESKTOP]);
    }
}

static gboolean
row_separator_func (GtkTreeModel *model,
                    GtkTreeIter *iter,
                    gpointer data)
{
  PlaceType type;

  gtk_tree_model_get (model, iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type,
                      -1);

  return type == PLACES_HEADING;
}

static void
gtk_places_sidebar_init (GtkPlacesSidebar *sidebar)
{
  GtkTreeView       *tree_view;
  GtkTreeViewColumn *col;
  GtkCellRenderer   *cell;
  GtkTreeSelection  *selection;
  GIcon             *eject;
  GtkTargetList     *target_list;
  gboolean           b;

  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (sidebar)), GTK_STYLE_CLASS_SIDEBAR);

  sidebar->cancellable = g_cancellable_new ();

  create_volume_monitor (sidebar);

  sidebar->open_flags = GTK_PLACES_OPEN_NORMAL;

  sidebar->bookmarks_manager = _gtk_bookmarks_manager_new ((GtkBookmarksChangedFunc)update_places, sidebar);

  sidebar->trash_monitor = _gtk_trash_monitor_get ();
  sidebar->trash_monitor_changed_id = g_signal_connect_swapped (sidebar->trash_monitor, "trash-state-changed",
                                                                G_CALLBACK (update_places), sidebar);

  sidebar->shortcuts = NULL;

  gtk_widget_set_size_request (GTK_WIDGET (sidebar), 140, 280);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (sidebar), NULL);
  gtk_scrolled_window_set_vadjustment (GTK_SCROLLED_WINDOW (sidebar), NULL);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sidebar), GTK_SHADOW_IN);

  gtk_style_context_set_junction_sides (gtk_widget_get_style_context (GTK_WIDGET (sidebar)),
                                        GTK_JUNCTION_RIGHT | GTK_JUNCTION_LEFT);

  /* list box */
  sidebar->list_box = gtk_list_box_new ();
  gtk_list_box_set_header_func (GTK_LIST_BOX (sidebar->list_box),
                                add_separator, sidebar, NULL);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (sidebar->list_box),
                                   GTK_SELECTION_BROWSE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (sidebar->list_box),
                                             TRUE);
  g_signal_connect (sidebar->list_box,
                    "row-activated",
                    (GCallback) on_row_activated,
                    sidebar);
  g_signal_connect (sidebar->list_box,
                    "button-release-event",
                    G_CALLBACK (on_button_release_event),
                    sidebar);
  g_signal_connect (sidebar->list_box,
                    "key-press-event",
                    G_CALLBACK (on_key_press_event),
                    sidebar);

  gtk_container_add (GTK_CONTAINER (sidebar), sidebar->list_box);
  gtk_widget_show (GTK_WIDGET (sidebar));

  /* tree view */
  tree_view = GTK_TREE_VIEW (gtk_tree_view_new ());
  gtk_tree_view_set_headers_visible (tree_view, FALSE);

  gtk_tree_view_set_row_separator_func (tree_view,
                                        row_separator_func,
                                        sidebar,
                                        NULL);

  col = gtk_tree_view_column_new ();

  /* icon renderer */
  cell = gtk_cell_renderer_pixbuf_new ();
  g_object_set (cell,
                "xpad", 10,
                "ypad", 8,
                NULL);
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_attributes (col, cell,
                                       "gicon", PLACES_SIDEBAR_COLUMN_GICON,
                                       "sensitive", PLACES_SIDEBAR_COLUMN_SENSITIVE,
                                       NULL);
  gtk_tree_view_column_set_cell_data_func (col, cell,
                                           icon_cell_renderer_func,
                                           sidebar, NULL);

  /* eject text renderer */
  cell = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (col, cell, TRUE);
  gtk_tree_view_column_set_attributes (col, cell,
                                       "text", PLACES_SIDEBAR_COLUMN_NAME,
                                       "visible", PLACES_SIDEBAR_COLUMN_EJECT,
                                       NULL);
  g_object_set (cell,
                "ellipsize", PANGO_ELLIPSIZE_END,
                "ellipsize-set", TRUE,
                NULL);

  /* eject icon renderer */
  cell = gtk_cell_renderer_pixbuf_new ();
  sidebar->eject_icon_cell_renderer = cell;
  eject = g_themed_icon_new_with_default_fallbacks ("media-eject-symbolic");
  g_object_set (cell,
                "mode", GTK_CELL_RENDERER_MODE_ACTIVATABLE,
                "stock-size", GTK_ICON_SIZE_MENU,
                "xpad", EJECT_BUTTON_XPAD,
                /* align right, because for some reason gtk+ expands
                   this even though we tell it not to. */
                "xalign", 1.0,
                "gicon", eject,
                NULL);
  gtk_tree_view_column_pack_start (col, cell, FALSE);
  gtk_tree_view_column_set_attributes (col, cell,
                                       "visible", PLACES_SIDEBAR_COLUMN_EJECT,
                                       NULL);
  g_object_unref (eject);

  /* normal text renderer */
  cell = gtk_cell_renderer_text_new ();
  sidebar->text_cell_renderer = cell;
  gtk_tree_view_column_pack_start (col, cell, TRUE);
  g_object_set (G_OBJECT (cell), "editable", FALSE, NULL);
  gtk_tree_view_column_set_attributes (col, cell,
                                       "text", PLACES_SIDEBAR_COLUMN_NAME,
                                       "visible", PLACES_SIDEBAR_COLUMN_NO_EJECT,
                                       "editable-set", PLACES_SIDEBAR_COLUMN_BOOKMARK,
                                       "sensitive", PLACES_SIDEBAR_COLUMN_SENSITIVE,
                                       NULL);
  g_object_set (cell,
                "ellipsize", PANGO_ELLIPSIZE_END,
                "ellipsize-set", TRUE,
                NULL);

  /* this is required to align the eject buttons to the right */
  gtk_tree_view_column_set_max_width (GTK_TREE_VIEW_COLUMN (col), 24);
  gtk_tree_view_append_column (tree_view, col);

  sidebar->store = shortcuts_model_new (sidebar);
  gtk_tree_view_set_tooltip_column (tree_view, PLACES_SIDEBAR_COLUMN_TOOLTIP);

  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (sidebar->store),
                                        PLACES_SIDEBAR_COLUMN_NAME,
                                        GTK_SORT_ASCENDING);
  gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (sidebar->store),
                                   PLACES_SIDEBAR_COLUMN_NAME,
                                   places_sidebar_sort_func,
                                   sidebar, NULL);

  gtk_tree_view_set_model (tree_view, GTK_TREE_MODEL (sidebar->store));
  gtk_widget_show (GTK_WIDGET (tree_view));
  gtk_tree_view_set_enable_search (tree_view, FALSE);

  gtk_widget_show (GTK_WIDGET (sidebar));
  sidebar->tree_view = tree_view;

  gtk_tree_view_set_search_column (tree_view, PLACES_SIDEBAR_COLUMN_NAME);
  selection = gtk_tree_view_get_selection (tree_view);
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);

  gtk_tree_selection_set_select_function (selection,
                                          tree_selection_func,
                                          sidebar,
                                          NULL);

  gtk_tree_view_enable_model_drag_source (GTK_TREE_VIEW (tree_view),
                                          GDK_BUTTON1_MASK,
                                          dnd_source_targets, G_N_ELEMENTS (dnd_source_targets),
                                          GDK_ACTION_MOVE);
  gtk_drag_dest_set (GTK_WIDGET (tree_view),
                     0,
                     NULL, 0,
                     GDK_ACTION_MOVE | GDK_ACTION_COPY | GDK_ACTION_LINK);
  target_list = gtk_target_list_new  (dnd_drop_targets, G_N_ELEMENTS (dnd_drop_targets));
  gtk_target_list_add_uri_targets (target_list, TEXT_URI_LIST);
  gtk_drag_dest_set_target_list (GTK_WIDGET (tree_view), target_list);
  gtk_target_list_unref (target_list);

  g_signal_connect (tree_view, "drag-motion",
                    G_CALLBACK (drag_motion_callback), sidebar);
  g_signal_connect (tree_view, "drag-leave",
                    G_CALLBACK (drag_leave_callback), sidebar);
  g_signal_connect (tree_view, "drag-data-received",
                    G_CALLBACK (drag_data_received_callback), sidebar);
  g_signal_connect (tree_view, "drag-drop",
                    G_CALLBACK (drag_drop_callback), sidebar);

  gtk_tree_view_set_activate_on_single_click (sidebar->tree_view, TRUE);

  sidebar->hostname = g_strdup (_("Computer"));
  sidebar->hostnamed_cancellable = g_cancellable_new ();
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                            G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                            NULL,
                            "org.freedesktop.hostname1",
                            "/org/freedesktop/hostname1",
                            "org.freedesktop.hostname1",
                            sidebar->hostnamed_cancellable,
                            hostname_proxy_new_cb,
                            sidebar);

  sidebar->drop_state = DROP_STATE_NORMAL;

  /* Don't bother trying to trace this across hierarchy changes... */
  sidebar->gtk_settings = gtk_settings_get_default ();
  g_signal_connect (sidebar->gtk_settings, "notify::gtk-shell-shows-desktop",
                    G_CALLBACK (shell_shows_desktop_changed), sidebar);
  g_object_get (sidebar->gtk_settings, "gtk-shell-shows-desktop", &b, NULL);
  sidebar->show_desktop = b;

  /* populate the sidebar */
  update_places (sidebar);
}

static void
gtk_places_sidebar_set_property (GObject      *obj,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (obj);

  switch (property_id)
    {
    case PROP_LOCATION:
      gtk_places_sidebar_set_location (sidebar, g_value_get_object (value));
      break;

    case PROP_OPEN_FLAGS:
      gtk_places_sidebar_set_open_flags (sidebar, g_value_get_flags (value));
      break;

    case PROP_SHOW_DESKTOP:
      gtk_places_sidebar_set_show_desktop (sidebar, g_value_get_boolean (value));
      break;

    case PROP_SHOW_CONNECT_TO_SERVER:
      gtk_places_sidebar_set_show_connect_to_server (sidebar, g_value_get_boolean (value));
      break;

    case PROP_SHOW_ENTER_LOCATION:
      gtk_places_sidebar_set_show_enter_location (sidebar, g_value_get_boolean (value));
      break;

    case PROP_LOCAL_ONLY:
      gtk_places_sidebar_set_local_only (sidebar, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, property_id, pspec);
      break;
    }
}

static void
gtk_places_sidebar_get_property (GObject    *obj,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GtkPlacesSidebar *sidebar = GTK_PLACES_SIDEBAR (obj);

  switch (property_id)
    {
    case PROP_LOCATION:
      g_value_take_object (value, gtk_places_sidebar_get_location (sidebar));
      break;

    case PROP_OPEN_FLAGS:
      g_value_set_flags (value, gtk_places_sidebar_get_open_flags (sidebar));
      break;

    case PROP_SHOW_DESKTOP:
      g_value_set_boolean (value, gtk_places_sidebar_get_show_desktop (sidebar));
      break;

    case PROP_SHOW_CONNECT_TO_SERVER:
      g_value_set_boolean (value, gtk_places_sidebar_get_show_connect_to_server (sidebar));
      break;

    case PROP_SHOW_ENTER_LOCATION:
      g_value_set_boolean (value, gtk_places_sidebar_get_show_enter_location (sidebar));
      break;

    case PROP_LOCAL_ONLY:
      g_value_set_boolean (value, gtk_places_sidebar_get_local_only (sidebar));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, property_id, pspec);
      break;
    }
}

static void
gtk_places_sidebar_dispose (GObject *object)
{
  GtkPlacesSidebar *sidebar;

  sidebar = GTK_PLACES_SIDEBAR (object);

  if (sidebar->cancellable)
    {
      g_cancellable_cancel (sidebar->cancellable);
      g_object_unref (sidebar->cancellable);
      sidebar->cancellable = NULL;
    }

  on_drag_end (sidebar);

  sidebar->tree_view = NULL;

  if (sidebar->bookmarks_manager != NULL)
    {
      _gtk_bookmarks_manager_free (sidebar->bookmarks_manager);
      sidebar->bookmarks_manager = NULL;
    }

  if (sidebar->popup_menu)
    {
      gtk_widget_destroy (sidebar->popup_menu);
      sidebar->popup_menu = NULL;
    }

  if (sidebar->trash_monitor)
    {
      g_signal_handler_disconnect (sidebar->trash_monitor, sidebar->trash_monitor_changed_id);
      sidebar->trash_monitor_changed_id = 0;
      g_clear_object (&sidebar->trash_monitor);
    }

  g_clear_object (&sidebar->store);

  g_slist_free_full (sidebar->shortcuts, g_object_unref);
  sidebar->shortcuts = NULL;

  if (sidebar->volume_monitor != NULL)
    {
      g_signal_handlers_disconnect_by_func (sidebar->volume_monitor,
                                            update_places, sidebar);
      g_clear_object (&sidebar->volume_monitor);
    }

  if (sidebar->hostnamed_cancellable != NULL)
    {
      g_cancellable_cancel (sidebar->hostnamed_cancellable);
      g_clear_object (&sidebar->hostnamed_cancellable);
    }

  g_clear_object (&sidebar->hostnamed_proxy);
  g_free (sidebar->hostname);
  sidebar->hostname = NULL;

  if (sidebar->gtk_settings)
    {
      g_signal_handlers_disconnect_by_func (sidebar->gtk_settings, shell_shows_desktop_changed, sidebar);
      sidebar->gtk_settings = NULL;
    }

  g_clear_object (&sidebar->current_location);
  g_clear_pointer (&sidebar->rename_uri, g_free);

  G_OBJECT_CLASS (gtk_places_sidebar_parent_class)->dispose (object);
}

static void
gtk_places_sidebar_class_init (GtkPlacesSidebarClass *class)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) class;

  gobject_class->dispose = gtk_places_sidebar_dispose;
  gobject_class->set_property = gtk_places_sidebar_set_property;
  gobject_class->get_property = gtk_places_sidebar_get_property;

  /**
   * GtkPlacesSidebar::open-location:
   * @sidebar: the object which received the signal.
   * @location: (type Gio.File): #GFile to which the caller should switch.
   * @open_flags: a single value from #GtkPlacesOpenFlags specifying how the @location should be opened.
   *
   * The places sidebar emits this signal when the user selects a location
   * in it.  The calling application should display the contents of that
   * location; for example, a file manager should show a list of files in
   * the specified location.
   *
   * Since: 3.10
   */
  places_sidebar_signals [OPEN_LOCATION] =
          g_signal_new (I_("open-location"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, open_location),
                        NULL, NULL,
                        _gtk_marshal_VOID__OBJECT_FLAGS,
                        G_TYPE_NONE, 2,
                        G_TYPE_OBJECT,
                        GTK_TYPE_PLACES_OPEN_FLAGS);

  /**
   * GtkPlacesSidebar::populate-popup:
   * @sidebar: the object which received the signal.
   * @menu: (type Gtk.Menu): a #GtkMenu.
   * @selected_item: (type Gio.File) (nullable): #GFile with the item to which the menu should refer, or #NULL in the case of a @selected_volume.
   * @selected_volume: (type Gio.Volume) (nullable): #GVolume if the selected item is a volume, or #NULL if it is a file.
   *
   * The places sidebar emits this signal when the user invokes a contextual
   * menu on one of its items.  In the signal handler, the application may
   * add extra items to the menu as appropriate.  For example, a file manager
   * may want to add a "Properties" command to the menu.
   *
   * It is not necessary to store the @selected_item for each menu item;
   * during their GtkMenuItem::activate callbacks, the application can use
   * gtk_places_sidebar_get_location() to get the file to which the item
   * refers.
   *
   * The @selected_item argument may be #NULL in case the selection refers to
   * a volume.  In this case, @selected_volume will be non-NULL.  In this case,
   * the calling application will have to g_object_ref() the @selected_volume and
   * keep it around for the purposes of its menu item's "activate" callback.
   *
   * The @menu and all its menu items are destroyed after the user
   * dismisses the menu.  The menu is re-created (and thus, this signal is
   * emitted) every time the user activates the contextual menu.
   *
   * Since: 3.10
   */
  places_sidebar_signals [POPULATE_POPUP] =
          g_signal_new (I_("populate-popup"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, populate_popup),
                        NULL, NULL,
                        _gtk_marshal_VOID__OBJECT_OBJECT_OBJECT,
                        G_TYPE_NONE, 3,
                        G_TYPE_OBJECT,
                        G_TYPE_OBJECT,
                        G_TYPE_OBJECT);

  /**
   * GtkPlacesSidebar::show-error-message:
   * @sidebar: the object which received the signal.
   * @primary: primary message with a summary of the error to show.
   * @secondary: secondary message with details of the error to show.
   *
   * The places sidebar emits this signal when it needs the calling
   * application to present an error message.  Most of these messages
   * refer to mounting or unmounting media, for example, when a drive
   * cannot be started for some reason.
   *
   * Since: 3.10
   */
  places_sidebar_signals [SHOW_ERROR_MESSAGE] =
          g_signal_new (I_("show-error-message"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, show_error_message),
                        NULL, NULL,
                        _gtk_marshal_VOID__STRING_STRING,
                        G_TYPE_NONE, 2,
                        G_TYPE_STRING,
                        G_TYPE_STRING);

  /**
   * GtkPlacesSidebar::show-connect-to-server:
   * @sidebar: the object which received the signal.
   *
   * The places sidebar emits this signal when it needs the calling
   * application to present an way to connect directly to a network server.
   * For example, the application may bring up a dialog box asking for
   * a URL like "sftp://ftp.example.com".  It is up to the application to create
   * the corresponding mount by using, for example, g_file_mount_enclosing_volume().
   *
   * Since: 3.10
   */
  places_sidebar_signals [SHOW_CONNECT_TO_SERVER] =
          g_signal_new (I_("show-connect-to-server"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, show_connect_to_server),
                        NULL, NULL,
                        _gtk_marshal_VOID__VOID,
                        G_TYPE_NONE, 0);

  /**
   * GtkPlacesSidebar::show-enter-location:
   * @sidebar: the object which received the signal.
   *
   * The places sidebar emits this signal when it needs the calling
   * application to present an way to directly enter a location.
   * For example, the application may bring up a dialog box asking for
   * a URL like "http://http.example.com".
   *
   * Since: 3.14
   */
  places_sidebar_signals [SHOW_ENTER_LOCATION] =
          g_signal_new (I_("show-enter-location"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, show_enter_location),
                        NULL, NULL,
                        _gtk_marshal_VOID__VOID,
                        G_TYPE_NONE, 0);

  /**
   * GtkPlacesSidebar::drag-action-requested:
   * @sidebar: the object which received the signal.
   * @context: (type Gdk.DragContext): #GdkDragContext with information about the drag operation
   * @dest_file: (type Gio.File): #GFile with the tentative location that is being hovered for a drop
   * @source_file_list: (type GLib.List) (element-type GFile) (transfer none):
	 *   List of #GFile that are being dragged
   *
   * When the user starts a drag-and-drop operation and the sidebar needs
   * to ask the application for which drag action to perform, then the
   * sidebar will emit this signal.
   *
   * The application can evaluate the @context for customary actions, or
   * it can check the type of the files indicated by @source_file_list against the
   * possible actions for the destination @dest_file.
   *
   * The drag action to use must be the return value of the signal handler.
   *
   * Returns: The drag action to use, for example, #GDK_ACTION_COPY
   * or #GDK_ACTION_MOVE, or 0 if no action is allowed here (i.e. drops
   * are not allowed in the specified @dest_file).
   *
   * Since: 3.10
   */
  places_sidebar_signals [DRAG_ACTION_REQUESTED] =
          g_signal_new (I_("drag-action-requested"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_LAST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, drag_action_requested),
                        NULL, NULL,
                        _gtk_marshal_INT__OBJECT_OBJECT_POINTER,
                        G_TYPE_INT, 3,
                        GDK_TYPE_DRAG_CONTEXT,
                        G_TYPE_OBJECT,
                        G_TYPE_POINTER /* GList of GFile */ );

  /**
   * GtkPlacesSidebar::drag-action-ask:
   * @sidebar: the object which received the signal.
   * @actions: Possible drag actions that need to be asked for.
   *
   * The places sidebar emits this signal when it needs to ask the application
   * to pop up a menu to ask the user for which drag action to perform.
   *
   * Returns: the final drag action that the sidebar should pass to the drag side
   * of the drag-and-drop operation.
   *
   * Since: 3.10
   */
  places_sidebar_signals [DRAG_ACTION_ASK] =
          g_signal_new (I_("drag-action-ask"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_LAST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, drag_action_ask),
                        NULL, NULL,
                        _gtk_marshal_INT__INT,
                        G_TYPE_INT, 1,
                        G_TYPE_INT);

  /**
   * GtkPlacesSidebar::drag-perform-drop:
   * @sidebar: the object which received the signal.
   * @dest_file: (type Gio.File): Destination #GFile.
   * @source_file_list: (type GLib.List) (element-type GFile) (transfer none):
   *   #GList of #GFile that got dropped.
   * @action: Drop action to perform.
   *
   * The places sidebar emits this signal when the user completes a
   * drag-and-drop operation and one of the sidebar's items is the
   * destination.  This item is in the @dest_file, and the
   * @source_file_list has the list of files that are dropped into it and
   * which should be copied/moved/etc. based on the specified @action.
   *
   * Since: 3.10
   */
  places_sidebar_signals [DRAG_PERFORM_DROP] =
          g_signal_new (I_("drag-perform-drop"),
                        G_OBJECT_CLASS_TYPE (gobject_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (GtkPlacesSidebarClass, drag_perform_drop),
                        NULL, NULL,
                        _gtk_marshal_VOID__OBJECT_POINTER_INT,
                        G_TYPE_NONE, 3,
                        G_TYPE_OBJECT,
                        G_TYPE_POINTER, /* GList of GFile */
                        G_TYPE_INT);

  properties[PROP_LOCATION] =
          g_param_spec_object ("location",
                               P_("Location to Select"),
                               P_("The location to highlight in the sidebar"),
                               G_TYPE_FILE,
                               G_PARAM_READWRITE);
  properties[PROP_OPEN_FLAGS] =
          g_param_spec_flags ("open-flags",
                              P_("Open Flags"),
                              P_("Modes in which the calling application can open locations selected in the sidebar"),
                              GTK_TYPE_PLACES_OPEN_FLAGS,
                              GTK_PLACES_OPEN_NORMAL,
                              G_PARAM_READWRITE);
  properties[PROP_SHOW_DESKTOP] =
          g_param_spec_boolean ("show-desktop",
                                P_("Show 'Desktop'"),
                                P_("Whether the sidebar includes a builtin shortcut to the Desktop folder"),
                                TRUE,
                                G_PARAM_READWRITE);
  properties[PROP_SHOW_CONNECT_TO_SERVER] =
          g_param_spec_boolean ("show-connect-to-server",
                                P_("Show 'Connect to Server'"),
                                P_("Whether the sidebar includes a builtin shortcut to a 'Connect to server' dialog"),
                                FALSE,
                                G_PARAM_READWRITE);
  properties[PROP_SHOW_ENTER_LOCATION] =
          g_param_spec_boolean ("show-enter-location",
                                P_("Show 'Enter Location'"),
                                P_("Whether the sidebar includes a builtin shortcut to manually enter a location"),
                                FALSE,
                                G_PARAM_READWRITE);
  properties[PROP_LOCAL_ONLY] =
          g_param_spec_boolean ("local-only",
                                P_("Local Only"),
                                P_("Whether the sidebar only includes local files"),
                                FALSE,
                                G_PARAM_READWRITE);

  g_object_class_install_properties (gobject_class, NUM_PROPERTIES, properties);
}

/**
 * gtk_places_sidebar_new:
 *
 * Creates a new #GtkPlacesSidebar widget.
 *
 * The application should connect to at least the
 * #GtkPlacesSidebar::open-location signal to be notified
 * when the user makes a selection in the sidebar.
 *
 * Returns: a newly created #GtkPlacesSidebar
 *
 * Since: 3.10
 */
GtkWidget *
gtk_places_sidebar_new (void)
{
  return GTK_WIDGET (g_object_new (gtk_places_sidebar_get_type (), NULL));
}



/* Drag and drop interfaces */

/* GtkTreeDragSource::row_draggable implementation for the shortcuts model */
static gboolean
shortcuts_model_row_draggable (GtkTreeDragSource *drag_source,
                               GtkTreePath       *path)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  PlaceType place_type;
  SectionType section_type;

  model = GTK_TREE_MODEL (drag_source);

  gtk_tree_model_get_iter (model, &iter, path);
  gtk_tree_model_get (model, &iter,
                      PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                      PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section_type,
                      -1);

  if (place_type != PLACES_HEADING && section_type == SECTION_BOOKMARKS)
    return TRUE;

  return FALSE;
}

/* Fill the GtkTreeDragSourceIface vtable */
static void
shortcuts_model_class_init (ShortcutsModelClass *klass)
{
}

static void
shortcuts_model_init (ShortcutsModel *model)
{
  model->sidebar = NULL;
}

static void
shortcuts_model_drag_source_iface_init (GtkTreeDragSourceIface *iface)
{
  iface->row_draggable = shortcuts_model_row_draggable;
}

static GtkListStore *
shortcuts_model_new (GtkPlacesSidebar *sidebar)
{
  ShortcutsModel *model;
  GType model_types[PLACES_SIDEBAR_COLUMN_COUNT] = {
    G_TYPE_INT,
    G_TYPE_STRING,
    G_TYPE_DRIVE,
    G_TYPE_VOLUME,
    G_TYPE_MOUNT,
    G_TYPE_STRING,
    G_TYPE_ICON,
    G_TYPE_INT,
    G_TYPE_BOOLEAN,
    G_TYPE_BOOLEAN,
    G_TYPE_BOOLEAN,
    G_TYPE_STRING,
    G_TYPE_INT,
    G_TYPE_STRING,
    G_TYPE_BOOLEAN
  };

  model = g_object_new (shortcuts_model_get_type (), NULL);
  model->sidebar = sidebar;

  gtk_list_store_set_column_types (GTK_LIST_STORE (model),
                                   PLACES_SIDEBAR_COLUMN_COUNT,
                                   model_types);

  return GTK_LIST_STORE (model);
}



/* Public methods for GtkPlacesSidebar */

/**
 * gtk_places_sidebar_set_open_flags:
 * @sidebar: a places sidebar
 * @flags: Bitmask of modes in which the calling application can open locations
 *
 * Sets the way in which the calling application can open new locations from
 * the places sidebar.  For example, some applications only open locations
 * “directly” into their main view, while others may support opening locations
 * in a new notebook tab or a new window.
 *
 * This function is used to tell the places @sidebar about the ways in which the
 * application can open new locations, so that the sidebar can display (or not)
 * the “Open in new tab” and “Open in new window” menu items as appropriate.
 *
 * When the #GtkPlacesSidebar::open-location signal is emitted, its flags
 * argument will be set to one of the @flags that was passed in
 * gtk_places_sidebar_set_open_flags().
 *
 * Passing 0 for @flags will cause #GTK_PLACES_OPEN_NORMAL to always be sent
 * to callbacks for the “open-location” signal.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_set_open_flags (GtkPlacesSidebar   *sidebar,
                                   GtkPlacesOpenFlags  flags)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  if (sidebar->open_flags != flags)
    {
      sidebar->open_flags = flags;
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_OPEN_FLAGS]);
    }
}

/**
 * gtk_places_sidebar_get_open_flags:
 * @sidebar: a #GtkPlacesSidebar
 *
 * Gets the open flags.
 *
 * Returns: the #GtkPlacesOpenFlags of @sidebar
 *
 * Since: 3.10
 */
GtkPlacesOpenFlags
gtk_places_sidebar_get_open_flags (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), 0);

  return sidebar->open_flags;
}

/**
 * gtk_places_sidebar_set_location:
 * @sidebar: a places sidebar
 * @location: (allow-none): location to select, or #NULL for no current path
 *
 * Sets the location that is being shown in the widgets surrounding the
 * @sidebar, for example, in a folder view in a file manager.  In turn, the
 * @sidebar will highlight that location if it is being shown in the list of
 * places, or it will unhighlight everything if the @location is not among the
 * places in the list.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_set_location (GtkPlacesSidebar *sidebar,
                                 GFile            *location)
{
  GList *children;
  GList *child;
  gchar *row_uri;
  gchar *uri;

  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  gtk_list_box_unselect_all (GTK_LIST_BOX (sidebar->list_box));

  if (sidebar->current_location != NULL)
    g_object_unref (sidebar->current_location);
  sidebar->current_location = location;
  if (sidebar->current_location != NULL)
    g_object_ref (sidebar->current_location);

  if (location == NULL)
          goto out;

  uri = g_file_get_uri (location);

  children = gtk_container_get_children (GTK_CONTAINER (sidebar->list_box));
  for (child = children; child != NULL; child = child->next)
    {
      g_object_get (SIDEBAR_ROW (child->data), "uri", &row_uri, NULL);
      if (row_uri != NULL && g_strcmp0 (row_uri, uri) == 0)
        {
          gtk_list_box_select_row (GTK_LIST_BOX (sidebar->list_box),
                                   GTK_LIST_BOX_ROW (child->data));
          break;
        }
    }

  g_free (uri);

 out:
  g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_LOCATION]);
}

/**
 * gtk_places_sidebar_get_location:
 * @sidebar: a places sidebar
 *
 * Gets the currently-selected location in the @sidebar.  This can be #NULL when
 * nothing is selected, for example, when gtk_places_sidebar_set_location() has
 * been called with a location that is not among the sidebar’s list of places to
 * show.
 *
 * You can use this function to get the selection in the @sidebar.  Also, if you
 * connect to the #GtkPlacesSidebar::populate-popup signal, you can use this
 * function to get the location that is being referred to during the callbacks
 * for your menu items.
 *
 * Returns: (transfer full): a GFile with the selected location, or #NULL if nothing is visually
 * selected.
 *
 * Since: 3.10
 */
GFile *
gtk_places_sidebar_get_location (GtkPlacesSidebar *sidebar)
{
  GtkListBoxRow *selected;
  GFile *file;

  g_return_val_if_fail (sidebar != NULL, NULL);

  file = NULL;
  selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));

  if (selected)
    {
      gchar *uri;

      g_object_get (SIDEBAR_ROW (selected), "uri", &uri, NULL);
      file = g_file_new_for_uri (uri);

      g_free (uri);
    }

  return file;
}

/**
 * gtk_places_sidebar_set_show_desktop:
 * @sidebar: a places sidebar
 * @show_desktop: whether to show an item for the Desktop folder
 *
 * Sets whether the @sidebar should show an item for the Desktop folder.
 * The default value for this option is determined by the desktop
 * environment and the user’s configuration, but this function can be
 * used to override it on a per-application basis.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_set_show_desktop (GtkPlacesSidebar *sidebar,
                                     gboolean          show_desktop)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  /* Don't bother disconnecting from the GtkSettings -- it will just
   * complicate things.  Besides, it's highly unlikely that this will
   * change while we're running, but we can ignore it if it does.
   */
  sidebar->show_desktop_set = TRUE;

  show_desktop = !!show_desktop;
  if (sidebar->show_desktop != show_desktop)
    {
      sidebar->show_desktop = show_desktop;
      update_places (sidebar);
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_SHOW_DESKTOP]);
    }
}

/**
 * gtk_places_sidebar_get_show_desktop:
 * @sidebar: a places sidebar
 *
 * Returns the value previously set with gtk_places_sidebar_set_show_desktop()
 *
 * Returns: %TRUE if the sidebar will display a builtin shortcut to the desktop folder.
 *
 * Since: 3.10
 */
gboolean
gtk_places_sidebar_get_show_desktop (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), FALSE);

  return sidebar->show_desktop;
}

/**
 * gtk_places_sidebar_set_show_connect_to_server:
 * @sidebar: a places sidebar
 * @show_connect_to_server: whether to show an item for the Connect to Server command
 *
 * Sets whether the @sidebar should show an item for connecting to a network server; this is off by default.
 * An application may want to turn this on if it implements a way for the user to connect
 * to network servers directly.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_set_show_connect_to_server (GtkPlacesSidebar *sidebar,
                                               gboolean          show_connect_to_server)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  show_connect_to_server = !!show_connect_to_server;
  if (sidebar->show_connect_to_server != show_connect_to_server)
    {
      sidebar->show_connect_to_server = show_connect_to_server;
      update_places (sidebar);
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_SHOW_CONNECT_TO_SERVER]);
    }
}

/**
 * gtk_places_sidebar_get_show_connect_to_server:
 * @sidebar: a places sidebar
 *
 * Returns the value previously set with gtk_places_sidebar_set_show_connect_to_server()
 *
 * Returns: %TRUE if the sidebar will display a “Connect to Server” item.
 *
 * Since: 3.10
 */
gboolean
gtk_places_sidebar_get_show_connect_to_server (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), FALSE);

  return sidebar->show_connect_to_server;
}

/**
 * gtk_places_sidebar_set_show_enter_location:
 * @sidebar: a places sidebar
 * @show_enter_location: whether to show an item for the Connect to Server command
 *
 * Sets whether the @sidebar should show an item for connecting to a network server; this is off by default.
 * An application may want to turn this on if it implements a way for the user to connect
 * to network servers directly.
 *
 * Since: 3.14
 */
void
gtk_places_sidebar_set_show_enter_location (GtkPlacesSidebar *sidebar,
                                               gboolean          show_enter_location)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  show_enter_location = !!show_enter_location;
  if (sidebar->show_enter_location != show_enter_location)
    {
      sidebar->show_enter_location = show_enter_location;
      update_places (sidebar);
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_SHOW_ENTER_LOCATION]);
    }
}

/**
 * gtk_places_sidebar_get_show_enter_location:
 * @sidebar: a places sidebar
 *
 * Returns the value previously set with gtk_places_sidebar_set_show_enter_location()
 *
 * Returns: %TRUE if the sidebar will display an “Enter Location” item.
 *
 * Since: 3.14
 */
gboolean
gtk_places_sidebar_get_show_enter_location (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), FALSE);

  return sidebar->show_enter_location;
}

/**
 * gtk_places_sidebar_set_local_only:
 * @sidebar: a places sidebar
 * @local_only: whether to show only local files
 *
 * Sets whether the @sidebar should only show local files.
 *
 * Since: 3.12
 */
void
gtk_places_sidebar_set_local_only (GtkPlacesSidebar *sidebar,
                                   gboolean          local_only)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));

  local_only = !!local_only;
  if (sidebar->local_only != local_only)
    {
      sidebar->local_only = local_only;
      update_places (sidebar);
      g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_LOCAL_ONLY]);
    }
}

/**
 * gtk_places_sidebar_get_local_only:
 * @sidebar: a places sidebar
 *
 * Returns the value previously set with gtk_places_sidebar_set_local_only().
 *
 * Returns: %TRUE if the sidebar will only show local files.
 *
 * Since: 3.12
 */
gboolean
gtk_places_sidebar_get_local_only (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), FALSE);

  return sidebar->local_only;
}

static GSList *
find_shortcut_link (GtkPlacesSidebar *sidebar,
                    GFile            *location)
{
  GSList *l;

  for (l = sidebar->shortcuts; l; l = l->next)
    {
      GFile *shortcut;

      shortcut = G_FILE (l->data);
      if (g_file_equal (shortcut, location))
        return l;
    }

  return NULL;
}

/**
 * gtk_places_sidebar_add_shortcut:
 * @sidebar: a places sidebar
 * @location: location to add as an application-specific shortcut
 *
 * Applications may want to present some folders in the places sidebar if
 * they could be immediately useful to users.  For example, a drawing
 * program could add a “/usr/share/clipart” location when the sidebar is
 * being used in an “Insert Clipart” dialog box.
 *
 * This function adds the specified @location to a special place for immutable
 * shortcuts.  The shortcuts are application-specific; they are not shared
 * across applications, and they are not persistent.  If this function
 * is called multiple times with different locations, then they are added
 * to the sidebar’s list in the same order as the function is called.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_add_shortcut (GtkPlacesSidebar *sidebar,
                                 GFile            *location)
{
  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));
  g_return_if_fail (G_IS_FILE (location));

  g_object_ref (location);
  sidebar->shortcuts = g_slist_append (sidebar->shortcuts, location);

  update_places (sidebar);
}

/**
 * gtk_places_sidebar_remove_shortcut:
 * @sidebar: a places sidebar
 * @location: location to remove
 *
 * Removes an application-specific shortcut that has been previously been
 * inserted with gtk_places_sidebar_add_shortcut().  If the @location is not a
 * shortcut in the sidebar, then nothing is done.
 *
 * Since: 3.10
 */
void
gtk_places_sidebar_remove_shortcut (GtkPlacesSidebar *sidebar,
                                    GFile            *location)
{
  GSList *link;
  GFile *shortcut;

  g_return_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar));
  g_return_if_fail (G_IS_FILE (location));

  link = find_shortcut_link (sidebar, location);
  if (!link)
          return;

  shortcut = G_FILE (link->data);
  g_object_unref (shortcut);

  sidebar->shortcuts = g_slist_delete_link (sidebar->shortcuts, link);
  update_places (sidebar);
}

/**
 * gtk_places_sidebar_list_shortcuts:
 * @sidebar: a places sidebar
 *
 * Gets the list of shortcuts.
 *
 * Returns: (element-type GFile) (transfer full):
 *     A #GSList of #GFile of the locations that have been added as
 *     application-specific shortcuts with gtk_places_sidebar_add_shortcut().
 * To free this list, you can use
 * |[<!-- language="C" -->
 * g_slist_free_full (list, (GDestroyNotify) g_object_unref);
 * ]|
 *
 * Since: 3.10
 */
GSList *
gtk_places_sidebar_list_shortcuts (GtkPlacesSidebar *sidebar)
{
  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), NULL);

  return g_slist_copy_deep (sidebar->shortcuts, (GCopyFunc) g_object_ref, NULL);
}

/**
 * gtk_places_sidebar_get_nth_bookmark:
 * @sidebar: a places sidebar
 * @n: index of the bookmark to query
 *
 * This function queries the bookmarks added by the user to the places sidebar,
 * and returns one of them.  This function is used by #GtkFileChooser to implement
 * the “Alt-1”, “Alt-2”, etc. shortcuts, which activate the cooresponding bookmark.
 *
 * Returns: (transfer full): The bookmark specified by the index @n, or
 * #NULL if no such index exist.  Note that the indices start at 0, even though
 * the file chooser starts them with the keyboard shortcut “Alt-1”.
 *
 * Since: 3.10
 */
GFile *
gtk_places_sidebar_get_nth_bookmark (GtkPlacesSidebar *sidebar,
                                     gint              n)
{
  GtkTreeIter iter;
  int k;
  GFile *file;

  g_return_val_if_fail (GTK_IS_PLACES_SIDEBAR (sidebar), NULL);

  file = NULL;

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (sidebar->store), &iter))
    {
      k = 0;

      do
        {
          PlaceType place_type;
          gchar *uri;

          gtk_tree_model_get (GTK_TREE_MODEL (sidebar->store), &iter,
                              PLACES_SIDEBAR_COLUMN_ROW_TYPE, &place_type,
                              PLACES_SIDEBAR_COLUMN_URI, &uri,
                              -1);

          if (place_type == PLACES_BOOKMARK)
            {
              if (k == n) 
                {
                  file = g_file_new_for_uri (uri);
                  g_free (uri);
                  break;
                }
              g_free (uri);
              k++;
            }
        }
      while (gtk_tree_model_iter_next (GTK_TREE_MODEL (sidebar->store), &iter));
    }

  return file;
}

/**
 * gtk_places_sidebar_set_drop_targets_visible:
 * @sidebar: a places sidebar
 * @visible: whether to show the valid targets or not
 *
 * Make the GtkPlacesSidebar show drop targets, so it can show the available drop
 * targets and a "new bookmark" row. This improves the drag and drop
 * experience of the user and allow applications to show at once the available
 * drop targets.
 * This needs to be called when the application is aware of a drag and when
 * the applications is aware of the end of a drag.
 * It's not needed to take care of the drag if it finish in GtkPlacesSidebar, so
 * it's not needed to conect to their signals to call gtk_places_sidebar_set_drop_targets_visible
 * manually, just call it when the drag ends on some other widget on your application.
 *
 * Since: 3.18
 */
void
gtk_places_sidebar_set_drop_targets_visible (GtkPlacesSidebar *sidebar,
                                             gboolean          visible)
{
  if (visible)
    {
      show_new_bookmark_row (sidebar, NULL);
      update_possible_drop_targets (sidebar, TRUE);
      sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT;
    }
  else
    {
      if (sidebar->drop_state == DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT ||
          sidebar->drop_state == DROP_STATE_NEW_BOOKMARK_ARMED)
        {
          if (!sidebar->dragging_over)
            {
              on_drag_end (sidebar);
            }
          else
            {
              /* In case this is called while we are dragging we need to mark the
               * drop state as no permanent so the leave timeout can do its job.
               * This will only happen in applications that call this in a wrong
               * time */
              sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED;
            }
        }
    }
}
