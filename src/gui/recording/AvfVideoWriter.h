#pragma once
#include "INativeVideoWriter.h"

namespace AetherSDR {

class AvfVideoWriter : public INativeVideoWriter {
public:
    AvfVideoWriter();
    ~AvfVideoWriter() override;

    bool open(const QString& filePath, int width, int height, int fps) override;
    void close() override;
    bool writeVideoFrame(const QImage& frame, qint64 ptsUs) override;
    bool writeAudioSamples(const QByteArray& pcmData, qint64 ptsUs) override;

private:
    // ARC +1 ownership of AVFWriterObjC via CFBridgingRetain (see .mm).
    void* m_opaqueWriter{nullptr};
};

} // namespace AetherSDR
