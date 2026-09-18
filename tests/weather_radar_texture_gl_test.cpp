#include "gui/map/WeatherRadarTexture.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSurfaceFormat>
#include <QVector2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

using namespace AetherSDR;

namespace {
using Channels = std::array<double, 4>;

Channels sample(const QImage& image, QVector2D uv)
{
    if (uv.x() < 0 || uv.x() > 1 || uv.y() < 0 || uv.y() > 1) {
        return {};
    }
    const double x = uv.x() * image.width() - .5;
    const double y = uv.y() * image.height() - .5;
    const int left = static_cast<int>(std::floor(x));
    const int top = static_cast<int>(std::floor(y));
    Channels result{};
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            // Independent expected value: premultiply EACH discrete source
            // pixel before interpolation. Do not sample straight RGB then
            // multiply the filtered alpha (the original production defect).
            const QRgb pixel = qPremultiply(image.pixelColor(
                std::clamp(left + dx, 0, image.width() - 1),
                std::clamp(top + dy, 0, image.height() - 1)).rgba());
            const double weight = (dx ? x - left : 1 - x + left)
                * (dy ? y - top : 1 - y + top);
            const Channels channels{double(qRed(pixel)), double(qGreen(pixel)),
                                    double(qBlue(pixel)), double(qAlpha(pixel))};
            for (int channel = 0; channel < 4; ++channel) {
                result[channel] += weight * channels[channel];
            }
        }
    }
    return result;
}
}

