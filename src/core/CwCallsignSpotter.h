#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// Watches a decoded-CW character stream for a station identifying itself
// ("DE KI6BCJ KI6BCJ") and emits the callsign once per station.
//
// CW arrives a few characters at a time with decode errors sprinkled in,
// so detection runs over a small rolling window of recent text and only
// fires when the callsign after DE appears twice in a row — a single
// garbled character breaks the repeat and correctly suppresses the spot.
// The same spotter can watch any text stream that identifies stations
// this way (the SSB voice-callsign decoder feeds its own path).
class CwCallsignSpotter : public QObject {
    Q_OBJECT

public:
    explicit CwCallsignSpotter(QObject* parent = nullptr);

public slots:
    // Slots so the automation bridge can drive detection end-to-end via
    // QMetaObject::invokeMethod (`qrz spottext`) without a GUI include.

    // Append decoded text (any chunk size). May emit callsignSpotted.
    void feedText(const QString& text);

    // Drop the rolling window — call on slice change / decoder reroute /
    // panel clear so text from two stations never concatenates.
    void clear();

signals:
    void callsignSpotted(const QString& call);

private:
    void scanWindow(bool streamSettled);
    void emitSpot(const QString& call);

    QString m_window;                       // recent decoded text, uppercase
    QHash<QString, qint64> m_lastSpotUtc;   // per-call re-emit suppression
    QTimer  m_settleTimer;                  // accepts an end-of-window match after silence

    static constexpr int    kWindowChars   = 160;
    static constexpr qint64 kReSpotSecs    = 120;  // same call fires again after this
    static constexpr int    kSettleMs      = 3000;
};

} // namespace AetherSDR
