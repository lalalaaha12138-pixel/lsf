# MyPlay2 音视频同步详解

## 1. 文档目标

本文结合 MyPlay2 当前代码，详细解释“视频同步音频”的实现。这里的“视频同步音频”是指：

```text
音频作为主时钟
视频根据自己与音频的时间差决定等待、显示或丢帧
```

本文适合已经了解基本 C++ 语法，但尚未系统学习 C++ 内存模型和播放器时钟的读者。重点回答以下问题：

- PTS 和时间基为什么必须一起使用？
- 音频时钟具体从哪里来？
- 为什么不能用“写入声卡的字节数”直接代表播放进度？
- 视频提前和视频落后时分别怎么处理？
- 为什么音频时钟需要原子变量？
- `std::memory_order_release` 与 `std::memory_order_acquire` 有什么作用？
- `acquire/release` 能保证什么，又不能保证什么？
- 没有音频或没有有效 PTS 时如何回退？

相关代码：

- [`mydemux.cpp`](mydemux.cpp)：取得音频、视频流时间基。
- [`mydecode.cpp`](mydecode.cpp)：给解码器设置压缩包时间基。
- [`audiothread.cpp`](audiothread.cpp)：建立并发布音频主时钟。
- [`videothread.cpp`](videothread.cpp)：等待、显示或丢弃视频帧。
- [`widget.cpp`](widget.cpp)：启动音视频线程、快速投包并实施队列背压。

## 2. 为什么播放器需要同步

音频和视频是两个独立的数据流：

```text
音频包 → 音频解码器 → PCM → 声卡
视频包 → 视频解码器 → YUV → OpenGL
```

两条路径的耗时不同：

- 音频和视频编码复杂度不同。
- 解码一个视频帧通常比解码一个音频帧耗时更多。
- 声卡内部存在播放缓冲区。
- 视频要经过线程信号、GUI 事件队列和 OpenGL 绘制。
- 操作系统线程调度会产生抖动。

如果两个线程只按照“解码完成就立即播放”的方式运行，常见结果是：

```text
声音已经到第 10 秒，画面还停在第 9.8 秒
```

或者：

```text
画面已经到第 10 秒，声音只播放到第 9.9 秒
```

因此播放器需要一个共同的时间轴，并选择一个主时钟。

## 3. 当前采用的同步策略

MyPlay2 使用音频作为主时钟：

```text
               ┌──────────────────────┐
音频 AVFrame → │ AudioResample        │
               └──────────┬───────────┘
                          ↓ PCM
               ┌──────────────────────┐
               │ QAudioOutput         │
               └──────────┬───────────┘
                          ↓ processedUSecs()
                   发布 audioClock
                          ↓
视频 AVFrame → videoPts - audioClock
                          │
              ┌───────────┼───────────┐
              ↓           ↓           ↓
          视频提前     时间接近     视频落后
            等待         显示         丢帧
```

选择音频为主时钟的主要原因是：

- 声卡会按照固定采样率连续消费 PCM。
- 频繁中断、插入或删除音频很容易产生爆音。
- 人耳对音频不连续非常敏感。
- 视频可以通过等待或丢帧较自然地调整进度。

## 4. PTS 与时间基

### 4.1 PTS 不是秒

`AVFrame::pts` 是显示时间戳，但它只是一个整数刻度：

```cpp
int64_t pts;
```

实际时间由 PTS 和时间基共同决定：

```text
实际时间（秒） = PTS × time_base
```

FFmpeg 使用 `AVRational` 表示时间基：

```cpp
typedef struct AVRational {
    int num;
    int den;
} AVRational;
```

时间基 `{1, 8000}` 表示：

```text
一个 PTS 刻度 = 1 / 8000 秒
              = 0.000125 秒
              = 125 微秒
```

如果：

```text
PTS = 16000
```

那么：

```text
实际时间 = 16000 × 1/8000 = 2 秒
```

### 4.2 音频和视频的时间基通常不同

例如：

```text
音频时间基 = {1, 48000}
音频 PTS   = 96000

视频时间基 = {1, 90000}
视频 PTS   = 180000
```

原始数值分别是 `96000` 和 `180000`，不能直接比较。但换算后：

```text
音频时间 = 96000  × 1/48000 = 2 秒
视频时间 = 180000 × 1/90000 = 2 秒
```

