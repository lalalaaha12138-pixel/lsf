#ifndef PLAYAUDIO_H
#define PLAYAUDIO_H

#include <QAudioOutput>

class QIODevice;

// Qt 5 中 QAudio 是保存音频状态枚举的命名空间，不能作为基类。
// 因此 PlayAudio 继承真正负责声音输出的 QAudioOutput。
class PlayAudio : public QAudioOutput
{
public:
    explicit PlayAudio(QObject *parent = nullptr);
    ~PlayAudio() override;

    // 打开默认音频设备并取得 Qt 的 Push 模式写入设备。
    bool open();

    // 尽量写入当前设备可接收的 PCM 数据，返回实际写入字节数。
    // 返回 0 表示缓冲区暂时已满，返回负数表示写入失败。
    qint64 write(const char *data, qint64 size);

    // 停止播放并使写入设备失效。
    void close();

private:
    // 优先选择 48 kHz、双声道、S16 PCM；设备不支持时使用最接近格式。
    static QAudioFormat selectOutputFormat();

    QIODevice *m_device = nullptr;
};

#endif // PLAYAUDIO_H
