QT       += core gui multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# FFmpeg shared development package (64-bit)
FFMPEG_ROOT = D:/Desktop/FFmpeg/ffmpeg-master-latest-win64-lgpl-shared
INCLUDEPATH += $$FFMPEG_ROOT/include
DEPENDPATH += $$FFMPEG_ROOT/include

win32 {
    LIBS += -L$$FFMPEG_ROOT/lib \
        -lavformat \
        -lavcodec \
        -lavutil \
        -lswresample
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    audioresample.cpp \
    audiothread.cpp \
    main.cpp \
    mydecode.cpp \
    mydemux.cpp \
    playaudio.cpp \
    videothread.cpp \
    videoopenglwidget.cpp \
    widget.cpp

HEADERS += \
    audioresample.h \
    audiothread.h \
    mydecode.h \
    mydemux.h \
    playaudio.h \
    videothread.h \
    videoopenglwidget.h \
    widget.h

FORMS += \
    widget.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
