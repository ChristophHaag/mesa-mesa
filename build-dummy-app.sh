#!/bin/sh
LANG=en
rm dummy-app
gcc dummy-app.c -o dummy-app -O0 -ggdb $(pkg-config --cflags --libs dbus-1)
./dummy-app
