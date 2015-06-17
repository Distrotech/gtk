#include <gio/gio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------------------- */

enum {
  IDDLE,
  SYNCING,
  ERROR
};

/* The object we want to export */
typedef struct _CloudProviderClass CloudProviderClass;
typedef struct _CloudProvider CloudProvider;

struct _CloudProviderClass
{
  GObjectClass parent_class;
};

struct _CloudProvider
{
  GObject parent_instance;

  gchar *name;
  gint status;
};


static GType cloud_provider_get_type (void);
G_DEFINE_TYPE (CloudProvider, cloud_provider, G_TYPE_OBJECT);

static void
cloud_provider_finalize (GObject *object)
{
  CloudProvider *self = (CloudProvider*)object;

  g_free (self->name);

  G_OBJECT_CLASS (cloud_provider_parent_class)->finalize (object);
}

static void
cloud_provider_init (CloudProvider *self)
{
  self->name = "cloud provider";
  self->status = IDDLE;
}

static void
cloud_provider_class_init (CloudProviderClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  gobject_class->finalize = cloud_provider_finalize;
}

/* ---------------------------------------------------------------------------------------------------- */

static GDBusNodeInfo *introspection_data = NULL;

/* Introspection data for the service we are exporting */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.gtk.CloudProvider'>"
  "    <method name='GetName'>"
  "      <arg type='s' name='name' direction='out'/>"
  "    </method>"
  "    <method name='GetStatus'>"
  "      <arg type='i' name='name' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";


static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  CloudProvider *cloud_provider = user_data;

  g_print ("handling method call\n");
#if 0
  if (g_strcmp0 (method_name, "ChangeCount") == 0)
    {
      gint change;
      g_variant_get (parameters, "(i)", &change);

      my_object_change_count (myobj, change);

      g_dbus_method_invocation_return_value (invocation, NULL);
    }
#endif
  if (g_strcmp0 (method_name, "GetName") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(s)", cloud_provider->name));
    }
  else if (g_strcmp0 (method_name, "GetStatus") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(s)", cloud_provider->status));
    }
}

static const GDBusInterfaceVTable interface_vtable =
{
  handle_method_call,
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  CloudProvider *cloud_provider = user_data;
  guint registration_id;

  g_print ("registring\n");
  registration_id = g_dbus_connection_register_object (connection,
                                                       "/org/gtk/CloudProviderServerExample",
                                                       introspection_data->interfaces[0],
                                                       &interface_vtable,
                                                       cloud_provider,
                                                       NULL,  /* user_data_free_func */
                                                       NULL); /* GError** */
  g_assert (registration_id > 0);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  CloudProvider *cloud_provider;
  guint owner_id;

  introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
  g_assert (introspection_data != NULL);

  cloud_provider = g_object_new (cloud_provider_get_type (), NULL);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.gtk.CloudProviderServerExample",
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             cloud_provider,
                             NULL);


  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  g_dbus_node_info_unref (introspection_data);

  g_object_unref (cloud_provider);

  return 0;
}

