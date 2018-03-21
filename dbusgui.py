#!/usr/bin/env python3
import sys

from PyQt5.QtWidgets import (QWidget, QPushButton, QListWidget, QListWidgetItem, QHBoxLayout, QVBoxLayout, QGridLayout,
                             QApplication, QAbstractItemView, QCompleter, QLineEdit)
from PyQt5.QtCore import pyqtSlot, Qt, QStringListModel, QSortFilterProxyModel
from PyQt5 import QtCore

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

GRIDSIZEV = 9
GRIDSIZEH = GRIDSIZEV // 3

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

hudoptions = glxinfoHudOptions()

class ThumbListWidget(QListWidget):
    def __init__(self, selectionMode=QAbstractItemView.ExtendedSelection):
        super().__init__()
        self.setIconSize(QtCore.QSize(124, 124))
        self.setDragDropMode(QAbstractItemView.DragDrop)
        self.setSelectionMode(selectionMode)
        self.setAcceptDrops(True)

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.accept()
        else:
            super(ThumbListWidget, self).dragEnterEvent(event)

    def dragMoveEvent(self, event):
        if event.mimeData().hasUrls():
            event.setDropAction(QtCore.Qt.CopyAction)
            event.accept()
        else:
            super(ThumbListWidget, self).dragMoveEvent(event)

    def dropEvent(self, event):
        #print('dropEvent', event)
        if event.mimeData().hasUrls():
            event.setDropAction(QtCore.Qt.CopyAction)
            event.accept()
            links = []
            for url in event.mimeData().urls():
                links.append(str(url.toLocalFile()))
            self.emit(QtCore.SIGNAL("dropped"), links)
        else:
            event.setDropAction(QtCore.Qt.MoveAction)
            super(ThumbListWidget, self).dropEvent(event)


class PidList (QListWidget):
    def Clicked(self, item):
            print(item)


class ConfigList (QGridLayout):
    configgrid = []

    def __init__(self):
        super().__init__()
        for i in range(GRIDSIZEH):
            self.configgrid.append([])
            for j in range(GRIDSIZEV):
                l = ThumbListWidget(QAbstractItemView.MultiSelection)
                l.setMaximumWidth(75)
                #l.addItem(str(i) + " " + str(j))
                self.configgrid[i].append(l)
                self.addWidget(l, j, i)
                
        #self.setSelectionMode(QAbstractItemView.MultiSelection)
    
    def removeSelectedItems(self, configlist):
        #print("type " + str(type(configlist)))
        assert isinstance(configlist, QListWidget)
        model = configlist.model()
        for selectedItem in configlist.selectedItems():
            #print("remove " + str(configlist.indexFromItem(selectedItem)) + " row ")
            qIndex = configlist.indexFromItem(selectedItem)
            model.removeRow(qIndex.row())
        
    def removeAllSelected(self):
        for i in range(GRIDSIZEH):
            for j in range(GRIDSIZEV):
                #print("remove from " + str(i) + " " + str(j))
                configlist = self.configgrid[i][j]
                self.removeSelectedItems(configlist)

    def clear(self):
        for i in range(GRIDSIZEH):
            for j in range(GRIDSIZEV):
                configlist = self.configgrid[i][j]
                configlist.clear()

    def getOptionString(self):
        s = ""
        #print("options")
        for i in range(GRIDSIZEH):
            onecolumn = ""
            for j in range(GRIDSIZEV):
                configlist = self.configgrid[i][j]
                onegraph = ""
                for itemindex in range(configlist.count()):
                    text = configlist.item(itemindex).text()
                    #print("item: " + text)
                    if onegraph != "":
                        onegraph += "+"
                    onegraph += text
                if onegraph != "":
                    if onecolumn != "":
                        onecolumn += ","
                    onecolumn += onegraph
            if onecolumn != "":
                if s != "":
                    s += ";"
                s += onecolumn
        #print(s)
        return s
                    
                
                
    def Clicked(self, item):
            print(item)


class AvailableConfigList (ThumbListWidget):
    def fill(self, filter):
        for option in hudoptions:
            #print("adding item " + option)
            if filter and not filter in option:
                continue
            self.addItem(QListWidgetItem(option))

    def clear(self):
        super().clear()

    def __init__(self, filter=None):
        super().__init__()
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.fill(filter)

    def Clicked(self, item):
            print(item.text())

#https://stackoverflow.com/a/7767999
class CustomQCompleter(QCompleter):
    def __init__(self, parent=None):
        super(CustomQCompleter, self).__init__(parent)
        self.local_completion_prefix = ""
        self.source_model = None

    def setModel(self, model):
        self.source_model = model
        super(CustomQCompleter, self).setModel(self.source_model)

    def updateModel(self):
        local_completion_prefix = self.local_completion_prefix
        class InnerProxyModel(QSortFilterProxyModel):
            def filterAcceptsRow(self, sourceRow, sourceParent):
                index0 = self.sourceModel().index(sourceRow, 0, sourceParent)
                return local_completion_prefix.lower() in self.sourceModel().data(index0, 0).lower()
        proxy_model = InnerProxyModel()
        proxy_model.setSourceModel(self.source_model)
        super(CustomQCompleter, self).setModel(proxy_model)

class Example(QWidget):
    pidlist = None
    configlist = None
    available = None
    searchbox = None

    def __init__(self):
        super().__init__()
        self.initUI()

    @pyqtSlot()
    def pidlist_click(self):
        assert isinstance(self.pidlist, QListWidget)
        assert isinstance(self.configlist, ConfigList)
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
        self.configlist.removeAllSelected()

    @pyqtSlot()
    def set_click(self):
        config = self.configlist.getOptionString()
        #assert isinstance(self.configlist, QListWidget)
        #for i in range(self.configlist.count()):
        #    config += self.configlist.item(i).text()
        #    if i < self.configlist.count() - 1:
        #        config += ","
        print("Setting config to \"" + config + "\" for the following applications:")
        for selectedItem in self.pidlist.selectedItems():
            conn = selectedItem.data(Qt.UserRole)
            conn["conn"].GraphConfiguration(config)
            conn["lastconfig"] = config
            print("\tbus: " + conn["busname"] + ", object: " + OBJECTPATH)

    @pyqtSlot()
    def filter_available(self):
        text = self.searchbox.text()
        self.available.clear()
        self.available.fill(text)


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

        searchboxmodel = QStringListModel()
        searchboxmodel.setStringList(hudoptions)
        searchboxcompleter = CustomQCompleter()
        searchboxcompleter.setModel(searchboxmodel)
        searchboxcompleter.setCompletionMode(QCompleter.PopupCompletion)

        searchbox = QLineEdit()
        searchbox.setCompleter(searchboxcompleter)
        searchbox.textChanged.connect(self.filter_available)
        self.searchbox = searchbox
        #searchbox.show()

        availableLayout = QVBoxLayout()
        availableLayout.addWidget(available)
        availableLayout.addWidget(searchbox)
        #availableLayout.addLayout(availableButtons)

        configLayout = QVBoxLayout()
        configLayout.addLayout(configlist)
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
