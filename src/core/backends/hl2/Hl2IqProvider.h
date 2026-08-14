#pragma once

#include <QString>
#include <QVector>

#include <complex>
#include <mutex>
#include <vector>

#include "core/IqProvider.h"

namespace AetherSDR {

// The Hermes-Lite 2's IQ endpoints (#4955).
//
// The HL2 has no DAX stream plane and no radio-side stream lifecycle, so its
// capabilities honestly report hasDaxStreams=false. What it DOES have is a
// Metis receive path that already demultiplexes one complex block per active
// DDC on every EP6 packet, for the panadapter and the host DSP. This adapter
// taps that same fan-out.
//
// Three things it must get right, each of which has already bitten this
// codebase once:
//
//  1. HANDEDNESS. HPSDR wire IQ is the conjugate of the analytic convention.
//     feedWireBlock() conjugates exactly once, here at the transport boundary.
//  2. CENTRE. The reported centre is the DDC's NCO — the pan centre — not the
//     slice's tuned frequency. On this backend the slice is shifted inside the
//     passband and the NCO moves only when the target would leave the window,
//     so the two are routinely far apart.
//  3. IDENTITY. A lease is held on the stable pan id. The wire DDC index is
//     contiguous and renumbers on every receiver rebuild; publishRoutes()
//     invalidates a lease whose pan has gone rather than letting it slide onto
//     whichever receiver inherited its slot.
//
// Opening an endpoint starts nothing and closing one stops nothing: the DDC is
// already running because the application's own panadapter and DSP chain are
// using it. That is the whole point of a tap — a TCI client's iq_stop must not
// tear down a receiver the operator is still looking at.
class Hl2IqProvider : public IqProvider {
    Q_OBJECT

public:
    // One running receiver, index-parallel to the wire: routes[i] describes
    // DDC i. Published by Hl2Backend whenever the receiver set, the shared rate
    // or an NCO moves.
    struct Route {
        QString   endpointId;   // the stable pan id, e.g. "hl2-0"
        long long centerHz{0};  // the DDC's NCO
    };

    explicit Hl2IqProvider(QObject* parent = nullptr);

    IqProviderCaps      capabilities() const override;
    QVector<IqEndpoint> endpoints() const override;

    // Backend → provider. GUI thread.
    void publishRoutes(const QVector<Route>& routes, int sampleRateHz,
                       int boardReceiverLimit);
    void setConnected(bool on);

    // Wire → provider. Called on the HL2 I/O thread, once per DDC per EP6
    // packet, with the buffer MetisClient reuses — so it copies.
    void feedWireBlock(int ddcIndex, const std::vector<std::complex<float>>& wireIq);

protected:
    IqLease openEndpoint(const QString& endpointId, int requestedRateHz) override;
    void    closeEndpoint(const QString& endpointId) override;
    IqLease adjustRate(const QString& endpointId, int requestedRateHz) override;

private:
    mutable std::mutex m_routeMutex;   // guards everything below; read on the
                                       // I/O thread by feedWireBlock()
    QVector<Route> m_routes;
    int  m_sampleRateHz{48000};
    int  m_boardReceiverLimit{1};
    bool m_connected{false};
};

}  // namespace AetherSDR
