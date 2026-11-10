#pragma once

#include "core/WsprAgent.h"

#include <QHostAddress>
#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>

class QTimer;

namespace AetherSDR {

class RadioModel;
class WsprAgentClient;

// Keeps the WSPR receiver's slice out of AetherSDR's own. The radio reports
// every headless slice AetherWSPR makes as owned by the GUI client then
// connected, and hands it to the next GUI client to connect when none was.
// This asks the receiver which slice is its own and what its client handle
// is (its /v1/status, from AetherWSPR 0.1.3 on), and tells RadioModel
// (setExternalSliceClient), which shows that slice as another client's.
//
// It asks when a LAN connection holds its slices (RadioModel's connect
// hold, released on the answer or by RadioModel's timeout) and every
// kRefreshMs while connected, which catches a reinstalled receiver with a
// new handle. Slices the receiver makes in between RadioModel recognizes by
// their status's source handle. Over SmartLink the receiver is out of reach
// and nothing is held or marked.
class WsprSliceGuard : public QObject {
    Q_OBJECT

public:
    static constexpr int kHoldMs = 2000;
    static constexpr int kRefreshMs = 60000;

    // The receiver's status at an address, or nullopt when it doesn't answer.
    using StatusSource = std::function<void(const QHostAddress& radio,
                                            std::function<void(std::optional<WsprStatus>)> done)>;

    explicit WsprSliceGuard(RadioModel* radio, QObject* parent = nullptr);

    // Tests: replaces the HTTP query.
    void setStatusSource(StatusSource source);

private:
    void query();
    void apply(const std::optional<WsprStatus>& status);

    QPointer<RadioModel> m_radio;
    WsprAgentClient* m_client{nullptr};
    StatusSource m_source;
    QTimer* m_refresh{nullptr};
    quint64 m_generation{0};   // answers to an older query are dropped
};

} // namespace AetherSDR