它们实际位于同一时刻。

因此下面的代码是错误的：

```cpp
if (videoFrame->pts > audioFrame->pts) {
    // 错误：两个 PTS 的单位可能不同。
}
```

### 4.3 MyDemux 提供时间基

当前项目从每个 `AVStream` 中取得时间基：

```cpp
AVRational MyDemux::videoTimeBase()
{
    return format->streams[videoStream]->time_base;
}

AVRational MyDemux::audioTimeBase()
{
    return format->streams[audioStream]->time_base;
}
```

`Widget::openMedia()` 将它们分别传给音频和视频线程：

```cpp
m_audioThread.open(audioParameters, m_demux.audioTimeBase());

m_videoThread.open(videoParameters,
                   m_demux.videoTimeBase(),
                   &m_audioThread);
```

### 4.4 为什么还要设置 `AVCodecContext::pkt_timebase`

解码器收到的 `AVPacket::pts` 和 `AVPacket::dts` 使用流时间基。当前代码在 `MyDecode::open()` 中设置：

```cpp
codecCtx->pkt_timebase = packetTimeBase;
```

这样 FFmpeg 才知道压缩包时间戳的单位，并能更可靠地为解码帧推导 `best_effort_timestamp`。

### 4.5 统一换算成微秒

项目使用 FFmpeg 通用微秒时间基：

```text
AV_TIME_BASE_Q = {1, 1000000}
```

换算方法：

```cpp
qint64 ptsUs = av_rescale_q(
    timestamp,
    streamTimeBase,
    AV_TIME_BASE_Q);
```

换算后音频和视频都以微秒为单位：

```text
1000 微秒      = 1 毫秒
1000000 微秒   = 1 秒
```

此时才能安全计算：

```cpp
qint64 differenceUs = videoPtsUs - audioClockUs;
```

## 5. 音频时钟由什么组成

音频时钟不是简单读取一个 PTS。当前实现由以下几部分共同组成：

```text
媒体时间原点
    + 声卡已经处理的时间
    + 距离最近一次采样经过的单调时间
```

同时还必须满足：

```text
音频时钟不能超过已经提交给声卡的 PCM 末尾
```

相关成员位于 `audioThread`：

```cpp
std::atomic<qint64> m_clockUs;
std::atomic<qint64> m_clockUpdatedSteadyUs;
std::atomic<qint64> m_submittedEndUs;

AVRational m_timeBase;
qint64 m_clockOriginUs;
qint64 m_submittedBytes;
bool m_clockStarted;
```

前三个原子变量会被视频线程读取；后四个普通成员只在音频工作线程中使用。

## 6. `tryStartClock()`：建立媒体时间原点

第一次成功生成可播放 PCM 后，音频线程调用：

```cpp
tryStartClock(frame);
```

### 6.1 选择时间戳

```cpp
int64_t timestamp = frame->best_effort_timestamp;
if (timestamp == AV_NOPTS_VALUE)
    timestamp = frame->pts;
```

优先使用 `best_effort_timestamp`，是因为 FFmpeg 会利用现有 PTS、DTS 和解码信息推测较适合播放的时间戳。

如果两个时间戳都等于 `AV_NOPTS_VALUE`，说明无法把声卡播放进度映射到媒体时间轴，暂时不启动音频同步时钟：

```cpp
if (timestamp == AV_NOPTS_VALUE)
    return;
```

### 6.2 换算成微秒

```cpp
const qint64 framePtsUs = av_rescale_q(
    timestamp,
    m_timeBase,
    AV_TIME_BASE_Q);
```

### 6.3 为什么要减去已经提交的时长

```cpp
m_clockOriginUs = framePtsUs - submittedDurationUs();
```

通常第一帧就有有效 PTS，此时已经提交的时长是 0：

```text
clockOrigin = firstFramePts
```

但也可能前几个音频帧没有 PTS，已经写入了一部分 PCM，后面的帧才出现有效时间戳。

例如：

```text
当前有效帧 PTS           = 2.500 秒
此前已经提交 PCM 时长    = 0.500 秒
```

声卡从启动到当前帧之间已经有 0.5 秒数据，因此声卡起点应对应：

```text
clockOrigin = 2.500 - 0.500 = 2.000 秒
```

