#include "videoopenglwidget.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

const char *vertexShaderSource = R"GLSL(
#version 330 core
layout(location = 0) in vec2 position;
layout(location = 2) in vec2 textureIn;
uniform vec2 scale;
out vec2 textureOut;

void main()
{
    gl_Position = vec4(position * scale, 0.0, 1.0);
    textureOut = textureIn;
}
)GLSL";

// 片元着色器从三张单通道纹理采样，并将视频范围的 YUV420P 转为 RGB。
const char *fragmentShaderSource = R"GLSL(
#version 330 core
in vec2 textureOut;
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
layout(location = 0) out vec4 fragColor;

void main()
{
    float y = 1.1643 * (texture(tex_y, textureOut).r - 0.0625);
    float u = texture(tex_u, textureOut).r - 0.5;
    float v = texture(tex_v, textureOut).r - 0.5;

    vec3 rgb;
    rgb.r = y + 1.7927 * v;
    rgb.g = y - 0.2132 * u - 0.5329 * v;
    rgb.b = y + 2.1124 * u;

    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)GLSL";

} // namespace

videoOpenGLWidget::videoOpenGLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(320, 180);
}

videoOpenGLWidget::~videoOpenGLWidget()
{
    cleanupOpenGL();
}

bool videoOpenGLWidget::setFrame(const AVFrame *frame)
{
    // AVFrame 仍属于解码器，先检查最基本的帧信息再读取其数据。
    if (!frame || frame->width <= 0 || frame->height <= 0 ||
        !frame->data[0] || !frame->data[1] || !frame->data[2]) {
        return false;
    }

    // 当前阶段只处理三平面的 YUV420P，其他像素格式不做隐式转换。
    if (frame->format != AV_PIX_FMT_YUV420P) {
        qWarning() << "videoOpenGLWidget only supports AV_PIX_FMT_YUV420P, got:"
                   << frame->format;
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    // YUV420P 的色度分辨率是亮度分辨率的一半；向上取整可兼容奇数尺寸。
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    const int ySize = width * height;
    const int chromaSize = chromaWidth * chromaHeight;
    QByteArray copiedFrame(ySize + 2 * chromaSize, Qt::Uninitialized);

    // AVFrame 的每行可能含有对齐填充，不能假设 linesize 等于画面宽度。
    // 逐行复制后，缓存中三个平面都是紧密排列的连续数据。
    char *dstPlanes[3] = {
        copiedFrame.data(),
        copiedFrame.data() + ySize,
        copiedFrame.data() + ySize + chromaSize
    };
    const int rowBytes[3] = {width, chromaWidth, chromaWidth};
    const int planeRows[3] = {height, chromaHeight, chromaHeight};
    for (int plane = 0; plane < 3; ++plane) {
        for (int row = 0; row < planeRows[plane]; ++row) {
            std::memcpy(dstPlanes[plane] + row * rowBytes[plane],
                        frame->data[plane] + row * frame->linesize[plane],
                        static_cast<size_t>(rowBytes[plane]));
        }
    }

    {
        QMutexLocker locker(&m_frameMutex);
        // 从这里开始，渲染器拥有独立副本，不再依赖传入 AVFrame 的生命周期。
        m_frameData.swap(copiedFrame);
        m_frameWidth = width;
        m_frameHeight = height;
        m_frameDirty = true;
    }

    // QWidget 只能在 GUI 线程请求刷新。当前实现从 GUI 定时器调用，
    // 同时保留从独立解码线程投递帧的能力。
    if (QThread::currentThread() == thread()) {
        update();
    } else {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
    return true;
}

void videoOpenGLWidget::clearFrame()
{
    {
        QMutexLocker locker(&m_frameMutex);
        m_frameData.clear();
        m_frameWidth = 0;
        m_frameHeight = 0;
        m_frameDirty = false;
    }
    update();
}

void videoOpenGLWidget::initializeGL()
{
    // initializeGL 被调用时，Qt 已经把此控件的 OpenGL 上下文设为当前上下文。
    initializeOpenGLFunctions();

    if (!createShaderProgram())
        return;

    // 使用覆盖标准化设备坐标 [-1, 1] 的矩形绘制一整帧视频。
    static const GLfloat positions[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    // OpenGL 纹理坐标原点在左下角，因此这里对视频画面做垂直翻转。
    static const GLfloat textureCoordinates[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f
    };

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vboPosition);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPosition);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(0);

    glGenBuffers(1, &m_vboTextureUv);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboTextureUv);
    glBufferData(GL_ARRAY_BUFFER, sizeof(textureCoordinates),
                 textureCoordinates, GL_STATIC_DRAW);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // 三个 sampler 固定绑定到 0、1、2 号纹理单元。
    glUseProgram(m_program);
    glUniform1i(glGetUniformLocation(m_program, "tex_y"), 0);
    glUniform1i(glGetUniformLocation(m_program, "tex_u"), 1);
    glUniform1i(glGetUniformLocation(m_program, "tex_v"), 2);
    glUseProgram(0);

    m_resourcesReady = true;
}

void videoOpenGLWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void videoOpenGLWidget::paintGL()
{
    // 先清成深色背景；视频宽高比与窗口不同时，未覆盖区域即为黑边。
    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_resourcesReady)
        return;

    uploadCurrentFrame();
    if (m_textureWidth <= 0 || m_textureHeight <= 0)
        return;

    glUseProgram(m_program);
    // 在顶点着色器中缩放矩形，保持原始宽高比，避免画面被拉伸。
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (width() > 0 && height() > 0) {
        const float videoAspect = static_cast<float>(m_textureWidth) /
                                  static_cast<float>(m_textureHeight);
        const float widgetAspect = static_cast<float>(width()) /
                                   static_cast<float>(height());
        if (videoAspect > widgetAspect)
            scaleY = widgetAspect / videoAspect;
        else
            scaleX = videoAspect / widgetAspect;
    }
    glUniform2f(glGetUniformLocation(m_program, "scale"), scaleX, scaleY);
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
    }

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

GLuint videoOpenGLWidget::compileShader(GLenum type, const char *source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    QByteArray log(qMax(logLength, 1), '\0');
    glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
    qCritical().noquote()
        << (type == GL_VERTEX_SHADER
                ? "Vertex shader compilation failed:"
                : "Fragment shader compilation failed:")
        << log.constData();

    glDeleteShader(shader);
    return 0;
}

bool videoOpenGLWidget::createShaderProgram()
{
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER,
                                               vertexShaderSource);
    if (vertexShader == 0)
        return false;

    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER,
                                                 fragmentShaderSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return false;
    }

    m_program = glCreateProgram();
    glAttachShader(m_program, vertexShader);
    glAttachShader(m_program, fragmentShader);
    glLinkProgram(m_program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
        return true;

    GLint logLength = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
    QByteArray log(qMax(logLength, 1), '\0');
    glGetProgramInfoLog(m_program, log.size(), nullptr, log.data());
    qCritical().noquote() << "Program link failed:" << log.constData();

    glDeleteProgram(m_program);
    m_program = 0;
    return false;
}

void videoOpenGLWidget::ensureYuvTextures(int width, int height)
{
    // 分辨率未变化时直接复用现有纹理。
    if (m_textureWidth == width && m_textureHeight == height && m_textures[0])
        return;

    if (m_textures[0])
        glDeleteTextures(3, m_textures);

    // 每个纹理只保存一个 8 位分量，分别对应 Y、U、V 平面。
    glGenTextures(3, m_textures);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    for (int i = 0; i < 3; ++i) {
        const int planeWidth = (i == 0) ? width : chromaWidth;
        const int planeHeight = (i == 0) ? height : chromaHeight;

        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                     planeWidth, planeHeight, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    m_textureWidth = width;
    m_textureHeight = height;
}

void videoOpenGLWidget::uploadCurrentFrame()
{
    QByteArray frameData;
    int width = 0;
    int height = 0;
    {
        QMutexLocker locker(&m_frameMutex);
        if (!m_frameDirty)
            return;
        // QByteArray 使用隐式共享：锁内取得稳定快照，后续 setFrame 写入时
        // 会自动分离，从而不需要在较慢的 OpenGL 上传期间一直持锁。
        frameData = m_frameData;
        width = m_frameWidth;
        height = m_frameHeight;
        m_frameDirty = false;
    }

    if (frameData.isEmpty() || width <= 0 || height <= 0)
        return;

    ensureYuvTextures(width, height);

    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    const int ySize = width * height;
    const int chromaSize = chromaWidth * chromaHeight;
    // setFrame 已将三个平面紧密排列，因此可以通过偏移直接定位。
    const char *planes[] = {
        frameData.constData(),
        frameData.constData() + ySize,
        frameData.constData() + ySize + chromaSize
    };

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; ++i) {
        const int planeWidth = (i == 0) ? width : chromaWidth;
        const int planeHeight = (i == 0) ? height : chromaHeight;

        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        planeWidth, planeHeight,
                        GL_RED, GL_UNSIGNED_BYTE, planes[i]);
    }
}

void videoOpenGLWidget::cleanupOpenGL()
{
    // 若控件从未显示过，OpenGL 上下文可能尚未创建，此时没有 GPU 资源要释放。
    if (!context())
        return;

    makeCurrent();
    if (m_textures[0])
        glDeleteTextures(3, m_textures);
    if (m_vboPosition)
        glDeleteBuffers(1, &m_vboPosition);
    if (m_vboTextureUv)
        glDeleteBuffers(1, &m_vboTextureUv);
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_program)
        glDeleteProgram(m_program);

    m_textures[0] = m_textures[1] = m_textures[2] = 0;
    m_vboPosition = 0;
    m_vboTextureUv = 0;
    m_vao = 0;
    m_program = 0;
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_resourcesReady = false;
    doneCurrent();
}
