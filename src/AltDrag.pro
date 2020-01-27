TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt
CONFIG -= console

INCLUDEPATH += .

SOURCES += \
    altdrag.c

win32:RC_FILE += include/altdrag.rc

win32-g++ {
    message("win32-g++")
    QMAKE_CXXFLAGS +=-mwindows
    QMAKE_LFLAGS +=-static -static-libgcc -static-libstdc++
}
win32-msvc* {
    message("win32-msvc*")
}

LIBS += -luser32 -lAdvapi32 -lShell32 -lshlwapi -lwininet -lcomctl32
