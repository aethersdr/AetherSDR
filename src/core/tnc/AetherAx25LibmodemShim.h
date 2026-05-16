#pragma once

#include "Ax25DecodedFrame.h"

#include <QByteArray>
#include <QObject>
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

    QVector<Ax25DecodedFrame> processMonoFloat(const float* samples,
                                               int sampleCount,
                                               int sampleRate);
    QVector<Ax25DecodedFrame> processRecoveredBitsForTest(const QVector<quint8>& bits,
                                                          double quality = 1.0);

    QString demodDescription() const;

public slots:
    void feedAudio(const QByteArray& monoFloat32Pcm, int sampleRate);

signals:
    void frameDecoded(const AetherSDR::Ax25DecodedFrame& frame);
    void statusChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
