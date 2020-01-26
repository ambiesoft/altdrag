TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += .

SOURCES += \
    altdrag.c

LIBS += -lshlwapi -lwininet -lcomctl32

win32:RC_FILE += include/altdrag.rc

