#!/usr/bin/env python3
import sys

from PyQt5.QtWidgets import (QWidget, QPushButton, QListWidget, QListWidgetItem, QHBoxLayout, QVBoxLayout, QApplication,
                             QAbstractItemView)
from PyQt5.QtCore import pyqtSlot, Qt

from pydbus import SessionBus

OBJECTPATH = "/mesa/hud"
bus = SessionBus()
dbus = bus.get("org.freedesktop.DBus")
busnames = [name for name in dbus.ListNames() if name.startswith("mesa.")]
conns = []
print("Found advertised hud objects:")
for busname in busnames:
    o = bus.get(busname, OBJECTPATH)
    binaryname = o.ApplicationBinary
    pid = busname.split("-")[1]
    conn = {"pid": pid, "busname": busname, "binaryname": binaryname.split("/")[-1], "conn": o, "lastconfig": ""}
    conns.append(conn)
    print("\t" + str(conn))
conns.sort(key=lambda x: int(x["pid"]))

optionblacklist = ["frametime_X", "low-fps"]
#o = bus.get('mesa.hud')

#print(o.Introspect())
#help(o)

#print("Application:", o.ApplicationBinary)

#config = "fps" if len(sys.argv) < 2 else sys.argv[1]
#print("Setting graph config to:", config)
#o.GraphConfiguration(config)

#reply = o.Configure(0)
#print(reply)

def parseGlxInfo(s):
    start = s.split("Available names:")[1].strip("\n")
    parsed = []
    for line in start.split("\n"):
        if line.startswith("    "):
            l = line[4:]
            if not l in optionblacklist:
                parsed.append(l) #TODO: handle "..."
        else:
            break
    #print("parsed glxinfo:" + str(parsed))
    return parsed

def glxinfoHudOptions():
    import subprocess, os
    my_env = os.environ.copy()
    my_env["GALLIUM_HUD"] = "help"
    output = subprocess.check_output(['glxinfo'], env=my_env).decode("UTF-8")
    parsed = parseGlxInfo(output)
    #print(parsed)
    return parsed

class PidList (QListWidget):
    def Clicked(self, item):
            print(item)


class ConfigList (QListWidget):
    def __init__(self):
        super().__init__()
        self.setSelectionMode(QAbstractItemView.MultiSelection)

    def Clicked(self, item):
            print(item)


class AvailableConfigList (QListWidget):
    def __init__(self):
        super().__init__()
        options = glxinfoHudOptions()
        for option in options:
            #print("adding item " + option)
            self.addItem(QListWidgetItem(option))
        self.setSelectionMode(QAbstractItemView.MultiSelection)

    def Clicked(self, item):
            print(item.text())


class Example(QWidget):
    pidlist = None
    configlist = None
    available = None

    def __init__(self):
        super().__init__()
        self.initUI()

    @pyqtSlot()
    def pidlist_click(self):
        assert isinstance(self.pidlist, QListWidget)
        assert isinstance(self.configlist, QListWidget)
        self.configlist.clear()
        item = self.pidlist.selectedItems()[0]
        lastconfig = item.data(Qt.UserRole)["lastconfig"]
        #print("lastconfig: ", lastconfig)
        if len(lastconfig) < 1:
            return
        for config in lastconfig.split(","):
            print("adding " + config + " " + str(len(lastconfig.split(","))))
            self.configlist.addItem(config)


    @pyqtSlot()
    def add_click(self):
        assert isinstance(self.available, QListWidget)
        assert isinstance(self.configlist, QListWidget)
        if len(self.available.selectedItems()) == 0: return
        for selectedItem in self.available.selectedItems():
        #print(item.text())
            self.configlist.addItem(selectedItem.text())

    @pyqtSlot()
    def remove_click(self):
        assert isinstance(self.configlist, QListWidget)
        model = self.configlist.model()
        for selectedItem in self.configlist.selectedItems():
            qIndex = self.configlist.indexFromItem(selectedItem)
            model.removeRow(qIndex.row())

    @pyqtSlot()
    def set_click(self):
        config = ""
        assert isinstance(self.configlist, QListWidget)
        for i in range(self.configlist.count()):
            config += self.configlist.item(i).text()
            if i < self.configlist.count() - 1:
                config += ","
        for selectedItem in self.pidlist.selectedItems():
            conn = selectedItem.data(Qt.UserRole)
            print("Setting config to \"" + config + "\" on bus " + conn["busname"] + ", object " + OBJECTPATH)
            conn["conn"].GraphConfiguration(config)
            conn["lastconfig"] = config

    def initUI(self):

        pidlist = PidList()
        for conn in conns:
            displayname = conn["binaryname"] + " (" + conn["pid"] + ")"
            item = QListWidgetItem(displayname)
            item.setData(Qt.UserRole, conn)
            pidlist.addItem(item)
        configlist = ConfigList()
        available = AvailableConfigList()
        self.pidlist = pidlist
        self.configlist = configlist
        self.available = available

        okButton = QPushButton("Set")
        cancelButton = QPushButton("Cancel")
        okButton.clicked.connect(self.set_click)

        availableButtons = QHBoxLayout()
        availableAddButton = QPushButton("Add")
        availableButtons.addWidget(availableAddButton)
        availableAddButton.clicked.connect(self.add_click)

        availableLayout = QVBoxLayout()
        availableLayout.addWidget(available)
        availableLayout.addLayout(availableButtons)

        configLayout = QVBoxLayout()
        configLayout.addWidget(configlist)
        configRemoveButton = QPushButton("Remove")
        configLayout.addWidget(configRemoveButton)
        configRemoveButton.clicked.connect(self.remove_click)


        # idlist.itemClicked.connect(pidlist.Clicked)
        pidlist.itemClicked.connect(self.pidlist_click)
        # available.itemClicked.connect(available.Clicked)

        #available.addItem(QListWidgetItem("fps"))
        #available.addItem(QListWidgetItem("cpu"))

        allvertical = QVBoxLayout()

        mainguihoriz = QHBoxLayout()

        mainguihoriz.addWidget(pidlist)
        mainguihoriz.addLayout(availableLayout)
        mainguihoriz.addLayout(configLayout)
        mainguihoriz.addStretch(1)

        mainbuttons = QHBoxLayout()
        mainbuttons.addStretch(1)
        mainbuttons.addWidget(okButton)
        mainbuttons.addWidget(cancelButton)


        allvertical.addLayout(mainguihoriz)
        allvertical.addLayout(mainbuttons)

        self.setLayout(allvertical)

        # self.setGeometry(300, 300, 300, 150)
        self.setGeometry(0,0,800,600)
        self.setWindowTitle('Mesa HUD GUI prototype')
        self.show()

if __name__ == '__main__':
    
    app = QApplication(sys.argv)
    ex = Example()
    sys.exit(app.exec_())
