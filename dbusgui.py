#!/usr/bin/env python3
from pydbus import SessionBus

bus = SessionBus()
o = bus.get('mesa.hud')

#print(o.Introspect())
#help(o)

print("Application:", o.ApplicationBinary)
o.AddGraph("foobar")

#reply = o.Configure(0)
#print(reply)

