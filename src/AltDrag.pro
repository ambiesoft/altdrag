TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG -= console

INCLUDEPATH += .

SOURCES += \
    altdrag.c

LIBS += -lshlwapi -lwininet -lcomctl32

win32:RC_FILE += include/altdrag.rc
QMAKE_CXXFLAGS +=-mwindows
