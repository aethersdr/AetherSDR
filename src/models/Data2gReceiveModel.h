#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace AetherSDR {

// One Data2G burst as the monitor heard it. Everything in it came off the
// air: treat the strings as untrusted text, never as markup.
struct Data2gHeardBurst {
    QDateTime heardUtc;   // when the decode finished
    QString head;         // description lines, each ending in '\n'
    QString text;         // head plus payloads, rendered for a log view
};

// Receive-only Data2G monitor for AetherModem (RFC #7159, Stage 1).
//
// Takes the modem's continuous 24 kHz mono receive audio and reports every
// Data2G burst heard on it. Decoding runs on this model's own thread; the
// model itself lives on its owner's thread and all its signals are emitted
// there.
//
// It owns no audio source, no sound device and no transmit authority, and
// nothing reachable from it can key a radio.
//
// In a build without the Data2G core (ENABLE_DATA2G=OFF) the model still
// exists, available() is false, and it never reports a burst.
class Data2gReceiveModel final : public QObject {
    Q_OBJECT
public:
    // Audio held for the decoder, at most. A Data2G burst runs up to 16 s and
    // is decoded only once it has arrived whole, so the bound is in seconds
    // of audio, not blocks. Past it the queue is dropped and the monitor
    // starts clean (counted in Stats::overflows).
    static constexpr int kSampleRateHz = 24000;
    static constexpr int kMaxQueuedSeconds = 30;

    struct Stats {
        quint64 bursts = 0;      // bursts reported
        quint64 overflows = 0;   // times the queue bound was hit
        quint64 faults = 0;      // decodes the core rejected
    };

    explicit Data2gReceiveModel(QObject* parent = nullptr);
    ~Data2gReceiveModel() override;

    static bool available();
    static QString upstreamVersion();   // empty when unavailable

    void setEnabled(bool enabled);
    bool isEnabled() const;

    // Mono float32 at kSampleRateHz, in blocks of any size. `discontinuity`
    // means this block does not follow the previous one. Ignored while
    // disabled.
    void feed(const QVector<float>& mono, bool discontinuity = false);

    // The audio is no longer one stream (source change, slice change): drop
    // what is queued, forget a burst in progress and the sessions learned.
    void reset();

    Stats stats() const;

signals:
    void burstHeard(const AetherSDR::Data2gHeardBurst& burst);
    void statsChanged();
    void enabledChanged(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Data2gHeardBurst)
