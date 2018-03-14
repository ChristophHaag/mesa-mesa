
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>
#include <stdbool.h>
#include "./dbus.h"

//getpid
#include <sys/types.h>
#include <unistd.h>

#include "hud_context.h"

void hud_parse_env_var(struct hud_context *hud, struct pipe_screen *screen, const char *env);

#define BUS_BASENAME "mesa.hud"
char *dbus_busname;;
char *dbus_objectpath = "/mesa/hud";
char *iface = "mesa.hud";

char* binaryname;

static void check_errors(DBusError *error) {
   if (dbus_error_is_set(error)) {
      printf("DBus: %s\n", error->message);
      dbus_error_free(&error);
   }
}

static DBusHandlerResult mesage_handler(DBusConnection *connection, DBusMessage *message, void *user_data);
static void respond_to_introspect(DBusConnection *connection, DBusMessage *request);
static void respond_to_addgraph(DBusConnection *connection, DBusMessage *request);
static void respond_to_configuration(DBusConnection *connection, DBusMessage *request);
static void respond_to_property(DBusConnection *connection, DBusMessage *request);
static void respond_to_property_all(DBusConnection *connection, DBusMessage *request);
DBusConnection *connection = NULL;

static bool initialized = false;
static char *reconfigured = NULL;

//TODO: why not from dbus.h
void dbus_init(void);
bool dbus_initialized(void);
void dbus_update(void);
char *dbus_reconfigured(void);

void dbus_init(void) {
   binaryname = "TODO";

   int pid = (int) getpid();
   char pidname [100];
   snprintf(pidname, 100, "%d", pid);
   printf("Appname: %s, Pid: %s\n", binaryname, pidname);
   dbus_busname = malloc(sizeof(char) * (strlen(BUS_BASENAME) + strlen(pidname)));
   sprintf(dbus_busname, "%s-%s", BUS_BASENAME, pidname);
   printf("busname %s\n", dbus_busname);

   DBusError error;

   DBusObjectPathVTable vtable;

   vtable.message_function = mesage_handler;
   vtable.unregister_function = NULL;

   char buffer[1024];

   dbus_error_init(&error);
   connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
   check_errors(&error);


   printf("Requesting bus name %s\n", dbus_busname);
   dbus_bus_request_name(connection, dbus_busname, 0, &error);
   check_errors(&error);

   printf("Registering dbus_objectpath path: %s\n", dbus_objectpath);
   dbus_connection_try_register_object_path(connection,
                                            dbus_objectpath,
                                            &vtable,
                                            NULL,
                                            &error);
   check_errors(&error);

   initialized = true;
   //dbus_connection_read_write_dispatch(connection, 1000);

}

bool dbus_initialized() {
   return initialized;
}

void dbus_update() {
   dbus_connection_read_write_dispatch(connection, 0);
}

char *dbus_reconfigured(void) {
   if (reconfigured == NULL) {
      return NULL;
   };
   char *copy = strdup(reconfigured);
   reconfigured = NULL;
   return copy;
}


static DBusHandlerResult mesage_handler(DBusConnection *connection, DBusMessage *message, void *user_data) {
   const char *interface_name = dbus_message_get_interface(message);
   const char *member_name = dbus_message_get_member(message);

   printf("message: %s, %s\n", interface_name, member_name);

   if (0==strcmp("org.freedesktop.DBus.Introspectable", interface_name) && 0==strcmp("Introspect", member_name)) {
      respond_to_introspect(connection, message);
      return DBUS_HANDLER_RESULT_HANDLED;
   } else if (0==strcmp(iface, interface_name) && 0==strcmp("AddGraph", member_name)) {
      respond_to_addgraph(connection, message);
      return DBUS_HANDLER_RESULT_HANDLED;
   } else if (0==strcmp(iface, interface_name) && 0==strcmp("GraphConfiguration", member_name)) {
      respond_to_configuration(connection, message);
      return DBUS_HANDLER_RESULT_HANDLED;
   } else if (0==strcmp("org.freedesktop.DBus.Properties", interface_name) && 0==strcmp("Get", member_name)) {
      respond_to_property(connection, message);
      return DBUS_HANDLER_RESULT_HANDLED;
   } else if (0==strcmp("org.freedesktop.DBus.Properties", interface_name) && 0==strcmp("GetAll", member_name)) {
      respond_to_property_all(connection, message);
      return DBUS_HANDLER_RESULT_HANDLED;
   } else {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
   }
}

static void respond_to_property(DBusConnection *connection, DBusMessage *request) {
   DBusMessage *reply;
   DBusError error;
   const char *interface, *property;

   dbus_error_init(&error);

   dbus_message_get_args(request, &error,
                         DBUS_TYPE_STRING, &interface,
                         DBUS_TYPE_STRING, &property,
                         DBUS_TYPE_INVALID);

   if (dbus_error_is_set(&error)) {
      reply = dbus_message_new_error(request, "wrong_arguments", "Illegal arguments to Sum");
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
      return;
   }

   if (0==strcmp("ApplicationBinary", property)) {
      printf("Sending property value %s, %s: %s\n", interface, property, binaryname);

      reply = dbus_message_new_method_return(request);
      dbus_message_append_args(reply,
                               DBUS_TYPE_STRING, &binaryname,
                               DBUS_TYPE_INVALID);
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
   } else {
      printf("property %s: %s not implemented\n", interface, property);
   }
}

