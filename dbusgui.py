#!/usr/bin/env python3
import sys

from PyQt5.QtWidgets import (QWidget, QPushButton, QListWidget, QListWidgetItem, QHBoxLayout, QVBoxLayout, QApplication)
from PyQt5.QtCore import pyqtSlot

from pydbus import SessionBus

OBJECTPATH = "/mesa/hud"
bus = SessionBus()
dbus = bus.get("org.freedesktop.DBus")
names = [(name, name.split("-")[1]) for name in dbus.ListNames() if name.startswith("mesa.")]
print("Found advertised hud objects from PIDs:")
for name in names:
        print("\t" + name[1])
#o = bus.get('mesa.hud')

#print(o.Introspect())
#help(o)

#print("Application:", o.ApplicationBinary)

#config = "fps" if len(sys.argv) < 2 else sys.argv[1]
#print("Setting graph config to:", config)
#o.GraphConfiguration(config)

#reply = o.Configure(0)
#print(reply)


class PidList (QListWidget):
        def Clicked(self, item):
                print(item)


class ConfigList (QListWidget):
        def Clicked(self, item):
                print(item)


class AvailableConfigList (QListWidget):
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

    @pyqtSlot()
    def add_click(self):
        assert isinstance(self.available, QListWidget)
        assert isinstance(self.configlist, QListWidget)
        if len(self.available.selectedItems()) == 0: return
        item = self.available.selectedItems()[0]
        #print(item.text())
        self.configlist.addItem(item.text())

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
            busname = 'mesa.hud' + "-" + selectedItem.text()
            print("Setting config to \"" + config + "\" on bus " + busname + ", object " + OBJECTPATH)
            o = bus.get(busname, OBJECTPATH)
            o.GraphConfiguration(config)

    def initUI(self):

        pidlist = PidList()
        for name in names:
                pidlist.addItem(QListWidgetItem(name[1]))
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

        available.addItem(QListWidgetItem("fps"))
        available.addItem(QListWidgetItem("cpu"))

        allvertical = QVBoxLayout()

        mainguihoriz = QHBoxLayout()

        mainguihoriz.addWidget(pidlist)
        mainguihoriz.addLayout(configLayout)
        mainguihoriz.addLayout(availableLayout)
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
