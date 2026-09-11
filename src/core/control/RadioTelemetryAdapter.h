#pragma once

#include "ControlResourceStore.h"

#include <QElapsedTimer>
#include <QMap>
#include <QTimer>

#include <functional>
#include <optional>

namespace AetherSDR {
class RadioModel;
namespace control {

// A bounded latest-value projection, not a sample stream. All callbacks and
// publication run on the model's owning thread; no transport or intent lives here.
class RadioTelemetryAdapter final : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxMeters = 64;
    static constexpr int kPublishIntervalMs = 100;
    static constexpr int kStaleAfterMs = 2000;

    RadioTelemetryAdapter(RadioModel* radio, ControlResourceStore* store, QString sessionId,
                          QObject* parent = nullptr, std::function<qint64()> clockMs = {});
    ~RadioTelemetryAdapter() override;
    QJsonObject meterDelivery() const;
    // Same bounded pass used by the timer; the clock can be injected for
    // deterministic, socket-free freshness and coalescing tests.
    void flush();

signals:
    void deliveryChanged();

private:
    struct Meter {
        QJsonObject definition;
        std::optional<float> sample;
        qint64 sampledAt{0};
        bool swr{false};
    };
    void define(int index);
    void sample(int index, float value);
    void clear();
    void resetConnection();
    void publishTransmit();
    qint64 now() const;

    RadioModel* m_radio;
    ControlResourceStore* m_store;
    QString m_sessionId;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::function<qint64()> m_clockMs;
    QMap<int, Meter> m_meters;
    std::optional<bool> m_confirmedTransmit;
    bool m_limited{false};
    bool m_canTransmit{false};
};
} // namespace control
} // namespace AetherSDR
