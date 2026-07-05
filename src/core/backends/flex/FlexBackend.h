#pragma once

#include <functional>

#include "core/backends/IRadioBackend.h"

class QThread;

namespace AetherSDR {

class RadioConnection;
class PanadapterStream;

// FlexBackend — the first IRadioBackend implementor (aetherd RFC step 2),
// wrapping the SmartSDR / FlexRadio wire stack.
//
// As of 2.2b it OWNS the wire objects: the RadioConnection and PanadapterStream
// plus their two worker threads, created here in the exact order RadioModel
// used (panStream thread first, connection thread second) and torn down here in
// the exact #502 order (BlockingQueued stop → deleteLater → thread quit/wait).
// RadioModel holds non-owning pointers it obtains via connection()/panStream()
// and keeps its command/WAN orchestration and its sub-models — so the move is
// ownership-only and behavior-neutral.
//
// The canonical core verbs build the exact SmartSDR command strings and emit
// them through the model-provided command sink; they grow onto the live path as
// the touchpoint burndown converts each model (2.3).
class FlexBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit FlexBackend(QObject* parent = nullptr);
    ~FlexBackend() override;

    // The owned wire objects, on their worker threads. Non-owning to callers;
    // valid for the backend's lifetime. RadioModel grabs these post-construction
    // and its connection()/panStream() getters delegate to them.
    RadioConnection*  connection() const { return m_connection; }
    PanadapterStream* panStream()  const { return m_panStream; }

    // Where the core verbs emit their SmartSDR command strings — RadioModel's
    // existing sendCommand() funnel, so verbs reuse the one wire-write path.
    void setCommandSink(std::function<void(const QString&)> sink);
    // Live model-string source for capabilities(). Call on the main thread,
    // where RadioModel lives — capabilities() is not thread-safe (no off-thread
    // caller exists yet; this documents the assumption).
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

    RadioConnection*  m_connection{nullptr};    // owned; lives on m_connThread
    QThread*          m_connThread{nullptr};    // owned (this-parented)
    PanadapterStream* m_panStream{nullptr};     // owned; lives on m_networkThread
    QThread*          m_networkThread{nullptr}; // owned (this-parented)
    std::function<void(const QString&)> m_sink;
    std::function<QString()> m_modelProvider;
};

}  // namespace AetherSDR