static void respond_to_property_all(DBusConnection *connection, DBusMessage *request) {
   DBusHandlerResult result;
   DBusMessageIter array, dict, iter, variant;
   const char *property = "ApplicationBinary";

   /*
    * All dbus functions used below might fail due to out of
    * memory error. If one of them fails, we assume that all
    * following functions will fail too, including
    * dbus_connection_send().
    */
   result = DBUS_HANDLER_RESULT_NEED_MEMORY;

   dbus_message_iter_init_append(request, &iter);
   dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array);

   /* Append all properties name/value pairs */
   property = "ApplicationBinary";
   dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
   dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &property);
   dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "s", &variant);
   dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &binaryname);
   dbus_message_iter_close_container(&dict, &variant);
   dbus_message_iter_close_container(&array, &dict);

   dbus_message_iter_close_container(&iter, &array);

   if (dbus_connection_send(connection, request, NULL))
      result = DBUS_HANDLER_RESULT_HANDLED;
   return result;
}

static void respond_to_introspect(DBusConnection *connection, DBusMessage *request) {
   DBusMessage *reply;

   const char *introspection_data;
   asprintf(&introspection_data,
            " <!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
            " <!-- dbus-sharp 0.8.1 -->"
            " <node>"
            "   <interface name=\"org.freedesktop.DBus.Introspectable\">"
            "     <method name=\"Introspect\">"
            "       <arg name=\"data\" direction=\"out\" type=\"s\" />"
            "     </method>"
            "   </interface>"
            "  <interface name='org.freedesktop.DBus.Properties'>\n"
            "    <method name='Get'>\n"
            "      <arg name='interface' type='s' direction='in' />\n"
            "      <arg name='property'  type='s' direction='in' />\n"
            "      <arg name='value'     type='s' direction='out' />\n"
            "    </method>\n"
            "    <method name='GetAll'>\n"
            "      <arg name='interface'  type='s'     direction='in'/>\n"
            "      <arg name='properties' type='a{sv}' direction='out'/>\n"
            "    </method>\n"
            "  </interface>\n"
            "   <interface name=\"%s\">"
            "     <method name=\"AddGraph\">"
            "       <arg name=\"configstring\" direction=\"in\" type=\"s\" />"
            "     </method>"
            "     <method name=\"GraphConfiguration\">"
            "       <arg name=\"configstring\" direction=\"in\" type=\"s\" />"
            "     </method>"
            "     <property name=\"ApplicationBinary\" type=\"s\" access=\"read\" />"
            "   </interface>"
            " </node>",
            iface);

   //printf("xml:\n%s\n", introspection_data);

   reply = dbus_message_new_method_return(request);
   dbus_message_append_args(reply,
                            DBUS_TYPE_STRING, &introspection_data,
                            DBUS_TYPE_INVALID);
   dbus_connection_send(connection, reply, NULL);
   dbus_message_unref(reply);
}

static void respond_to_configuration(DBusConnection *connection, DBusMessage *request) {
   DBusMessage *reply;
   DBusError error;
   char* configstring;

   dbus_error_init(&error);

   dbus_message_get_args(request, &error,
                         DBUS_TYPE_STRING, &configstring,
                         DBUS_TYPE_INVALID);
   if (dbus_error_is_set(&error)) {
      reply = dbus_message_new_error(request, "wrong_arguments", "Illegal arguments to Sum");
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
      return;
   }

   printf("Setting hud configuration to %s\n", configstring);
   reconfigured = configstring;

   //hud_parse_env_var(hudctx, hudscreen, "fps");

   reply = dbus_message_new_method_return(request);
   // TODO: how to not send a return value properly
   dbus_message_append_args(reply,
                            DBUS_TYPE_INVALID);
   dbus_connection_send(connection, reply, NULL);
   dbus_message_unref(reply);
}

static void respond_to_addgraph(DBusConnection *connection, DBusMessage *request) {
   DBusMessage *reply;
   DBusError error;
   char* configstring;

   dbus_error_init(&error);

   dbus_message_get_args(request, &error,
                         DBUS_TYPE_STRING, &configstring,
                         DBUS_TYPE_INVALID);
   if (dbus_error_is_set(&error)) {
      reply = dbus_message_new_error(request, "wrong_arguments", "Illegal arguments to Sum");
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
      return;
   }

   printf("Adding graph with config %s\n", configstring);

   //hud_parse_env_var(hudctx, hudscreen, "fps");

   reply = dbus_message_new_method_return(request);
   // TODO: how to not send a return value properly
   dbus_message_append_args(reply,
                            DBUS_TYPE_INVALID);
   dbus_connection_send(connection, reply, NULL);
   dbus_message_unref(reply);
}
