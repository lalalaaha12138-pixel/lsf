#ifndef WIDGET_H
#define WIDGET_H

#include "mydecode.h"
#include "mydemux.h"
#include "audiothread.h"

#include <QTimer>
#include <QWidget>

struct AVFrame;
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

    // 打开媒体文件并启动视频解码。成功后由定时器持续拉取视频帧。
    bool openMedia(const QString &fileName);

private slots:
    void decodeNextFrame();

private:
    // 停止定时器并释放解码器、解封装器中与当前文件有关的状态。
    void stopPlayback();

    // 将 m_videoFrame 交给渲染器；渲染器复制数据后立即解引用该帧。
    bool presentFrame();

    Ui::Widget *ui = nullptr;
    videoOpenGLWidget *m_videoWidget = nullptr;
    MyDemux m_demux;
    MyDecode m_videoDecoder;
    // 音频包由 Widget 分发给该线程，线程内完成解码、重采样和播放。
    audioThread m_audioThread;
    AVFrame *m_videoFrame = nullptr;
    QTimer m_decodeTimer;
    // 到达文件尾后只向解码器发送一次空包，用来取出内部缓存的延迟帧。
    bool m_draining = false;
};

#endif // WIDGET_H
