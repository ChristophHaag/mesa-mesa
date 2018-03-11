#!/bin/sh
LANG=en
rm -f mesa.hud.c mesa-hud.h dummy-app
gdbus-codegen --interface-prefix mesahud --generate-c-code mesa.hud mesa.hud.xml
gcc dummy-app.c mesa.hud.c -o dummy-app -O0 -ggdb $(pkg-config --cflags --libs dbus-1 dbus-glib-1 glib-2.0 gio-2.0 gio-unix-2.0)
./dummy-app
