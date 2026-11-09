#pragma once

#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace AetherSDR {
class NetworkRadioLookup final : public QObject {
public:
    enum class Error { None, InvalidHost, NoIpv4Address, TimedOut };
    struct Result { QHostAddress address; Error error = Error::None; };
    using Completion = std::function<void(Result)>;
    using Resolver = std::function<int(const QString&, QObject*, std::function<void(const QHostInfo&)>)>;
    using Canceller = std::function<void(int)>;

    explicit NetworkRadioLookup(QObject* parent = nullptr);
    NetworkRadioLookup(Resolver resolver, Canceller canceller, int timeoutMs, QObject* parent = nullptr);
    ~NetworkRadioLookup() override;
    void start(const QString& host, Completion completion);
    void cancel();

private:
    void finish(quint64 generation, Result result, bool abortLookup = false);
    Resolver m_resolver;
    Canceller m_canceller;
    int m_timeoutMs;
    QTimer m_timer;
    QMetaObject::Connection m_timeoutConnection;
    quint64 m_generation = 0;
    int m_lookupId = -1;
    bool m_active = false;
    Completion m_completion;
};
} // namespace AetherSDR
