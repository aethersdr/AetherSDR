#pragma once

#include <QImage>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLTexture>

#include <memory>

namespace AetherSDR::WeatherRadarTexture {

// One byte-order/alpha contract for flat playback, globe playback, live atlas,
// and detail tiles. Premultiply BEFORE bilinear/mipmap filtering, not in the
// fragment shader afterwards: filtering opaque red beside transparent black
// gives (R=.5,A=.5); multiplying that sample again yields R=.25 instead of .5.
// That wrong order makes moving subpixel echo edges darken and appear to shrink.
// Matching code explicitly converts back to straight colors when it needs them.
constexpr QImage::Format kImageFormat = QImage::Format_RGBA8888_Premultiplied;

inline QImage prepareImage(const QImage& image)
{
    return image.convertToFormat(kImageFormat);
}

inline bool uploadRows(QOpenGLTexture& texture, const QImage& image,
                       int firstRow, int rowCount)
{
    if (image.isNull() || !texture.isStorageAllocated()
        || texture.width() != image.width() || texture.height() != image.height()
        || firstRow < 0 || rowCount <= 0 || firstRow > image.height() - rowCount) {
        return false;
    }
    // Normal playback images are prepared once in the decoder worker. Keep a
    // bounded stripe fallback for other callers, rather than doing a full-size
    // conversion on every paint or assuming unknown QImage bytes are RGBA.
    const QImage converted = image.format() == kImageFormat ? QImage{}
        : prepareImage(image.copy(0, firstRow, image.width(), rowCount));
    const uchar* pixels = image.format() == kImageFormat
        ? image.constScanLine(firstRow) : converted.constBits();
    if (pixels == nullptr) {
        return false;
    }
    QOpenGLPixelTransferOptions options;
    options.setAlignment(4);
    options.setRowLength(image.format() == kImageFormat
        ? image.bytesPerLine() / 4 : converted.bytesPerLine() / 4);
    texture.setData(0, firstRow, 0, image.width(), rowCount, 1,
                    QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, pixels, &options);
    return true;
}

inline std::unique_ptr<QOpenGLTexture> makeTexture(const QImage& image,
                                                 bool mipmaps = false)
{
    if (image.isNull()) {
        return {};
    }
    // Do NOT use QOpenGLTexture(QImage) or setData(QImage): Qt converts those
    // overloads back to STRAIGHT RGBA8888, silently undoing premultiplication.
    // Explicit raw upload is required even when image.format() is premultiplied.
    std::unique_ptr<QOpenGLTexture> texture =
        std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
    texture->setSize(image.width(), image.height());
    texture->setMipLevels(mipmaps ? texture->maximumMipLevels() : 1);
    texture->setAutoMipMapGenerationEnabled(false);
    texture->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
    if (!uploadRows(*texture, image, 0, image.height())) {
        return {};
    }
    texture->setMinificationFilter(mipmaps ? QOpenGLTexture::LinearMipMapLinear
                                          : QOpenGLTexture::Linear);
    texture->setMagnificationFilter(QOpenGLTexture::Linear);
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    if (mipmaps) {
        texture->generateMipMaps();
    }
    return texture;
}

// Used verbatim by BOTH production shaders and the GPU regression test. Sample
// premultiplied texels, blend those samples, and apply layer opacity exactly once.
// GL compositing uses ONE, ONE_MINUS_SRC_ALPHA. No post-filter alpha multiply.
inline constexpr char kFragmentFunctions[] = R"GLSL(
    lowp vec4 radarSample(sampler2D frame, highp vec2 sourceUv) {
        lowp float inside = step(0.0, sourceUv.x) * step(sourceUv.x, 1.0)
            * step(0.0, sourceUv.y) * step(sourceUv.y, 1.0);
        return texture2D(frame, clamp(sourceUv, 0.0, 1.0)) * inside;
    }
    lowp vec4 radarComposite(lowp vec4 previous, lowp vec4 current,
                            highp float blend, lowp float opacity) {
        return mix(previous, current, clamp(blend, 0.0, 1.0)) * opacity;
    }
)GLSL";

} // namespace AetherSDR::WeatherRadarTexture
