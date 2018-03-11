// build: See build-dummy-app.sh

#include "mesa.hud.h"
#include <stdio.h>


static gboolean on_handle_configure(MesaHud *skeleton, GDBusMethodInvocation *invocation,
                                    guint seconds) {
   printf("handle configure\n");
   return TRUE;
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  printf("name acquired\n");
  MesaHud *skeleton;
  skeleton = mesa_hud_skeleton_new ();
  g_signal_connect(skeleton, "handle-configure", G_CALLBACK(on_handle_configure), NULL);

  g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(skeleton), connection,
                                   "/mesa/hud", NULL);
}

int main(int argc, char** argv) {
   GMainLoop *loop;

   loop = g_main_loop_new(NULL, FALSE);

   g_bus_own_name(G_BUS_TYPE_SESSION, "mesa.hud", G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                  on_name_acquired, NULL, NULL, NULL);

   g_main_loop_run(loop);
   return 0;
}
