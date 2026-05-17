#pragma once

#include "Ax25DecodedFrame.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace AetherSDR {

enum class Ax25TonePolarity {
    Normal,
    Inverted,
};

struct Ax25DemodConfig {
    Ax25ModemProfile profile{Ax25ModemProfile::Hf300};
    int sampleRate{24000};
    int baud{300};
    double markHz{1600.0};
    double spaceHz{1800.0};
    Ax25TonePolarity polarity{Ax25TonePolarity::Normal};
};

struct Ax25DecoderDiagnostics {
    int sampleRate{0};
    int audioSamples{0};
    double rmsDbfs{-120.0};
    double peakDbfs{-120.0};
    double clippedPercent{0.0};
    double markToneHz{0.0};
    double spaceToneHz{0.0};
    double markToneDbfs{-120.0};
    double spaceToneDbfs{-120.0};
    double markMinusSpaceDb{0.0};
    double receiveGateRmsDbfs{-120.0};
    double receiveGateFloorDbfs{-120.0};
    bool receiveGateOpen{false};
    quint64 receiveGateResets{0};
    int decodeLanes{1};
    int demodSymbols{0};
    double averageConfidence{0.0};
    double onesPercent{0.0};
    bool searching{true};
    bool inPreamble{false};
    bool inFrame{false};
    bool aborted{false};
    int currentFrameBits{0};
    int lastFrameBits{0};
    int preambleFlags{0};
    quint64 hdlcFrameStarts{0};
    quint64 hdlcFrameCandidates{0};
    quint64 plausibleAx25Candidates{0};
    quint64 framesAccepted{0};
    quint64 decodeRejected{0};
    quint64 rejectTooShort{0};
    quint64 rejectBadFcs{0};
    quint64 rejectMalformed{0};
    QString lastRejectReason;
    QString lastRejectPreviewHex;
    QString lastRejectActualFcs;
    QString lastRejectExpectedFcs;
    int lastRejectFrameBits{0};
    int lastRejectFrameBytes{0};
};

struct Ax25TransmitFrame {
    QString source;
    QString destination;
    QStringList path;
    quint8 control{0x03};
    quint8 pid{0xf0};
    QByteArray payload;
    QString payloadText;
    QString payloadHex;
};

struct Ax25TransmitResult {
    bool ok{false};
    QString error;
    Ax25TransmitFrame frame;
    QByteArray stereoFloat32Pcm;
    int sampleRate{0};
    int baud{0};
    double markHz{0.0};
    double spaceHz{0.0};
    Ax25TonePolarity polarity{Ax25TonePolarity::Normal};
    int preambleFlags{0};
    int postambleFlags{0};
    int frameBytes{0};
    int bitCount{0};
    int audioFrames{0};
    int vitaPacketFrames{0};
    double durationSeconds{0.0};
    double rmsDbfs{-120.0};
    double peakDbfs{-120.0};
};

Ax25DemodConfig ax25DemodConfigForProfile(
    Ax25ModemProfile profile,
    Ax25TonePolarity polarity = Ax25TonePolarity::Normal);
QString ax25ModemProfileName(Ax25ModemProfile profile);

class AetherAx25LibmodemShim : public QObject {
    Q_OBJECT

public:
    explicit AetherAx25LibmodemShim(QObject* parent = nullptr);
    ~AetherAx25LibmodemShim() override;

    Ax25DemodConfig config() const;
    void configure(const Ax25DemodConfig& config);
    void reset();
    void setDiagnosticsLoggingEnabled(bool enabled);
    bool diagnosticsLoggingEnabled() const;

    QVector<Ax25DecodedFrame> processMonoFloat(const float* samples,
                                               int sampleCount,
                                               int sampleRate);
    QVector<Ax25DecodedFrame> processRecoveredBitsForTest(const QVector<quint8>& bits,
                                                          double quality = 1.0);
    Ax25TransmitResult buildTransmitAudio(const QString& text,
                                          const QString& defaultSource,
                                          const QString& defaultDestination = QStringLiteral("APRS")) const;

    Ax25DecoderDiagnostics diagnosticsSnapshot() const;
    QString demodDescription() const;

public slots:
    void feedAudio(const QByteArray& monoFloat32Pcm, int sampleRate);

signals:
    void frameDecoded(const AetherSDR::Ax25DecodedFrame& frame);
    void diagnosticsUpdated(const AetherSDR::Ax25DecoderDiagnostics& diagnostics);
    void statusChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Ax25DecoderDiagnostics)
