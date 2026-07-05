#pragma once

#include <functional>

#include "core/backends/IRadioBackend.h"

namespace AetherSDR {

class RadioConnection;

// FlexBackend — the first IRadioBackend implementor (aetherd RFC step 2.2),
// wrapping the SmartSDR / FlexRadio wire stack.
//
// This is the introductory SKELETON. It is constructed, owned, and torn down by
// RadioModel inside the real connection lifecycle, and it observes live wire
// lifecycle events (re-emitting them as the interface's connected/disconnected/
// connectionError). It does NOT yet:
//   * absorb ownership of RadioConnection / PanadapterStream or their worker
//     threads (the #502 ASAN teardown ordering is the migration's single
//     riskiest move — its own verified increment, 2.2b), nor
//   * reroute the command hot path (sendCmd) or the slice-0 RX data plane.
// So this change is purely ADDITIVE and behavior-neutral: nothing above the
// seam is rerouted through the backend yet.
//
// The canonical core verbs (setSliceFrequency/Mode/Filter, setKeying) build the
// exact SmartSDR command strings and emit them through the model-provided
// command sink. They are correct but not yet on the live path — the touchpoint
// burndown (2.3) routes each model through them one at a time.
class FlexBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit FlexBackend(QObject* parent = nullptr);
    ~FlexBackend() override;

    // ---- transitional wiring (2.2: RadioModel still owns these objects) ----
    // Observe the connection's lifecycle signals (non-owning; the connection
    // lives on its worker thread, so these arrive via queued connections).
    void attachConnection(RadioConnection* conn);
    // Where the core verbs emit their SmartSDR command strings — RadioModel's
    // existing sendCommand() funnel, so verbs reuse the one wire-write path.
    void setCommandSink(std::function<void(const QString&)> sink);
    // Live model-string source, for capabilities() (avoids caching a value that
    // changes when the `model` status arrives).
    void setModelProvider(std::function<QString()> provider);

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

private:
    void send(const QString& cmd);

    RadioConnection* m_connection{nullptr};   // non-owning in 2.2 (RadioModel owns it)
    std::function<void(const QString&)> m_sink;
    std::function<QString()> m_modelProvider;
};

}  // namespace AetherSDR
