#include "config.h"
#include <glib.h>
#include <gtk/gtk.h>

static void
on_manager_changed (GtkCloudProviderManager *manager)
{

  g_print ("Manager changed\n");
}

gint
main (gint   argc,
      gchar *argv[])
{
  GtkCloudProviderManager *manager;
  GMainLoop *loop;

  g_set_prgname ("my-program");
  g_set_application_name ("My-program");


  manager = gtk_cloud_provider_manager_dup_singleton ();
  g_signal_connect (manager, "changed", G_CALLBACK (on_manager_changed), NULL);
  gtk_cloud_provider_manager_update (manager);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
