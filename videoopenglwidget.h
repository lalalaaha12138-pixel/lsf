#ifndef VIDEOOPENGLWIDGET_H
#define VIDEOOPENGLWIDGET_H

#include <QByteArray>
#include <QMutex>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>

// 视频画面渲染控件：接收视频线程复制好的 YUV420P 数据，
// 将 Y、U、V 三个平面上传到 OpenGL 纹理中完成显示。
class videoOpenGLWidget : public QOpenGLWidget,
                          protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit videoOpenGLWidget(QWidget *parent = nullptr);
    ~videoOpenGLWidget() override;

    // frameData 必须按 Y + U + V 顺序紧密排列。
    bool setFrame(const QByteArray &frameData, int width, int height);

    // 清除当前缓存的画面，下一次绘制时只显示背景色。
    void clearFrame();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    // OpenGL 资源只能在当前控件的 OpenGL 上下文中创建和销毁。
    GLuint compileShader(GLenum type, const char *source);
    bool createShaderProgram();

    // 视频分辨率发生变化时重新分配 Y、U、V 三张纹理。
    void ensureYuvTextures(int width, int height);

    // 将最近收到的一帧上传到 GPU；没有新帧时保留上一帧纹理。
    void uploadCurrentFrame();
    void cleanupOpenGL();

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vboPosition = 0;
    GLuint m_vboTextureUv = 0;
    GLuint m_textures[3] = {0, 0, 0};

    // 帧通过 Qt 队列信号进入 GUI 线程；锁同时保护 paintGL 的缓存快照。
    QMutex m_frameMutex;
    QByteArray m_frameData;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    bool m_frameDirty = false;

    // 当前 GPU 纹理尺寸，用于判断是否需要重新创建纹理。
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    bool m_resourcesReady = false;
};

#endif // VIDEOOPENGLWIDGET_H