## 7. `submittedDurationUs()`：字节数怎样变成时长

`m_submittedBytes` 记录成功写入 `QAudioOutput` 的 PCM 字节数：

```cpp
m_submittedBytes += written;
```

以 48 kHz、双声道、16 位 PCM 为例：

```text
每个声道样本字节数 = 16 / 8 = 2 字节
一个 sample frame   = 2 字节 × 2 声道 = 4 字节
```

如果已经写入 `192000` 字节：

```text
sample frame 数量 = 192000 / 4 = 48000
持续时间          = 48000 / 48000 = 1 秒
```

对应代码：

```cpp
const int bytesPerSample = outputFormat.sampleSize() / 8;
const int bytesPerFrame =
    bytesPerSample * outputFormat.channelCount();

const qint64 submittedSamples =
    m_submittedBytes / bytesPerFrame;

return av_rescale(submittedSamples,
                  AV_TIME_BASE,
                  outputFormat.sampleRate());
```

这里的 `submittedSamples` 表示 sample frame 数量，不需要再乘声道数。

## 8. `refreshClock()`：从声卡采样播放进度

### 8.1 为什么不能只看写入量

调用：

```cpp
m_device->write(data, size);
```

成功只表示 PCM 已经进入 `QAudioOutput` 缓冲区，不代表用户已经听到了这些数据。

假设一次写入了 200 ms PCM：

```text
已经写入设备：200 ms
真正经过声卡处理：20 ms
仍在设备中排队：180 ms
```

如果把“已经写入 200 ms”当作“已经播放 200 ms”，视频会提前约 180 ms。

### 8.2 使用 `processedUSecs()`

当前实现使用：

```cpp
m_playAudio->processedUSecs()
```

它表示 `QAudioOutput` 从 `start()` 以后已经处理的音频时长。于是媒体时间为：

```cpp
qint64 playedUs =
    m_clockOriginUs + m_playAudio->processedUSecs();
```

例如：

```text
媒体时间原点        = 2.000 秒
声卡已经处理        = 0.120 秒
音频媒体时钟        = 2.120 秒
```

`processedUSecs()` 是 Qt 音频后端提供的估算值，比“已写入字节数”更接近实际播放位置，但不应理解为扬声器振膜级别的绝对精确测量。

### 8.3 限制不能超过已提交 PCM 末尾

```cpp
const qint64 submittedEndUs =
    m_clockOriginUs + submittedDurationUs();

if (playedUs > submittedEndUs)
    playedUs = submittedEndUs;
```

如果只向声卡提交到媒体第 2.2 秒，音频时钟就不能超过 2.2 秒。

### 8.4 发布一次时钟快照

```cpp
m_submittedEndUs.store(
    submittedEndUs,
    std::memory_order_release);

m_clockUpdatedSteadyUs.store(
    steadyClockUs(),
    std::memory_order_release);

m_clockUs.store(
    playedUs,
    std::memory_order_release);
```

三个值分别表示：

```text
m_submittedEndUs          已提交 PCM 的媒体时间末尾
m_clockUpdatedSteadyUs    采样 processedUSecs() 时的单调时钟
m_clockUs                 最近一次采样得到的媒体音频时钟
```

## 9. `clockUs()`：视频线程如何取得当前音频时间

视频线程不能直接调用音频线程中的 `QAudioOutput`，否则会违反 Qt 对象的线程归属规则。因此它只读取音频线程发布的原子数据。

### 9.1 读取最近一次快照

```cpp
const qint64 sampledClockUs =
    m_clockUs.load(std::memory_order_acquire);

const qint64 sampledAtUs =
    m_clockUpdatedSteadyUs.load(std::memory_order_acquire);

const qint64 submittedEndUs =
    m_submittedEndUs.load(std::memory_order_acquire);
```

如果时钟尚未建立或已经停止，返回：

```cpp
audioThread::InvalidClockUs
```

它使用 `qint64` 的最小值，而不是 `-1`，因为媒体 PTS 允许为负数，`-1` 微秒理论上可能是合法媒体时间。

### 9.2 为什么还需要单调时钟推算

音频线程不会在每一微秒都调用 `processedUSecs()`。如果视频线程只能得到上一次采样值，音频时钟会呈阶梯状跳动。

因此记录采样时的单调时钟：

```cpp
m_clockUpdatedSteadyUs
```

