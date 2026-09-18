#pragma once

#include "core/tnc/Ax25.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

namespace AetherSDR {

// WIDE1-1 fill-in digipeater (New n-N paradigm). Answers the first unused
// hop when it is WIDE1-1 (optionally RELAY or our own call), substitutes
// MYCALL with the H-bit set, and leaves the rest of the path alone. Does
// not decrement WIDEn-N — that is a wide-area digi's job.
class AprsFillInDigipeater : public QObject {
    Q_OBJECT

public:
    enum class Drop {
        Repeated,     // we encoded a fill-in copy
        NotUi,
        Own,          // source is MYCALL
        Duplicate,    // same source+destination+info inside the dupe window
        AlreadyHeard, // our call already has the H-bit in the via list
        NoUnusedHop,
        NoAliasMatch,
    };

    struct Decision {
        Drop drop{Drop::NoAliasMatch};
        std::optional<ax25::Frame> outgoing; // set when drop == Repeated
        QString reason;
    };

    struct Stats {
        quint64 heard{0};
        quint64 repeated{0};
        quint64 droppedDupe{0};
        quint64 droppedNoMatch{0};
        quint64 droppedOwn{0};
        quint64 droppedOther{0};
    };

    explicit AprsFillInDigipeater(QObject* parent = nullptr);

    void setMyAddress(const ax25::Address& addr) { m_my = addr; }
    ax25::Address myAddress() const { return m_my; }

    void setAlias(const ax25::Address& alias) { m_alias = alias; }
    ax25::Address alias() const { return m_alias; }

    void setAlsoMyCall(bool on) { m_alsoMyCall = on; }
    bool alsoMyCall() const { return m_alsoMyCall; }

    void setAlsoRelay(bool on) { m_alsoRelay = on; }
    bool alsoRelay() const { return m_alsoRelay; }

    void setDupeWindowSecs(int secs);
    int dupeWindowSecs() const { return m_dupeSecs; }

    // Does not TX. Always updates dupe memory; recordStats controls counters.
    Decision consider(const ax25::Frame& frame, bool recordStats = true);

    Stats stats() const { return m_stats; }

    static QString tnc2(const ax25::Frame& frame);

private:
    bool hopMatches(const ax25::Address& hop) const;
    void remember(const QString& key, qint64 nowMs);
    bool isDuplicate(const QString& key, qint64 nowMs);
    void pruneDupes(qint64 nowMs);

    ax25::Address m_my;
    ax25::Address m_alias; // default WIDE1-1, set in ctor
    bool m_alsoMyCall{true};
    bool m_alsoRelay{false};
    int m_dupeSecs{30};
    Stats m_stats;
    QHash<QString, qint64> m_dupes; // key → last-seen ms
};

} // namespace AetherSDR
