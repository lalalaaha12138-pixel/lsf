QT       += core gui multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# FFmpeg 路径按以下顺序读取：
# 1. qmake 命令行：qmake FFMPEG_ROOT=D:/path/to/ffmpeg
# 2. 项目本地 config.pri（该文件不会提交到 Git）
# 3. 工程内置 third_party/ffmpeg
# 4. 系统环境变量 FFMPEG_ROOT
isEmpty(FFMPEG_ROOT) {
    exists($$PWD/config.pri) {
        include($$PWD/config.pri)
    }
}

isEmpty(FFMPEG_ROOT) {
    BUNDLED_FFMPEG_ROOT = $$clean_path($$PWD/third_party/ffmpeg)
    exists($$BUNDLED_FFMPEG_ROOT/include/libavformat/avformat.h) {
        FFMPEG_ROOT = $$BUNDLED_FFMPEG_ROOT
    }
}

isEmpty(FFMPEG_ROOT) {
    FFMPEG_ROOT = $$(FFMPEG_ROOT)
}

isEmpty(FFMPEG_ROOT) {
    error("FFmpeg was not found. Keep third_party/ffmpeg in the project, configure config.pri, set FFMPEG_ROOT, or pass FFMPEG_ROOT to qmake.")
}

FFMPEG_ROOT = $$clean_path($$FFMPEG_ROOT)
!exists($$FFMPEG_ROOT/include/libavformat/avformat.h) {
    error("Invalid FFMPEG_ROOT: $$FFMPEG_ROOT (libavformat/avformat.h was not found)")
}

INCLUDEPATH += $$quote($$FFMPEG_ROOT/include)
DEPENDPATH += $$quote($$FFMPEG_ROOT/include)

win32 {
    !exists($$FFMPEG_ROOT/lib) {
        error("Invalid FFMPEG_ROOT: $$FFMPEG_ROOT/lib was not found")
    }

    LIBS += -L$$quote($$FFMPEG_ROOT/lib) \
        -lavformat \
        -lavcodec \
        -lavutil \
        -lswresample

    # 将程序实际依赖的 FFmpeg DLL 复制到可执行文件旁边。
    # 这样从 Qt Creator 或资源管理器启动时都不依赖系统 PATH。
    FFMPEG_RUNTIME_DLLS = \
        $$FFMPEG_ROOT/bin/avformat-63.dll \
        $$FFMPEG_ROOT/bin/avcodec-63.dll \
        $$FFMPEG_ROOT/bin/avutil-61.dll \
        $$FFMPEG_ROOT/bin/swresample-7.dll

    CONFIG(debug, debug|release) {
        FFMPEG_RUNTIME_DIR = $$clean_path($$OUT_PWD/debug)
    } else {
        FFMPEG_RUNTIME_DIR = $$clean_path($$OUT_PWD/release)
    }

    for(runtimeDll, FFMPEG_RUNTIME_DLLS) {
        !exists($$runtimeDll) {
            error("Missing FFmpeg runtime library: $$runtimeDll")
        }
        QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$shell_path($$runtimeDll)) $$shell_quote($$shell_path($$FFMPEG_RUNTIME_DIR)) $$escape_expand(\n\t)
    }
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