视频线程读取时计算距离上次采样经过了多久：

```cpp
const qint64 elapsedUs = qMax<qint64>(
    0,
    steadyClockUs() - sampledAtUs);
```

然后推算：

```cpp
sampledClockUs + elapsedUs
```

最后仍然限制到 PCM 末尾：

```cpp
return qMin(sampledClockUs + elapsedUs,
            submittedEndUs);
```

### 9.3 为什么使用 `steady_clock`

`std::chrono::steady_clock` 是单调时钟：

```text
只会向前走，不会因为用户修改系统时间、网络校时或时区变化而突然跳动
```

播放器测量两个时刻之间经过了多久，应使用单调时钟，而不是系统日期时间。

## 10. 为什么需要 `std::atomic`

### 10.1 当前存在两个真正并行的线程

```text
audioThread::run()  写入音频时钟
videoThread::run()  读取音频时钟
```

如果使用普通变量：

```cpp
qint64 m_clockUs;
```

音频线程写入的同时，视频线程可能正在读取。这在 C++ 中叫 data race（数据竞争）。

数据竞争不是简单的“偶尔读到旧值”，而是未定义行为。编译器可以基于“程序不存在数据竞争”的假设进行优化，最终可能出现：

- 读到意料之外的值。
- 长时间看不到另一个线程的更新。
- 在某些架构上读到撕裂值。
- Debug 正常而 Release 异常。
- 程序表现无法可靠推理。

使用：

```cpp
std::atomic<qint64>
```

能够保证每次 `load()` 和 `store()` 都是原子操作，不会与另一个线程的原子访问形成数据竞争。

### 10.2 `volatile` 不能代替 `atomic`

下面的写法不能解决线程同步：

```cpp
volatile qint64 m_clockUs;
```

`volatile` 主要用于内存映射硬件寄存器等场景。它不能建立 C++ 线程间的同步关系，也不能保证复合的跨线程读写安全。

## 11. C++ 内存顺序入门

原子操作除了操作本身，还可以指定 memory order（内存顺序）。常见选项有：

| 内存顺序 | 简化理解 |
| --- | --- |
| `memory_order_relaxed` | 只保证当前原子变量读写不可撕裂，不负责发布其他数据 |
| `memory_order_release` | 用于发布；它之前的操作不能被挪到发布之后 |
| `memory_order_acquire` | 用于接收；它之后的操作不能被挪到接收之前 |
| `memory_order_acq_rel` | 同一个读改写操作同时承担 acquire 和 release |
| `memory_order_seq_cst` | 最强的顺序，额外提供所有此类操作的全局单一顺序 |

如果不写内存顺序：

```cpp
m_abort.store(true);
m_abort.load();
```

默认使用 `memory_order_seq_cst`。

## 12. `release/acquire` 到底解决什么问题

### 12.1 先看一个普通数据发布例子

假设线程 A 准备数据：

```cpp
payload = 123;
ready.store(true, std::memory_order_release);
```

线程 B 接收数据：

```cpp
if (ready.load(std::memory_order_acquire)) {
    use(payload);
}
```

当线程 B 的 acquire 读到了线程 A release 写入的 `true`，就建立了同步关系：

```text
线程 A 中 release 之前的写入
        happens-before
线程 B 中 acquire 之后的读取
```

可以把它理解为：

```text
release：数据准备好了，现在正式发布
acquire：我确认看到了这次发布，现在可以读取配套数据
```

### 12.2 为什么 CPU 或编译器会调整顺序

为了提高性能，编译器和 CPU 可能在不影响单线程结果的前提下重新安排指令、缓存写入和读取。

单线程观察：

```cpp
data = 100;
ready = true;
```

看起来一定先写 `data`，再写 `ready`。但跨线程观察时，如果没有同步约束，另一个线程不应假定两个写入一定按照源码顺序可见。

release/acquire 就是用来建立这种跨线程的可见性和先后关系。

## 13. 当前音频时钟为什么使用 release

音频线程发布一次时钟快照时，按照以下顺序写入：

```cpp
// 配套数据先写。
m_submittedEndUs.store(endUs,
                       std::memory_order_release);
m_clockUpdatedSteadyUs.store(nowUs,
                             std::memory_order_release);

// 基础音频时钟最后发布。
m_clockUs.store(playedUs,
                std::memory_order_release);
```

