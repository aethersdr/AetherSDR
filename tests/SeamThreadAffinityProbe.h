#pragma once

// Tripwire for IRadioBackend contract rules 2 and 6 ("THREADING AND LIFETIME
// CONTRACT" in IRadioBackend.h): every seam signal is emitted on the thread
// the backend object lives on, and nothing is emitted after disconnected().
//
// Connects to EVERY signal IRadioBackend declares with Qt::DirectConnection,
// so the recording lambda runs on whichever thread actually emits and can
// compare it against backend->thread(). Attach it to any backend a test
// already drives (a fake radio on localhost, injected frames, the simulator)
// and assert violations().isEmpty() at the end — no other change to the test.
//
// attachAllSeamSignals() is checked against the meta-object by
// backend_seam_affinity_test: a signal added to IRadioBackend without a line
// here fails that test, so the tripwire cannot silently stop covering one.

#include "core/backends/IRadioBackend.h"

#include <QHash>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>

namespace AetherSDR::test {

inline QString seamThreadName(QThread* t)
{
    if (!t) return QStringLiteral("<null>");
    const QString n = t->objectName();
    return n.isEmpty()
        ? QStringLiteral("QThread@%1").arg(reinterpret_cast<quintptr>(t), 0, 16)
        : n;
}

class SeamThreadAffinityProbe {
public:
    explicit SeamThreadAffinityProbe(IRadioBackend* b) : m_backend(b), m_home(b->thread()) {}

    // Safe from any thread: a violation IS a call from another thread, and the
    // recorder has to survive it to report it.
    void record(const char* signal)
    {
        QThread* current = QThread::currentThread();
        QMutexLocker lock(&m_mutex);
        const QString name = QString::fromLatin1(signal);
        ++m_counts[name];
        if (current != m_home) {
            m_violations << QStringLiteral("%1 emitted on %2, backend lives on %3")
                                .arg(name, seamThreadName(current), seamThreadName(m_home));
        }
        if (m_sawDisconnected && name != QLatin1String("disconnected")) {
            m_afterDisconnect << name;
        }
        if (name == QLatin1String("disconnected")) {
            m_sawDisconnected = true;
        }
    }
    // A reconnect legitimately follows a disconnect; call this when the test
    // reconnects so rule 6 is judged per session.
    void resetDisconnectGate()
    {
        QMutexLocker lock(&m_mutex);
        m_sawDisconnected = false;
    }

    QStringList violations() const { QMutexLocker l(&m_mutex); return m_violations; }
    QStringList afterDisconnect() const { QMutexLocker l(&m_mutex); return m_afterDisconnect; }
    QStringList observed() const
    {
        QMutexLocker l(&m_mutex);
        QStringList out;
        for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it) {
            out << QStringLiteral("%1×%2").arg(it.key()).arg(it.value());
        }
        out.sort();
        return out;
    }
    int count(const QString& name) const { QMutexLocker l(&m_mutex); return m_counts.value(name); }
    int probed() const { return m_probedNames.size(); }
    QSet<QString> probedNames() const { return m_probedNames; }
    QObject* context() { return &m_context; }
    IRadioBackend* backend() const { return m_backend; }
    void noteProbed(const char* name) { m_probedNames.insert(QString::fromLatin1(name)); }

private:
    IRadioBackend* m_backend;
    QThread* m_home;
    QObject m_context;
    mutable QMutex m_mutex;
    QHash<QString, int> m_counts;
    QStringList m_violations;
    QStringList m_afterDisconnect;
    bool m_sawDisconnected{false};
    QSet<QString> m_probedNames;
};

template <typename Signal>
inline void attachSeamSignal(SeamThreadAffinityProbe& p, Signal signal, const char* name)
{
    QObject::connect(p.backend(), signal, p.context(),
                     [&p, name](auto&&...) { p.record(name); },
                     Qt::DirectConnection);
    p.noteProbed(name);
}

#define AETHER_SEAM_PROBE(sig) attachSeamSignal(p, &IRadioBackend::sig, #sig)

