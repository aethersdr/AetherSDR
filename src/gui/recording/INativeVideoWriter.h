#pragma once
#include <QString>
#include <QImage>
#include <QByteArray>

namespace AetherSDR {

class INativeVideoWriter {
public:
    virtual ~INativeVideoWriter() = default;

    // Initializes the file writer, dimensions, and audio format
    virtual bool open(const QString& filePath, int width, int height, int fps) = 0;
    
    // Finalizes and closes the file container
    virtual void close() = 0;

    // Writes an uncompressed frame to the video stream
    virtual bool writeVideoFrame(const QImage& frame, qint64 ptsUs) = 0;

    // Writes stereo int16 PCM audio samples to the audio stream
    virtual bool writeAudioSamples(const QByteArray& pcmData, qint64 ptsUs) = 0;
};

} // namespace AetherSDR
