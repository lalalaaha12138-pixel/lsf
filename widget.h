#ifndef WIDGET_H
#define WIDGET_H

#include "audiothread.h"
#include "mydemux.h"
#include "videothread.h"

#include <QTimer>
#include <QWidget>

class videoOpenGLWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget() override;

    // 打开媒体文件，初始化音视频线程并启动数据包分发定时器。
    bool openMedia(const QString &fileName);

private slots:
    // 从唯一的解封装器读取数据包，并按流类型交给对应工作线程。
    void dispatchNextPackets();

private:
    void stopPlayback();

    Ui::Widget *ui = nullptr;
    videoOpenGLWidget *m_videoWidget = nullptr;
    MyDemux m_demux;
    videoThread m_videoThread;
    audioThread m_audioThread;
    QTimer m_packetTimer;
};

#endif // WIDGET_H
