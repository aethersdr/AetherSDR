#pragma once
#ifdef HAVE_WEBSOCKETS
#include <QObject>
#include <QByteArray>
#include <memory>

namespace AetherSDR {
class RadioModel;
class TciServer;
class PcmFrame;

// Owner-thread receive adapter for the existing virtual audio bridges. Uses
// TCI's live receiver bindings even while its listener is stopped. Owns no
// device, transport, thread or PCM queue; output is float32 stereo at 24 kHz.
class DaxReceiveModel final : public QObject {
    Q_OBJECT
public:
    DaxReceiveModel(RadioModel& model, TciServer& routing, QObject* parent = nullptr);
    ~DaxReceiveModel() override;
    void setEnabled(bool enabled);

signals:
    void audioReady(int channel, const QByteArray& stereo24);
    void channelReset(int channel);
    void channelSliceChanged(int channel, int sliceId);

private:
    void refreshBindings();
    void receive(int sliceId, const PcmFrame& frame);
    struct State;
    std::unique_ptr<State> m_state;
};
}
#endif
