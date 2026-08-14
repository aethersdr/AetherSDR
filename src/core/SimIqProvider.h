#pragma once

#include <QString>
#include <QVector>

#include "core/IqProvider.h"

namespace AetherSDR {

// A deterministic IQ provider with no radio behind it (#4955).
//
// It exists so the seam's contract — handedness, centre reporting, lease
// reference counting, rate negotiation, backpressure — can be tested on Linux,
// macOS and Windows with no hardware and no network. Every sample it produces
// is a closed-form function of its arguments: no clock, no RNG, no threads.
//
// It is a first-class provider rather than a test double, because the same
// object is what a future replay fixture would grow out of, and because a
// provider only ever exercised through mocks is a provider whose real contract
// nobody has run.
class SimIqProvider : public IqProvider {
    Q_OBJECT

public:
    explicit SimIqProvider(QObject* parent = nullptr);

    struct EndpointSpec {
        QString   id;
        long long centerHz{0};
    };

    void setEndpoints(const QVector<EndpointSpec>& eps);
    void removeEndpoint(const QString& id);        // receiver churn
    void setConnected(bool on);
    void setCapacity(int maxConcurrentEndpoints);
    // topology/rate policy under test: PerStream honours a supported request,
    // SharedAcrossReceivers and FixedObserved report the current rate instead.
    void setRatePolicy(const QVector<int>& supported, int achievedHz,
                       IqRateTopology topology, bool radioWide);

    IqProviderCaps      capabilities() const override;
    QVector<IqEndpoint> endpoints() const override;

    // A complex exponential at `offsetHz` FROM THE REPORTED CENTRE, in the
    // canonical orientation: a positive offset is RF above centre. This is the
    // reference a handedness test measures every adapter against.
    void emitTone(const QString& endpointId, double offsetHz, int sampleCount);
    // Arbitrary samples, for tests that need a specific waveform.
    void emitSamples(const QString& endpointId, std::vector<IqSample> samples);

    // Adapter-hook observation, so a test can prove the LAST release closes the
    // endpoint and earlier ones do not.
    int opens() const  { return m_opens; }
    int closes() const { return m_closes; }

protected:
    IqLease openEndpoint(const QString& endpointId, int requestedRateHz) override;
    void    closeEndpoint(const QString& endpointId) override;
    IqLease adjustRate(const QString& endpointId, int requestedRateHz) override;

private:
    const EndpointSpec* find(const QString& id) const;

    QVector<EndpointSpec> m_endpoints;
    QVector<int>   m_supportedRates{48000, 96000, 192000};
    int            m_achievedHz{48000};
    IqRateTopology m_topology{IqRateTopology::PerStream};
    bool           m_radioWide{false};
    bool           m_connected{true};
    int            m_capacity{4};
    int            m_opens{0};
    int            m_closes{0};
};

}  // namespace AetherSDR
