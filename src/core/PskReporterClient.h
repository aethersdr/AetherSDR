#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

class MqttClient;

// One reception report of our transmitted signal.
struct PskReporterSpot {
    QString receiverCallsign;
    QString receiverLocator;
    QString senderCallsign;
    QString senderLocator;
    QString mode;
    qint64  frequencyHz{0};
    int     snr{-999};        // dB, -999 = not reported
    qint64  flowStartSeconds{0};
};

// Fetches reception reports of our callsign from pskreporter.info.
//
// Two transports:
//   * HTTP polling of https://retrieve.pskreporter.info/query — XML
//     receptionReport records. PSK Reporter policy: poll no more often
//     than once every five minutes, so the interval is clamped to >= 5
//     minutes and `lastseqno` is used so repeat polls are incremental.
//     There is deliberately NO manual-refresh path.
//   * Live MQTT (mqtt.pskreporter.info, TLS) — the officially sanctioned
//     real-time feed; used when intervalMs == kLiveMqtt.
class PskReporterClient : public QObject {
    Q_OBJECT

public:
    static constexpr int kMinPollMs = 5 * 60 * 1000;   // PSK Reporter policy
    static constexpr int kLiveMqtt  = -1;              // sentinel interval

    explicit PskReporterClient(QObject* parent = nullptr);
    ~PskReporterClient() override;

    void setCallsign(const QString& callsign);
    QString callsign() const { return m_callsign; }

    // Lookback window (seconds): how far back spots are backfilled and
    // retained/displayed. Clamped to the PSK Reporter 24h API ceiling.
    void setLookbackSeconds(int seconds);
    int lookbackSeconds() const { return m_lookbackSec; }

    // intervalMs: kLiveMqtt for the MQTT live feed, otherwise a polling
    // period (clamped to kMinPollMs).
    void start(int intervalMs);
    void stop();
    bool isRunning() const { return m_running; }

    // Spots retained in the rolling lookback window, capped.
    const QVector<PskReporterSpot>& spots() const { return m_spots; }

    // Connection state for the UI indicator.
    bool isLive() const { return m_running && m_intervalMs == kLiveMqtt; }
    bool isMqttConnected() const;
    bool lastHttpOk() const { return m_lastHttpOk; }
    bool sawError() const { return m_sawError; }
    // "MQTT" when the live broker is connected, else "HTTP".
    QString transport() const;

signals:
    void spotsUpdated();                 // m_spots changed
    void statusChanged(const QString& status);
    void connectionStateChanged();       // transport/health changed

private slots:
    void poll();

private:
    void handleQueryReply(const QByteArray& xml);
    void handleMqttMessage(const QString& topic, const QByteArray& payload);
    void appendSpot(const PskReporterSpot& spot);
    void pruneOldSpots();
    void startMqtt();
    void stopMqtt();
    // Begin (or keep) HTTP polling as a fallback while MQTT is unavailable.
    void startFallbackPolling();

    // Disk persistence: spots survive a client restart within the tombstone
    // window (kSpotTtlSeconds) so the map repopulates immediately on reopen
    // over the course of a day, rather than waiting for fresh reports.
    QString cacheFilePath() const;
    void loadCache();
    void saveCache();

    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QTimer m_mqttHealthTimer;
    QTimer m_saveTimer;
    QString m_callsign;
    QVector<PskReporterSpot> m_spots;
    qint64 m_lastSeqNo{-1};
    int    m_intervalMs{kMinPollMs};
    int    m_lookbackSec{kDefaultLookbackSec};
    int    m_fetchedLookbackSec{0};  // deepest window backfilled this session
    bool   m_running{false};
    bool   m_fetchInFlight{false};
    bool   m_cacheDirty{false};
    bool   m_lastHttpOk{false};
    bool   m_sawError{false};
    MqttClient* m_mqtt{nullptr};

    // MQTT feed health counters, summarized to the log periodically.
    quint64 m_mqttMsgTotal{0};
    quint64 m_mqttMsgWindow{0};
    qint64  m_mqttLastMsgEpoch{0};

    static constexpr const char* kQueryUrl =
        "https://retrieve.pskreporter.info/query";
    static constexpr const char* kMqttHost = "mqtt.pskreporter.info";
    // Plain MQTT (1883), not TLS (1884): the feed is public, read-only spot
    // data with no credentials, so TLS adds nothing here — and relying on
    // OpenSSL's default cert store made TLS verification fail on shipped
    // macOS/Windows builds (no broker connection → no live spots). Plain
    // MQTT behaves identically on every platform with no CA handling.
    static constexpr quint16 kMqttPort = 1883;
    static constexpr int kMaxSpots = 2000;
    // HTTP fallback poll cadence when MQTT can't connect (port blocked etc.).
    static constexpr int kFallbackPollMs = 5 * 60 * 1000;
    // PSK Reporter's retrieval API caps flowStartSeconds at 24h; the lookback
    // window can't exceed that. Default to 1 hour.
    static constexpr int kMaxLookbackSec = 24 * 60 * 60;
    static constexpr int kDefaultLookbackSec = 60 * 60;
    // Throttle disk writes while spots stream in from the live feed.
    static constexpr int kSaveIntervalMs = 60 * 1000;
};

} // namespace AetherSDR