// Every signal IRadioBackend declares.
inline void attachAllSeamSignals(SeamThreadAffinityProbe& p)
{
    AETHER_SEAM_PROBE(connected);
    AETHER_SEAM_PROBE(disconnected);
    AETHER_SEAM_PROBE(connectionError);
    AETHER_SEAM_PROBE(configurationWarning);
    AETHER_SEAM_PROBE(capabilitiesChanged);
    AETHER_SEAM_PROBE(transmitFrequencyCheckChanged);
    AETHER_SEAM_PROBE(radioDialLockChanged);
    AETHER_SEAM_PROBE(linkStatsUpdated);
    AETHER_SEAM_PROBE(extensionResult);
    AETHER_SEAM_PROBE(extensionError);
    AETHER_SEAM_PROBE(sliceChanged);
    AETHER_SEAM_PROBE(sliceRemoved);
    AETHER_SEAM_PROBE(sliceLifecycleFailed);
    AETHER_SEAM_PROBE(meterUpdate);
    AETHER_SEAM_PROBE(transmitChanged);
    AETHER_SEAM_PROBE(keyingStateConfirmed);
    AETHER_SEAM_PROBE(amplifierChanged);
    AETHER_SEAM_PROBE(tunerChanged);
    AETHER_SEAM_PROBE(radioChanged);
    AETHER_SEAM_PROBE(gpsChanged);
    AETHER_SEAM_PROBE(memoryChanged);
    AETHER_SEAM_PROBE(memoryRefreshStarted);
    AETHER_SEAM_PROBE(memoryRefreshProgress);
    AETHER_SEAM_PROBE(memoryRefreshFinished);
    AETHER_SEAM_PROBE(profileChanged);
    AETHER_SEAM_PROBE(meterDefined);
    AETHER_SEAM_PROBE(meterRemoved);
    AETHER_SEAM_PROBE(panCenterBandwidthChanged);
    AETHER_SEAM_PROBE(panRemoved);
    AETHER_SEAM_PROBE(notchChanged);
    AETHER_SEAM_PROBE(notchRemoved);
    AETHER_SEAM_PROBE(sliceAudioFrameReady);
    AETHER_SEAM_PROBE(panWideChanged);
    AETHER_SEAM_PROBE(panRangeChanged);
    AETHER_SEAM_PROBE(panBandwidthLimitsChanged);
    AETHER_SEAM_PROBE(panRfGainChanged);
    AETHER_SEAM_PROBE(operatingStateChanged);
    AETHER_SEAM_PROBE(panRfGainInfoChanged);
    AETHER_SEAM_PROBE(panPreampInfoChanged);
    AETHER_SEAM_PROBE(panPreampChanged);
    AETHER_SEAM_PROBE(panAttenuatorInfoChanged);
    AETHER_SEAM_PROBE(panAttenuatorChanged);
    AETHER_SEAM_PROBE(panRxAntennaChanged);
    AETHER_SEAM_PROBE(panAntennaListChanged);
    AETHER_SEAM_PROBE(panWaterfallLineDurationChanged);
    AETHER_SEAM_PROBE(extensionStatus);
    AETHER_SEAM_PROBE(spectrumFrameReady);
    AETHER_SEAM_PROBE(waterfallRowReady);
    AETHER_SEAM_PROBE(audioFrameReady);
}

#undef AETHER_SEAM_PROBE

// Every signal IRadioBackend declares, by name, from the meta-object.
inline QStringList declaredSeamSignals()
{
    const QMetaObject& mo = IRadioBackend::staticMetaObject;
    QStringList names;
    for (int i = mo.methodOffset(); i < mo.methodCount(); ++i) {
        const QMetaMethod m = mo.method(i);
        if (m.methodType() == QMetaMethod::Signal) names << QString::fromLatin1(m.name());
    }
    names.removeDuplicates();   // a default argument declares two overloads
    names.sort();
    return names;
}

} // namespace AetherSDR::test