可以把 `m_clockUs` 理解成“这次快照已经发布”的主要标志：

```text
先准备：PCM 末尾、采样时刻
最后发布：采样得到的音频时钟
```

`release` 阻止这些准备工作在内存模型中被重排到最终发布之后。

## 14. 当前视频线程为什么使用 acquire

读取端首先读取基础音频时钟：

```cpp
const qint64 sampledClockUs =
    m_clockUs.load(std::memory_order_acquire);
```

如果这个 acquire 读到了音频线程某次 release 发布的值，那么发布之前的配套更新会对当前线程可见。

随后读取：

```cpp
m_clockUpdatedSteadyUs.load(std::memory_order_acquire);
m_submittedEndUs.load(std::memory_order_acquire);
```

当前实现对三个原子读取都使用 acquire，写入都使用 release。这是一种偏保守、便于初学者理解的写法。

理论上可以进一步缩小屏障范围，例如：

```cpp
// 写入端：配套字段先 relaxed 写入，最后 release 发布主字段。
end.store(value, std::memory_order_relaxed);
updatedAt.store(now, std::memory_order_relaxed);
clock.store(current, std::memory_order_release);

// 读取端：先 acquire 主字段，再 relaxed 读取配套字段。
auto current = clock.load(std::memory_order_acquire);
auto now = updatedAt.load(std::memory_order_relaxed);
auto limit = end.load(std::memory_order_relaxed);
```

但这种写法对存储顺序和读取顺序要求更严格。当前代码优先保持可读性，没有采用更激进的内存顺序优化。

## 15. acquire/release 不能保证什么

### 15.1 它不是互斥锁

`acquire/release` 不会阻止音频线程继续更新，也不会让视频线程独占这些变量。

### 15.2 多个原子变量不是数据库事务

三个独立原子变量不会自动变成一个不可分割的整体。视频线程可能在音频线程两次刷新之间读取：

```text
某个字段来自第 N 次刷新
另一个字段已经来自第 N+1 次刷新
```

当前代码通过“先写上限和采样时刻，最后发布基础时钟”以及末尾上限约束，让读取结果保持保守和可用。这里允许极短时间内出现少量估算误差，因为同步阈值本来就是毫秒级。

如果以后要求三个字段必须来自完全相同的一次快照，可以选择：

1. 使用 `QMutex` 保护一个包含三个字段的结构体；
2. 使用版本号实现 sequence lock；
3. 将需要的信息压缩成一个可原子交换的数据表示。

对于初学者，互斥锁版本通常最容易验证正确性。原子方案适合频繁读取、允许小幅估算误差且不希望视频线程等待锁的时钟数据。

### 15.3 原子类型不一定真正 lock-free

`std::atomic<T>` 保证原子语义，但 C++ 标准不保证所有平台上的所有 `T` 都一定通过无锁 CPU 指令实现。

可以查询：

```cpp
m_clockUs.is_lock_free();
```

在当前 64 位平台上，64 位整数通常能够高效原子访问，但代码不应把“atomic”与“标准保证 lock-free”画等号。

## 16. `synchronizeFrame()`：视频如何同步音频

视频线程每解码出一帧，先执行：

```cpp
const SyncDecision decision = synchronizeFrame(frame);
```

返回值：

```cpp
enum class SyncDecision {
    Present,  // 显示
    Drop,     // 丢帧
    Abort     // 停止线程
};
```

### 16.1 取得视频时间

```cpp
const qint64 videoPtsUs = frameTimestampUs(frame);
```

`frameTimestampUs()` 同样优先使用 `best_effort_timestamp`，然后使用 `pts`，最后通过视频流时间基换算成微秒。

没有时间戳时无法同步：

```cpp
if (videoPtsUs == AV_NOPTS_VALUE)
    return SyncDecision::Present;
```

### 16.2 计算时间差

```cpp
qint64 differenceUs = videoPtsUs - audioClockUs;
```

符号含义必须记清楚：

```text
differenceUs > 0  视频 PTS 更大，视频跑到音频前面，需要等待
differenceUs = 0  音视频位于同一媒体时间
differenceUs < 0  视频落后于音频，需要立即显示或丢帧
```

### 16.3 视频提前：分段等待

