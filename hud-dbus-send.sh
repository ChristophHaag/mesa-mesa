#!/bin/sh

dbus-send --session --dest=mesa.hud --type=method_call /mesa/hud mesa.hud.AddGraph string:"fps"
