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
// attachAllSeamSignals()'s table is GENERATED (SeamSignalProbeTable.inc, by
// tools/gen_seam_probe_table.py) and guarded twice: `--check` in
// static-checks.yml on every pull request, in seconds and without a build,
// and backend_seam_affinity_test against the meta-object at runtime. The
// static gate is the one that matters — the runtime one was already here when
// #5825 drifted the table, and it only spoke after the merge.

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

// Every signal IRadioBackend declares. The table itself is GENERATED from
// src/core/backends/IRadioBackend.h by tools/gen_seam_probe_table.py, and
// .github/workflows/static-checks.yml runs that tool's --check on every pull
// request. Before #5825 this list was hand-maintained: that PR added
// autoRfGainArmSettled to the header, the probe line was never written, and
// backend_seam_affinity_test passed in the PR and failed on main with
// "51 probed, 52 declared". Add the signal to the header and regenerate.
inline void attachAllSeamSignals(SeamThreadAffinityProbe& p)
{
#include "SeamSignalProbeTable.inc"
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
