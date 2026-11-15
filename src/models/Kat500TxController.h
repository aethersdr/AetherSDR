#pragma once

#include "TxController.h"
#include "Kat500Types.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace AetherSDR {
class Kat500Model;
class RadioModel;

// Dedicated KAT500 pre-tune sequence. A captured Tune input is carried through
// every asynchronous step; status and timers can never manufacture TX authority.
class Kat500TxController final : public QObject {
    Q_OBJECT
public:
    Kat500TxController(RadioModel* radio, Kat500Model* tuner, QObject* parent = nullptr);
    ~Kat500TxController() override;
    bool selected() const { return m_selected; }
    bool busy() const { return m_phase != Phase::Idle; }
    bool matched() const;
    QString unavailableReason() const;
    QString message() const { return m_message; }
    bool start(const TxController::Input& carrier);
    void cancel();

signals:
    void changed();
    void tunerDisconnected();

private:
    enum class Phase { Idle, Bypass, Frequency, Search, Carrier, Result, Cancelling };
    void refresh();
    void publish();
    bool matchContextCurrent() const;
    void ensureBypass();
    bool internalBypassed() const;
    bool originalIntentValid() const;
    void recallFrequency();
    void beginSearch();
    void operationFinished();
    void telemetry(const QString& command);
    void stop(const QString& reason, bool cancelSearch);
    void pollCancellation();
    void setPhase(Phase phase, int timeoutMs);

    QPointer<RadioModel> m_radio;
    QPointer<Kat500Model> m_tuner;
    TxController::Input m_carrier;
    QTimer m_deadline;
    QTimer m_watchdog;
    Phase m_phase{Phase::Idle};
    bool m_selected{false};
    bool m_refreshing{false};
    bool m_bypassRequested{false};
    bool m_ownsOperation{false};
    bool m_searchRequested{false};
    bool m_cancelPending{false};
    bool m_completed{false};
    int m_frequencyKhz{0};
    QString m_serial;
    QString m_message;
    Kat500::Status m_matchContext;
    using Presentation = std::tuple<bool, bool, bool, bool, std::optional<Kat500::Mode>,
                                    std::optional<bool>, bool, QString, QString>;
    std::optional<Presentation> m_presentation;
};
} // namespace AetherSDR
