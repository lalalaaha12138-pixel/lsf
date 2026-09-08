#include "playaudio.h"

#include <QAudioDeviceInfo>
#include <QDebug>
#include <QIODevice>
#include <QtGlobal>

QAudioFormat PlayAudio::selectOutputFormat()
{
    QAudioFormat requested;
    requested.setCodec(QStringLiteral("audio/pcm"));
    requested.setSampleRate(48000);
    requested.setChannelCount(2);
    requested.setSampleSize(16);
    requested.setSampleType(QAudioFormat::SignedInt);
    requested.setByteOrder(QAudioFormat::LittleEndian);

    const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
    if (device.isNull())
        return QAudioFormat();

    return device.isFormatSupported(requested)
        ? requested
        : device.nearestFormat(requested);
}

PlayAudio::PlayAudio(QObject *parent)
    : QAudioOutput(selectOutputFormat(), parent)
{
}

PlayAudio::~PlayAudio()
{
    close();
}

bool PlayAudio::open()
{
    close();
    if (!format().isValid()) {
        qWarning() << "No valid default audio output format";
        return false;
    }

    // 约 200 ms 的设备缓冲可降低短时线程调度抖动导致的爆音概率。
    setBufferSize(format().bytesForDuration(200000));
    m_device = start();
    if (!m_device) {
        qWarning() << "Cannot start the default audio output device, error:"
                   << error();
        return false;
    }
    return true;
}

qint64 PlayAudio::write(const char *data, qint64 size)
{
    if (!m_device || !data || size <= 0)
        return -1;

    const int available = bytesFree();
    if (available <= 0)
        return 0;

    return m_device->write(data, qMin(size, static_cast<qint64>(available)));
}

void PlayAudio::close()
{
    if (m_device || state() != QAudio::StoppedState)
        stop();
    m_device = nullptr;
}
