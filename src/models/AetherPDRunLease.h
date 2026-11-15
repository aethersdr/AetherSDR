#pragma once

#include "core/txlinearity/TxLinearityController.h"
#include "core/txlinearity/AetherPDTestClient.h"
#include "core/TxGrantManager.h"
#include "core/backends/RadioControlTypes.h"
#include "models/TxController.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace AetherSDR {
class RadioModel;

// Trusted engine composition. Creation only binds an unused helper; start is
// the explicit operator boundary. All methods/destruction are owner-thread
// only. This lease never arms a calibration source.
class AetherPDRunLease final : public QObject, public txlin::ITxLinearityRunLease,
                               public std::enable_shared_from_this<AetherPDRunLease> {
    struct ConstructionKey {};
public:
    static std::shared_ptr<AetherPDRunLease> create(RadioModel& radio, RadioControlSession session,
        std::unique_ptr<txlin::AetherPDTestClient> helper = {}, std::function<bool()> runCurrent = {});
    AetherPDRunLease(ConstructionKey, RadioModel& radio, RadioControlSession session,
                     std::unique_ptr<txlin::AetherPDTestClient> helper, std::function<bool()> runCurrent);
    ~AetherPDRunLease() override;
    // Absolute deadline uses TxCoordinator::monotonicMs(), also used by the controller.
    bool start(std::int64_t absoluteDeadlineMs) override;
    void release() override;
    State state() const override;
    void poll();
    const QString& error() const { return m_error; }

private:
    struct Gate {
        std::atomic<bool> retired{false};
        std::atomic<qint64> deadline{0};
        bool current();
    };
    bool current() const;
    void fail(const QString& error);
    QPointer<RadioModel> m_radio;
    RadioControlSession m_session;
    std::unique_ptr<txlin::AetherPDTestClient> m_helper;
    std::function<bool()> m_current;
    QPointer<TxGrantManager> m_manager;
    TxGrantManager::Client m_client;
    TxGrantManager::Grant m_grant;
    TxCoordinator::Operation m_operation;
    TxController::Input m_input;
    std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();
    QTimer m_timer;
    QString m_error;
    qint64 m_deadline{0};
    quint32 m_handle{0};
    bool m_started{false};
    bool m_admitted{false};
    bool m_keyRequested{false};
    bool m_keyObserved{false};
    bool m_releasing{false};
    bool m_polling{false};
};
} // namespace AetherSDR