int main(int argc, char** argv)
{
    // No sockets/radio/network. The default gate skips BEFORE GUI discovery;
    // explicit opt-in requires an actual GPU and fails if it cannot be tested.
    if (!qEnvironmentVariableIsSet("AETHERSDR_TEST_RADAR_GL")) {
        std::cout << "SKIP: set AETHERSDR_TEST_RADAR_GL=1 for native texture math\n";
        return 77;
    }
    QGuiApplication app(argc, argv);
    QSurfaceFormat format;
    format.setVersion(3, 2);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        return 1;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        return 1;
    }
    QOpenGLFunctions* gl = context.functions();
    gl->initializeOpenGLFunctions();
    std::cout << "GL renderer=" << gl->glGetString(GL_RENDERER) << '\n';
    QOpenGLShaderProgram program;
    const QByteArray vertex = R"GLSL(#version 150
        in vec2 position;
        void main() { gl_Position = vec4(position, 0, 1); }
    )GLSL";
    const QByteArray fragment = QByteArray(R"GLSL(#version 150
        #define texture2D texture
        uniform sampler2D sourceA;
        uniform sampler2D sourceB;
        uniform vec2 uvA;
        uniform vec2 uvB;
        uniform float blend;
        uniform float opacity;
        out vec4 result;
    )GLSL") + WeatherRadarTexture::kFragmentFunctions + R"GLSL(
        void main() {
            result = radarComposite(radarSample(sourceA, uvA),
                                    radarSample(sourceB, uvB), blend, opacity);
        }
    )GLSL";
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
        || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
        || !program.link() || !program.bind()) {
        std::cerr << qPrintable(program.log()) << '\n';
        return 1;
    }
    QOpenGLVertexArrayObject vao;
    QOpenGLBuffer vertices;
    if (!vao.create() || !vertices.create()) {
        return 1;
    }
    vao.bind();
    vertices.bind();
    const std::array<float, 6> triangle{-1, -1, 3, -1, -1, 3};
    vertices.allocate(triangle.data(), sizeof(triangle));
    program.enableAttributeArray("position");
    program.setAttributeBuffer("position", GL_FLOAT, 0, 2);
    QOpenGLFramebufferObject fbo(1, 1, QOpenGLFramebufferObject::NoAttachment,
                                  GL_TEXTURE_2D, GL_RGBA8);
    if (!fbo.isValid() || !fbo.bind()) {
        return 1;
    }
    gl->glViewport(0, 0, 1, 1);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_DITHER);

    QImage a(2, 2, QImage::Format_RGBA8888);
    a.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    a.setPixelColor(1, 0, QColor(0, 0, 0, 0));
    a.setPixelColor(0, 1, QColor(0, 0, 255, 255));
    a.setPixelColor(1, 1, QColor(0, 255, 0, 127));
    QImage b(2, 2, QImage::Format_RGBA8888);
    b.setPixelColor(0, 0, QColor(200, 80, 40, 63));
    b.setPixelColor(1, 0, QColor(255, 0, 255, 0)); // hidden RGB must not leak
    b.setPixelColor(0, 1, QColor(90, 180, 250, 192));
    b.setPixelColor(1, 1, QColor(255, 255, 255, 255));

    int comparisons = 0;
    int maximum = 0;
    const std::array<QVector2D, 9> points{QVector2D(.25f, .25f), {.5f, .25f},
        {.75f, .25f}, {.25f, .75f}, {.75f, .75f}, {.5f, .5f},
        {.3125f, .6875f}, {-.01f, .5f}, {1.01f, .5f}};
    // PNG decode -> worker preparation, raw image fallback, stripe reuse, and
    // live mipmaps all use the SAME production upload and shader helpers.
    for (int route = 0; route < 4; ++route) {
        QImage uploadA = a;
        QImage uploadB = b;
        if (route == 1) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!a.save(&buffer, "PNG")) {
                return 1;
            }
            uploadA = WeatherRadarTexture::prepareImage(QImage::fromData(png, "PNG"));
            uploadB = WeatherRadarTexture::prepareImage(b);
            if (WeatherRadarTexture::prepareImage(uploadA).cacheKey() != uploadA.cacheKey()) {
                std::cerr << "Prepared cache entry was copied on reuse\n";
                return 1;
            }
        }
        std::unique_ptr<QOpenGLTexture> textureA = WeatherRadarTexture::makeTexture(uploadA, route == 3);
        std::unique_ptr<QOpenGLTexture> textureB = WeatherRadarTexture::makeTexture(uploadB, route == 3);
        if (textureA == nullptr || textureB == nullptr) {
            return 1;
        }
        if (route == 2) {
            // Reuse storage and upload disjoint rows, like playback lookahead.
            if (!WeatherRadarTexture::uploadRows(*textureA, b, 0, 1)
                || !WeatherRadarTexture::uploadRows(*textureA, b, 1, 1)
                || WeatherRadarTexture::uploadRows(*textureA, b, 2, 1)) {
                return 1;
            }
            uploadA = b;
        }
        if (route == 3) {
            textureA->setMipBaseLevel(1);
            textureA->setMipMaxLevel(1);
            textureB->setMipBaseLevel(1);
            textureB->setMipMaxLevel(1);
        }
        textureA->bind(0);
        textureB->bind(1);
        program.setUniformValue("sourceA", 0);
        program.setUniformValue("sourceB", 1);
        for (const QVector2D& uv : points) {
            for (float blend : {0.0f, .37f, 1.0f}) {
                for (float opacity : {.78f, 1.0f}) {
                    const QVector2D targetUv(1 - uv.x(), uv.y());
                    program.setUniformValue("uvA", uv);
                    program.setUniformValue("uvB", targetUv);
                    program.setUniformValue("blend", blend);
                    program.setUniformValue("opacity", opacity);
                    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
                    std::array<uchar, 4> actual{};
                    gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
                    const bool inside = uv.x() >= 0 && uv.x() <= 1;
                    const QVector2D expectedUv = route == 3 && inside ? QVector2D(.5f, .5f) : uv;
                    const QVector2D expectedTarget = route == 3 && inside ? QVector2D(.5f, .5f) : targetUv;
                    const Channels previous = sample(uploadA, expectedUv);
                    const Channels current = sample(uploadB, expectedTarget);
                    for (int channel = 0; channel < 4; ++channel) {
                        const int expected = qRound(((1 - blend) * previous[channel]
                            + blend * current[channel]) * opacity);
                        const int delta = std::abs(int(actual[channel]) - expected);
                        maximum = std::max(maximum, delta);
                        if (delta > 2) {
                            std::cerr << "route=" << route << " uv=" << uv.x() << ',' << uv.y()
                                << " blend=" << blend << " channel=" << channel
                                << " actual=" << int(actual[channel]) << " expected=" << expected << '\n';
                            return 1;
                        }
                    }
                    ++comparisons;
                }
            }
        }
    }
    if (gl->glGetError() != GL_NO_ERROR) {
        std::cerr << "OpenGL error in native texture regression\n";
        return 1;
    }
    std::cout << "Native texture comparisons=" << comparisons
        << " maximum RGBA8 channel error=" << maximum << '\n';
    return 0;
}
