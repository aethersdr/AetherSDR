#pragma once

#include "core/TxCoordinator.h"
#include "TransmitModel.h"

#include <QPointer>
#include <array>
#include <functional>

namespace AetherSDR {

class RadioModel;
class CwxModel;

// One trusted input/authorization lifetime. This is a producer of the desktop
// compatibility actor, not another arbiter or a remote transmit grant. Capture
// an Input at the operator boundary and carry that value through queued work.
// Neither a queued callback nor a reconnect can renew its authority.
class TxController final {
    struct InputScopeTag {};
    struct NativeDeviceTag {};
public:
    using Activity = TxCoordinator::Activity;
    class Input {
    public:
        [[nodiscard]] bool valid() const;
        [[nodiscard]] bool belongsTo(const RadioModel* radio) const;
        [[nodiscard]] bool belongsTo(const CwxModel* model) const;
        [[nodiscard]] bool active() const;
        [[nodiscard]] bool start(bool twoTone = false) const;
        [[nodiscard]] bool send(const QString& text, std::function<void()> admitted = {}) const;
        [[nodiscard]] bool sendMacro(int index, std::function<void()> admitted = {}) const;
        [[nodiscard]] bool sendChar(const QString& text) const;
        void stop() const;
        void abort() const;
        [[nodiscard]] bool bypassAtu() const;
        [[nodiscard]] TxCoordinator::Context media() const;
        [[nodiscard]] TxCoordinator::Request request() const { return m_request; }
        [[nodiscard]] Input derive() const;
    private:
        friend class TxController;
        QPointer<RadioModel> m_radio;
        TxCoordinator::Request m_request;
        Activity m_activity{Activity::Mox};
        TransmitModel::PttSource m_source{TransmitModel::PttSource::Mox};
    };

    explicit TxController(RadioModel* radio,
                          TransmitModel::PttSource source = TransmitModel::PttSource::Mox);
    TxController(InputScopeTag, std::shared_ptr<TxController> source, const TxCoordinator::Request& input);
    TxController(NativeDeviceTag, RadioModel* radio, QObject* device);
    ~TxController();
    TxController(const TxController&) = delete;
    TxController& operator=(const TxController&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool originalSessionCurrent() const;
    [[nodiscard]] bool sameController(const std::shared_ptr<TxController>& other) const;
    // A compound input (for example a double-click) may prepare its second
    // activation after the first changed the UI. Restrict every fresh request
    // it captures to the original input epoch, never to callback-time state.
    // This view owns no new producer and normal destruction does not cancel
    // already queued work; the source controller still owns that lifetime.
    static std::shared_ptr<TxController> captureInputScope(const std::shared_ptr<TxController>& source);
    static std::shared_ptr<TxController> captureInputScope(const std::shared_ptr<TxController>& source,
                                                         const TxCoordinator::Request& input);
    // Trusted native-device composition. Only raw-input capture is worker-safe;
    // the resulting scope and all action dispatch remain on the model thread.
    static std::shared_ptr<TxController> forNativeDevice(RadioModel* radio, QObject* device);
    [[nodiscard]] TxCoordinator::Request captureRawInput() const;
    void discardDeviceInputs() const;
    void cleanupDeviceInputs();
    [[nodiscard]] bool belongsTo(const RadioModel* radio) const;
    // Repeated input during an existing hold keeps its original request. A
    // released/refused request is never retried; fresh input gets a fresh one.
    [[nodiscard]] Input capture(Activity activity);
    // Capture a fresh sequence at its explicit start. Each scheduled item
    // derives from this unbound input; it cannot replace an existing hold.
    [[nodiscard]] Input captureProgram(Activity activity);
    [[nodiscard]] Input current(Activity activity) const;
    [[nodiscard]] bool hasWork() const;
    void setAdmissionObserver(std::function<void()> observer);
    // Terminal for this lifetime. Invalidate before cleanup so nested UI loops,
    // worker queues and later input cannot key again through this controller.
    void invalidate();

private:
    static std::size_t index(Activity activity);
    QPointer<RadioModel> m_radio;
    TxCoordinator::Producer m_producer;
    TxCoordinator::Request m_connection;
    TransmitModel::PttSource m_source;
    std::array<Input, 6> m_inputs;
    bool m_invalidating{false}; // owner-thread cleanup reentrancy guard
    bool m_nativeDevice{false};
    std::shared_ptr<TxController> m_scopeOwner;
    TxCoordinator::Request m_scopeInput;
};

} // namespace AetherSDR
