QT -= gui

TEMPLATE = lib
DEFINES += HOOKS_LIBRARY

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0


# Default rules for deployment.
unix {
    target.path = /usr/lib
}
!isEmpty(target.path): INSTALLS += target

SOURCES += \
    hooks.c

win32-g++ {
    message("win32-g++")
    # QMAKE_CXXFLAGS +=-static -static-libgcc -static-libstdc++
    QMAKE_LFLAGS +=-static -static-libgcc -static-libstdc++
    LIBS += -lshlwapi -lcomctl32 -lpsapi -lole32
}
win32-msvc* {
    message("win32-msvc*")
    LIBS += user32.lib Advapi32.lib shlwapi.lib comctl32.lib psapi.lib Ole32.lib
}


