#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "core/IqProvider.h"

namespace AetherSDR {

class DaxIqModel;

// FlexRadio's DAX-IQ plane behind the backend-neutral seam (#4955).
//
// Everything Flex-shaped about TCI IQ lives here and nowhere else: the
// `stream create type=dax_iq` lifecycle, the `display pan set <pan>
// daxiq_channel=N` binding that is the only reason samples ever flow, the
// per-stream rate, and the little-endian float32 payload the radio sends for
// this stream type alone (pan/wf/meter/audio are big-endian; reading dax_iq the
// same way turns every float into a denormal and the skimmer sees a flat, dead
// stream — #3913).
//
// OWNERSHIP, which is the part that is easy to get wrong. A DAX-IQ channel may
// already be bound to this pan because the DAX-IQ applet or the WFM demodulator
// created it. This adapter BORROWS such a stream and, on the last release,
// detaches without removing it. It removes only a stream it created itself.
class FlexIqProvider : public IqProvider {
    Q_OBJECT

public:
    using CommandSink      = std::function<void(const QString&)>;
    using PanLister        = std::function<QStringList()>;
    using PanCenterHzOfPan = std::function<long long(const QString&)>;

    explicit FlexIqProvider(QObject* parent = nullptr);

    // Wiring. All optional: a provider with no DAX model and no pan lister
    // simply has no endpoints, which is the correct answer for a session that
    // has not connected yet.
    void setDaxIqModel(DaxIqModel* dax) { m_dax = dax; }
    void setCommandSink(CommandSink sink) { m_sendCommand = std::move(sink); }
    void setPanResolvers(PanLister lister, PanCenterHzOfPan center);
    void setConnected(bool on);

    IqProviderCaps      capabilities() const override;
    QVector<IqEndpoint> endpoints() const override;

    // PanadapterStream::iqDataReady. `daxChannel` is 1..4.
    void feedRawIqPacket(int daxChannel, const QByteArray& lePayload, int sampleRateHz);

    // The radio went away. Every lease is invalidated rather than left pointing
    // at a channel that will be re-created for someone else on reconnect.
    void handleDisconnect();

    // The DAX-IQ channels this radio family exposes.
    static constexpr int kNumChannels = 4;

protected:
    IqLease openEndpoint(const QString& endpointId, int requestedRateHz) override;
    void    closeEndpoint(const QString& endpointId) override;
    IqLease adjustRate(const QString& endpointId, int requestedRateHz) override;

private:
    struct Binding {
        int  channel{0};      // 1..4
        bool owned{false};    // we created the radio-side stream, so we remove it
    };

    int  currentRateFor(int channel) const;
    long long centerFor(const QString& panId) const;

    DaxIqModel*      m_dax{nullptr};
    CommandSink      m_sendCommand;
    PanLister        m_panLister;
    PanCenterHzOfPan m_panCenter;
    bool             m_connected{false};

    QHash<QString, Binding> m_bindings;    // pan id → DAX-IQ channel
    QHash<int, QString>     m_channelPan;  // and back, for the sample path
};

}  // namespace AetherSDR
