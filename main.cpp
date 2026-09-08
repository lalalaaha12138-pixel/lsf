#include "widget.h"

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTimer>

int main(int argc, char *argv[])
{
    // videoOpenGLWidget 使用 GLSL 330 和 VAO，因此在创建 QApplication 前
    // 请求 OpenGL 3.3 Core 上下文。
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    Widget player;
    player.resize(960, 540);
    player.show();

    // 窗口显示后再选择/打开文件，避免文件对话框阻塞窗口初始化。
    // 命令行提供路径时不弹框，便于调试和自动化测试。
    const QStringList arguments = app.arguments();
    QTimer::singleShot(0, &player, [&player, arguments]() {
        QString mediaFile;
        if (arguments.size() > 1) {
            mediaFile = arguments.at(1);
        } else {
            mediaFile = QFileDialog::getOpenFileName(
                &player,
                QObject::tr("Open video"),
                QString(),
                QObject::tr("Video files (*.mp4 *.mkv *.avi *.mov *.flv *.ts);;All files (*.*)"));
        }

        if (!mediaFile.isEmpty() && !player.openMedia(mediaFile)) {
            QMessageBox::critical(&player,
                                  QObject::tr("Playback error"),
                                  QObject::tr("Could not open or decode the selected video."));
        }
    });

    return app.exec();
}
