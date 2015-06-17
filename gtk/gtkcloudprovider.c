/* gtkcloudprovider.c
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
#include "gtkcloudprovider.h"
#include <gio/gio.h>

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


typedef struct
{
  gchar *name;
  GtkCloudProviderStatus status;
  GIcon *icon;
  GMenuModel *menu;

  GDBusProxy *proxy;
  gchar *bus_name;
  gchar *object_path;
} GtkCloudProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GtkCloudProvider, gtk_cloud_provider, G_TYPE_OBJECT)

enum {
  PROP_0,
  LAST_PROP
};

enum {
  CHANGED,
  LAST_SIGNAL
};

static GParamSpec *gParamSpecs [LAST_PROP];
static guint gSignals [LAST_SIGNAL];

static void
on_get_name (GObject      *source_object,
             GAsyncResult *res,
             gpointer      user_data)
{
  GtkCloudProvider *self = GTK_CLOUD_PROVIDER (user_data);
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);
  GError *error = NULL;
  GVariant *variant;

  variant = g_dbus_proxy_call_finish (priv->proxy, res, &error);
  if (error != NULL)
    {
      g_warning ("Error getting the provider name %s", error->message);
    }
  g_print ("variant %s\n", g_variant_get_type (variant));
  priv->name = g_variant_dup_string (variant, NULL);

  g_print ("name %s\n", priv->name);

  g_signal_emit_by_name (self, "changed");
}

static void
on_proxy_created (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  GError *error = NULL;
  GtkCloudProvider *self = GTK_CLOUD_PROVIDER (user_data);
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  g_print ("creating proxy\n");
  priv->proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (error != NULL)
    {
      g_warning ("Error creating proxy for cloud provider %s", error->message);
      return;
    }

  g_dbus_proxy_call (priv->proxy,
                     "GetName",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     (GAsyncReadyCallback) on_get_name,
                     self);
}

GtkCloudProvider*
gtk_cloud_provider_new (const gchar *bus_name,
                        const gchar *object_path)
{
  GtkCloudProvider *self;
  GtkCloudProviderPrivate *priv;
  GDBusNodeInfo *proxy_info;
  GDBusInterfaceInfo *interface_info;
  GError *error = NULL;

  self = g_object_new (GTK_TYPE_CLOUD_PROVIDER, NULL);
  priv = gtk_cloud_provider_get_instance_private (self);

  proxy_info = g_dbus_node_info_new_for_xml (introspection_xml, &error);
  interface_info = g_dbus_node_info_lookup_interface (proxy_info, "org.gtk.CloudProvider");
  priv->bus_name = g_strdup (bus_name);
  priv->object_path = g_strdup (object_path);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_NONE,
                            interface_info,
                            bus_name,
                            object_path,
                            "org.gtk.CloudProvider",
                            NULL,
                            on_proxy_created,
                            self);

  return self;
}

static void
gtk_cloud_provider_finalize (GObject *object)
{
  GtkCloudProvider *self = (GtkCloudProvider *)object;
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  g_free (priv->name);
  g_clear_object (&priv->icon);
  g_clear_object (&priv->menu);
  g_clear_object (&priv->proxy);
  g_free (priv->bus_name);
  g_free (priv->object_path);

  G_OBJECT_CLASS (gtk_cloud_provider_parent_class)->finalize (object);
}

static void
gtk_cloud_provider_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GtkCloudProvider *self = GTK_CLOUD_PROVIDER (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_cloud_provider_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GtkCloudProvider *self = GTK_CLOUD_PROVIDER (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_cloud_provider_class_init (GtkCloudProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gtk_cloud_provider_finalize;
  object_class->get_property = gtk_cloud_provider_get_property;
  object_class->set_property = gtk_cloud_provider_set_property;

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
gtk_cloud_provider_init (GtkCloudProvider *self)
{
}

const gchar*
gtk_cloud_provider_get_name (GtkCloudProvider *self)
{
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  return priv->name;
}

GtkCloudProviderStatus
gtk_cloud_provider_get_status (GtkCloudProvider *self)
{
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  return priv->status;
}

GIcon*
gtk_cloud_provider_get_icon (GtkCloudProvider *self)
{
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  return priv->icon;
}

GMenuModel*
gtk_cloud_provider_get_menu (GtkCloudProvider *self)
{
  GtkCloudProviderPrivate *priv = gtk_cloud_provider_get_instance_private (self);

  return priv->menu;
}