```cpp
while (differenceUs > 2000 && !m_abort.load()) {
    QThread::msleep(sleepMs);
    audioClockUs = m_audioClockSource->clockUs();
    differenceUs = videoPtsUs - audioClockUs;
}
```

`2000` 微秒是 2 ms 容差。线程调度和声卡时间本身就存在误差，没有必要为了最后几微秒忙等。

每次最多睡眠 10 ms，而不是一次睡完整个差值，原因是：

- 音频时钟还在前进，需要重新读取。
- 用户可能随时停止或切换文件。
- 一次睡眠过长容易因为调度误差睡过头。
- 音频可能在等待期间结束或失效。

### 16.4 视频落后：丢帧

```cpp
if (differenceUs < -lateThresholdUs)
    return SyncDecision::Drop;
```

迟到阈值优先使用当前视频帧的持续时间：

```cpp
frameDurationUs = av_rescale_q(
    frame->duration,
    m_timeBase,
    AV_TIME_BASE_Q);
```

再限制到 20~100 ms：

```cpp
lateThresholdUs = qBound<qint64>(
    20000,
    qAbs(frameDurationUs),
    100000);
```

这样可以避免异常 `duration` 导致轻微抖动就丢帧，或者已经严重落后仍不丢帧。

`Drop` 后不会复制 YUV 数据，也不会向 GUI 发送信号，而是立即处理下一帧：

```text
丢掉过时画面 → 继续解码 → 再次比较 → 直到追上音频
```

### 16.5 停止：及时返回 Abort

等待循环不断检查：

```cpp
m_abort.load()
```

`m_abort` 是原子布尔值。当前没有显式指定内存顺序，因此使用默认的 `memory_order_seq_cst`。

收到停止请求后返回 `Abort`，可以避免 `stopVideo()` 长时间等待工作线程退出。

## 17. 没有音频时的回退时钟

以下情况可能没有有效音频时钟：

- 文件没有音频流。
- 音频设备打开失败。
- 音频帧没有有效 PTS。
- 音频播放已经结束，但视频还有尾部内容。

视频线程使用第一帧视频 PTS 和 `QElapsedTimer` 建立本地时钟：

```cpp
if (!m_videoTimer.isValid()) {
    m_firstVideoPtsUs = videoPtsUs;
    m_videoTimer.start();
}
```

当前视频帧应该出现的相对时间：

```cpp
const qint64 targetElapsedUs =
    videoPtsUs - m_firstVideoPtsUs;
```

实际已经经过的时间：

```cpp
m_videoTimer.nsecsElapsed() / 1000
```

两者相减得到还需要等待多久。

## 18. 为什么需要快速投包和队列背压

旧实现按照平均帧率，每个定时周期只投递一个视频包。如果视频已经落后并丢掉一帧，下一包仍要等待完整帧周期，视频就很难真正追上音频。

当前改为：

```text
1 ms 定时器快速读取 packet
        ↓
视频队列最多 64 个包
音频队列最多 256 个包
        ↓
任一队列满时暂停解封装
```

这样视频线程落后时可以连续取得多个帧并丢弃过时帧。同时队列容量防止把整个媒体文件一次性读进内存。

当前背压按包数量计算。不同 packet 的字节大小可能相差很多，后续可以改为按队列总字节数限制。

## 19. 文件结束时的处理

文件结束包含三个不同层级：

```text
解封装器 EOF
    ↓ 通知不再有 AVPacket
解码器 drain
    ↓ sendPacket(nullptr)，取出延迟帧
重采样器和声卡缓冲
    ↓ 播放剩余 PCM
真正播放结束
```

音频线程在解码器返回 `AVERROR_EOF` 后调用：

```cpp
waitForDeviceDrain();
```

等待过程中继续刷新音频时钟，直到 `QAudioOutput` 进入 `IdleState`、`StoppedState`，或者达到保护性超时。

最后把音频时钟设为无效：

```cpp
m_clockUs.store(InvalidClockUs,
                std::memory_order_release);
```

这样如果视频比音频更长，视频会切换到自身 PTS 时钟，而不会永远等待停止在最后一个值的音频时钟。

注意：当前 `AudioResample` 结束时还没有显式调用空输入排空 `SwrContext`，变采样率场景下理论上可能遗留少量尾部样本。它与解码器 drain、声卡 drain 是三个不同问题。

