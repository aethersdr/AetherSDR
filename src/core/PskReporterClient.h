#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

class QNetworkReply;
class QJsonArray;

namespace AetherSDR {

class MqttClient;
class PskReporterClientTestAccess;

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

struct PskReporterMonitor {
    QString callsign;
    QString locator;
    QString mode;
    QString decoderSoftware;
    qint64 frequencyHz{0};
};

// Fetches reception reports for the selected map scope from pskreporter.info.
//
// Two transports:
//   * HTTP polling of https://retrieve.pskreporter.info/query — XML
//     receptionReport records. PSK Reporter policy: poll no more often
//     than once every five minutes, so the interval is clamped to >= 5
//     minutes and `lastseqno` is used so repeat polls are incremental.
//     There is deliberately NO manual-refresh path.
//   * Live MQTT (mqtt.pskreporter.info, TLS) — the officially sanctioned
//     real-time callsign feed; used when intervalMs == kLiveMqtt. The map's
//     separate global client owns the reusable all-stations snapshot.
class PskReporterClient : public QObject {
    Q_OBJECT

public:
    enum class QueryScope {
        Callsign,
        Anyone
    };

    static constexpr int kMinPollMs = 5 * 60 * 1000;   // PSK Reporter policy
    static constexpr int kLiveMqtt  = -1;              // sentinel interval

    explicit PskReporterClient(QObject* parent = nullptr);
    ~PskReporterClient() override;

    void setCallsign(const QString& callsign);
    QString callsign() const { return m_callsign; }
    QueryScope queryScope() const { return m_scope; }

    // A dedicated MQTT layer can disable its HTTP seed/fallback when a
    // separate all-stations client already owns the shared HTTP cadence.
    void setHttpPollingEnabled(bool enabled) { m_httpPollingEnabled = enabled; }

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
    const QVector<PskReporterMonitor>& monitors() const { return m_monitors; }
    bool resultsLimited() const { return m_resultsLimited; }

    // Connection state for the UI indicator.
    bool isLive() const { return m_running && m_intervalMs == kLiveMqtt; }
    bool isMqttConnected() const;
    bool lastHttpOk() const { return m_lastHttpOk; }
    bool sawError() const { return m_sawError; }
    bool httpRequestInFlight() const { return m_fetchInFlight; }
    int lastHttpStatus() const { return m_lastHttpStatus; }
    QString lastHttpError() const { return m_lastHttpError; }
    QDateTime lastHttpRequestAt() const;
    QDateTime nextHttpRequestAt() const;
    // "MQTT" when the live broker is connected, else "HTTP".
    QString transport() const;

signals:
    void spotsUpdated();                 // m_spots changed
    void statusChanged(const QString& status);
    void connectionStateChanged();       // transport/health changed

private slots:
    void poll();

private:
    friend class PskReporterClientTestAccess;

    struct ParsedHttpSnapshot {
        QVector<PskReporterSpot> spots;
        QVector<PskReporterMonitor> monitors;
        qint64 lastSeqNo{-1};
        int parsedReports{0};
        QString parseError;
    };

    void handleQueryReply(const QByteArray& xml, QueryScope responseScope);
    void handleMqttMessage(const QString& topic, const QByteArray& payload);
    void appendSpot(const PskReporterSpot& spot);
    static bool appendSpot(QVector<PskReporterSpot>& spots,
                           const PskReporterSpot& spot);
    void pruneOldSpots();
    void startMqtt();
    void stopMqtt();
    // Begin (or keep) HTTP polling as a fallback while MQTT is unavailable.
    void startFallbackPolling();
    void scheduleHttpPollAtAllowedTime();
    QString httpThrottleStateFilePath() const;
    void loadHttpThrottleState();
    void saveHttpThrottleState() const;
    static qint64 rateLimitBackoffMs(int consecutiveRateLimits);
    QueryScope httpQueryScope() const;
    static bool appendHttpResponseChunk(QByteArray& response,
                                        const QByteArray& chunk);
    static ParsedHttpSnapshot parseHttpSnapshot(const QByteArray& xml);
    void handleParsedHttpSnapshot(ParsedHttpSnapshot snapshot,
                                  QueryScope responseScope);

    // Disk persistence: spots survive a client restart within the tombstone
    // window (kSpotTtlSeconds) so the map repopulates immediately on reopen
    // over the course of a day, rather than waiting for fresh reports.
    QString cacheFilePath() const;
    QString cacheFilePath(QueryScope scope, const QString& callsign) const;
    void loadCache();
    void saveCache();
    void saveCacheSnapshot(QueryScope scope, const QString& callsign,
                           const QVector<PskReporterSpot>& spots,
                           const QVector<PskReporterMonitor>& monitors) const;
    void restoreCachedMonitors(const QJsonArray& monitorArray);

    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QTimer m_mqttHealthTimer;
    QTimer m_saveTimer;
    QTimer m_httpThrottleTimer;
    QString m_callsign;
    QueryScope m_scope{QueryScope::Callsign};
    QVector<PskReporterSpot> m_spots;
    QVector<PskReporterMonitor> m_monitors;
    qint64 m_lastSeqNo{-1};
    int    m_intervalMs{kMinPollMs};
    int    m_lookbackSec{kDefaultLookbackSec};
    int    m_fetchedLookbackSec{0};  // deepest window backfilled this session
    QString m_cacheDirectoryOverride;  // test isolation; empty in production
    bool   m_running{false};
    bool   m_httpPollingEnabled{true};
    bool   m_fetchInFlight{false};
    QPointer<QNetworkReply> m_queryReply;
    quint64 m_queryGeneration{0};
    bool   m_cacheDirty{false};
    bool   m_lastHttpOk{false};
    bool   m_sawError{false};
    bool   m_resultsLimited{false};
    int    m_lastHttpStatus{0};
    QString m_lastHttpError;
    qint64 m_lastHttpRequestEpochMs{0};
    qint64 m_nextHttpAllowedEpochMs{0};
    int m_consecutiveRateLimits{0};
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
    static constexpr int kMaxMonitors = 6000;
    // Bound the decompressed XML body, including activeReceiver rows whose
    // count is not limited by the PSK Reporter retrieval API.
    static constexpr qint64 kMaxHttpResponseBytes = 16 * 1024 * 1024;
    static constexpr qint64 kHttpReadBufferBytes = 64 * 1024;
    // HTTP fallback poll cadence when MQTT can't connect (port blocked etc.).
    static constexpr int kFallbackPollMs = 5 * 60 * 1000;
    static constexpr qint64 kRateLimitBaseMs = 15 * 60 * 1000;
    static constexpr qint64 kRateLimitMaxMs = 2 * 60 * 60 * 1000;
    // PSK Reporter's retrieval API caps flowStartSeconds at 24h; the lookback
    // window can't exceed that. Default to 1 hour.
    static constexpr int kMaxLookbackSec = 24 * 60 * 60;
    static constexpr int kDefaultLookbackSec = 60 * 60;
    // Throttle disk writes while spots stream in from the live feed.
    static constexpr int kSaveIntervalMs = 60 * 1000;
};

} // namespace AetherSDR
