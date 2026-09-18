#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>

#include <functional>
#include <memory>

namespace AetherSDR {

// Shared across map managers, projections and dialog lifetimes. No settings,
// sockets or timers: tests inject monotonic time instead of waiting minutes.
class MapProviderRetryPolicy {
public:
    // Consumer timers must cover the initial cooldown including positive jitter.
    static constexpr int kConsumerRetryMs = 66000;
    struct Admission {
        qint64 delayMs{0};
        quint64 generation{0};
        bool probe{false};
    };
    explicit MapProviderRetryPolicy(std::function<qint64()> clock = {});
    static QString provider(const QUrl& url);
    static qint64 retryAfterMs(const QByteArray& value, const QDateTime& now);
    Admission admit(const QUrl& url);
    void complete(const QUrl& url, Admission admission, bool failed, bool canceled,
                  const QByteArray& retryAfter = {},
                  const QDateTime& wallNow = QDateTime::currentDateTimeUtc());

private:
    struct State {
        qint64 until{0};
        unsigned failures{0};
        quint64 generation{0};
        bool probeInFlight{false};
    };
    qint64 now() const;
    QElapsedTimer m_elapsed;
    std::function<qint64()> m_clock;
    QMutex m_mutex;
    QHash<QString, State> m_states;
};

// Only the explicitly named NASA and weather imagery hosts are gated. Other
// map providers retain the existing QNetworkAccessManager behavior. During a
// cooldown a local asynchronous error feeds existing UI retry/status paths;
// it never starts HTTP or restarts/extends the provider's cooldown.
class MapProviderNetworkAccessManager : public QNetworkAccessManager {
public:
    inline static std::function<QNetworkReply*(const QNetworkRequest&, QObject*)> radarRequestHandler;
    explicit MapProviderNetworkAccessManager(QObject* parent = nullptr,
        std::shared_ptr<MapProviderRetryPolicy> policy = {});

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                QIODevice* outgoingData = nullptr) override;
    // Injected-reply seam; production delegates to Qt's real transport/cache.
    virtual QNetworkReply* sendRequest(Operation operation, const QNetworkRequest& request,
                                      QIODevice* outgoingData);

private:
    std::shared_ptr<MapProviderRetryPolicy> m_policy;
};

} // namespace AetherSDR
