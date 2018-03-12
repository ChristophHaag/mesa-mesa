#!/usr/bin/env python3
import sys
from pydbus import SessionBus

bus = SessionBus()
o = bus.get('mesa.hud')

#print(o.Introspect())
#help(o)

#print("Application:", o.ApplicationBinary)

config = "fps" if len(sys.argv) < 2 else sys.argv[1]
print("Setting graph config to:", config)
o.GraphConfiguration(config)

#reply = o.Configure(0)
#print(reply)