## 20. 完整同步示例

假设：

```text
音频第一帧媒体 PTS      = 2.000 秒
QAudioOutput 已处理      = 0.120 秒
距离最近采样又经过       = 0.008 秒
已经提交的 PCM 末尾      = 2.200 秒
当前视频帧 PTS           = 2.180 秒
```

音频时钟：

```text
audioClock = 2.000 + 0.120 + 0.008
           = 2.128 秒
```

没有超过 2.200 秒的 PCM 末尾，因此结果有效。

视频差值：

```text
difference = 2.180 - 2.128
           = +0.052 秒
           = +52 ms
```

差值为正，说明视频早了 52 ms。视频线程分段等待，每次醒来重新读取音频时钟，接近目标时间后显示。

另一个例子：

```text
audioClock = 2.128 秒
videoPts   = 2.070 秒
difference = -58 ms
```

假设当前帧迟到阈值为 40 ms：

```text
-58 ms < -40 ms
```

返回 `Drop`，当前画面不显示，继续处理下一帧。

## 21. 当前实现的边界与后续改进

### 21.1 `processedUSecs()` 存在平台误差

不同操作系统和音频后端对“已经处理”的统计精度可能不同。当前实现适合基础播放器，但不等于专业播放引擎的硬件时钟校准。

### 21.2 GUI 渲染也可能产生延迟

视频线程决定显示后，通过 Qt 队列信号把 `QByteArray` 发送到 GUI。如果 GUI 线程繁忙，待显示帧仍可能排队。

后续可以让渲染控件只保留最新帧，避免 GUI 信号队列积压旧画面。

### 21.3 暂停和 seek 尚未重置时钟

增加暂停、继续和跳转时，需要同时处理：

- 暂停或恢复 `QAudioOutput`。
- 重置 `m_clockOriginUs` 和已提交时长。
- 清空音视频包队列和解码器缓存。
- 重置视频 `QElapsedTimer`。
- 重新建立新的第一帧 PTS 基准。

### 21.4 时间戳跳变

网络流、拼接文件或损坏媒体可能出现 PTS 大幅跳变。当前代码没有专门检测 discontinuity。后续可以设置“不同步阈值”，时间差异常大时重建时钟，而不是长时间等待或连续丢帧。

### 21.5 变速播放

当前单调时钟默认按照 1.0 倍速度前进。倍速播放时，需要把经过时间乘以播放速度，并调整音频重采样或使用专门的变速不变调算法。

## 22. 调试建议

学习同步时，可以暂时每隔若干帧输出一次：

```cpp
qDebug() << "video pts(ms):" << videoPtsUs / 1000.0
         << "audio clock(ms):" << audioClockUs / 1000.0
         << "difference(ms):" << differenceUs / 1000.0;
```

不要每帧长期开启日志，因为控制台输出本身会影响线程调度和同步结果。

重点观察：

```text
正常播放：difference 在较小范围内波动
视频提前：difference 为正，随后逐渐接近 0
视频落后：difference 小于负阈值，出现 Drop
音频无效：进入 QElapsedTimer 回退路径
```

## 23. 推荐阅读代码顺序

1. `MyDemux::audioTimeBase()` 和 `videoTimeBase()`。
2. `MyDecode::open()` 中的 `pkt_timebase`。
3. `audioThread::submittedDurationUs()`。
4. `audioThread::tryStartClock()`。
5. `audioThread::refreshClock()`。
6. `audioThread::clockUs()`。
7. `videoThread::frameTimestampUs()`。
8. `videoThread::synchronizeFrame()`。
9. `Widget::dispatchNextPackets()` 的队列背压。

## 24. 核心记忆

```text
PTS 必须结合 time_base 才有实际时间意义

audioClock ≈
    音频媒体起点
    + QAudioOutput 已处理时长
    + 距离最近采样经过的单调时间

audioClock 不能超过已提交 PCM 的末尾

videoPts - audioClock > 0：视频提前，等待
videoPts - audioClock ≈ 0：正常显示
videoPts - audioClock < -阈值：视频落后，丢帧

std::atomic 解决跨线程数据竞争
release 用于发布此前准备好的数据
acquire 用于接收并观察这次发布
acquire/release 不会自动把多个原子变量变成一个事务
volatile 不能代替 atomic
```
