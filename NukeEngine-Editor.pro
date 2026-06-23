TEMPLATE = app
CONFIG += console c++14
CONFIG += x86_64
CONFIG -= app_bundle
CONFIG -= qt

# Qt 5.3 and lower doesn't recognize "c++14". Use c++11 and then replace
# the compiler flags with c++14.
contains(QT_MAJOR_VERSION, 5):lessThan(QT_MINOR_VERSION, 4) {
    CONFIG += c++11
    QMAKE_CXXFLAGS_CXX11 = $$replace(QMAKE_CXXFLAGS_CXX11, "std=c\+\+11", "std=c++1y")
    QMAKE_CXXFLAGS_CXX11 = $$replace(QMAKE_CXXFLAGS_CXX11, "std=c\+\+0x", "std=c++1y")
}

# Qt 4 doesn't even know about C++11, so just add c++14 directly.
contains(QT_MAJOR_VERSION, 4) {
    QMAKE_CXXFLAGS += -std=c++1y
}

!macx{
ICON += res/logo.ico
win32:RC_ICONS += res/logo.ico
}

macx{
ICON = myapp.icns
}

DEFINES += GLM_ENABLE_EXPERIMENTAL EDITOR

CONFIG(debug, debug|release) {
    DESTDIR = debug
} else {
    DESTDIR = release
}


SOURCES += \
        main.cpp \

HEADERS += \
    editor/editorui.h \

DISTFILES += \
    res/logo.ico \
    config/main.lua \
    fonts/OpenSansLight.ttf \
    imgui.ini \
    res/logo.icns


macx{
win32:CONFIG(release, debug|release): LIBS += -L$$PWD/../NukeEngine/build/debug/release/ -lNukeEngine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/../NukeEngine/build/debug/debug/ -lNukeEngine
else:unix: LIBS += -L$$PWD/../NukeEngine/build/debug/ -lNukeEngine

LIBS += -L/System/Library -L/Library -L/usr/local/lib -lboost_thread-mt -lboost_system -lboost_filesystem -llua -lassimp -lGLEW -lGLUT -lGLFW -framework OpenGL -framework GLUT -framework Cocoa -framework CoreFoundation

INCLUDEPATH += /usr/local/include


DEPENDPATH += $$PWD/../NukeEngine

DEFINES += __APPLE__

}

!macx{
LIBS += -L/usr/local/lib -lglut -lGL -lGLU -lGLEW -pthread -lboost_thread -lboost_system -lboost_filesystem -llua -ldl -lassimp

win32:CONFIG(release, debug|release): LIBS += -L$$PWD/../NukeEngine/build/debug/ -lNukeEngine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/../NukeEngine/build/debug/ -lNukeEngine
else:unix: LIBS += -L$$PWD/../NukeEngine/build/$$DESTDIR/ -lNukeEngine

INCLUDEPATH +=

DEPENDPATH += $$PWD/../NukeEngine
}


INCLUDEPATH += $$PWD/../NukeEngine \
    $$PWD/../NukeEngine/deps/glm \
#    $$PWD/../NukeEngine/deps \
    $$PWD/../NukeEngine/deps/bgfx/include \
    $$PWD/../NukeEngine/deps/bx/include \
#    $$PWD/../NukeEngine/deps/glfw/include \
    $$PWD/../NukeEngine/deps/LuaBridge/Source

win32:CONFIG(release, debug|release): LIBS += -L$$PWD/../NukeEngine/deps/bgfx/build/osx64_clang/bin/release/ -lbgfx-shared-libDebug
else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/../NukeEngine/deps/bgfx/build/osx64_clang/bin/debug/ -lbgfx-shared-libDebug
else:unix: LIBS += -L$$PWD/../NukeEngine/deps/bgfx/build/osx64_clang/bin/ -lbgfx-shared-libDebug

INCLUDEPATH += $$PWD/../NukeEngine/deps/bgfx/include
DEPENDPATH += $$PWD/../NukeEngine/deps/bgfx/include
