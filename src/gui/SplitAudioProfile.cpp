#include "SplitAudioProfile.h"

#include <QJsonValue>

namespace AetherSDR {

namespace {

// A pan/gain percentage the radio will accept. Out-of-range is clamped rather
// than rejected: the value still carries the operator's intent ("hard right"),
// and a settings file hand-edited to 150 should land the slice at 100, not
// discard the whole arrangement.
bool readPercent(const QJsonObject& o, const char* key, bool& has, int& out)
{
    const auto v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull())
        return true;              // absent is fine — just not learned
    if (!v.isDouble())
        return false;             // present but wrong type — the object is bad
    // Clamp in the double domain BEFORE narrowing: a finite JSON number such as
    // 1e100 is outside int's range, and casting it first is undefined behaviour.
    out = static_cast<int>(qBound(0.0, v.toDouble(), 100.0));
    has = true;
    return true;
}

bool readBool(const QJsonObject& o, const char* key, bool& has, bool& out)
{
    const auto v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull())
        return true;
    if (!v.isBool())
        return false;
    out = v.toBool();
    has = true;
    return true;
}

}  // namespace

SplitAudioProfile SplitAudioProfile::fromJson(const QJsonObject& o)
{
    SplitAudioProfile p;
    if (o.isEmpty())
        return p;

    // Version gate first. A future writer is free to add fields, but a profile
    // stamped with a version this build does not know is not partially mined
    // for the fields whose names happen to match — their meaning is exactly
    // what the version defines.
    const auto ver = o.value(QStringLiteral("v"));
    // Compared as a double, never narrowed: 1.5 is not version 1, and a huge
    // value must not reach an int cast.
    if (!ver.isDouble() || ver.toDouble() != static_cast<double>(kVersion))
        return p;

    // monitor is read before the audio values and kept even if those turn out
    // to be malformed: it is a chosen setting, and discarding a deliberate
    // choice because a learned value rotted would be the wrong trade.
    const auto mon = o.value(QStringLiteral("monitor"));
    if (mon.isString())
        p.monitor = (mon.toString() == QLatin1String("both")) ? Monitor::Both
                                                              : Monitor::Solo;

    SplitAudioProfile learned = p;
    const bool ok = readBool(o, "txMuted", learned.hasTxMute, learned.txMuted)
                 && readPercent(o, "txGain", learned.hasTxGain, learned.txGain)
                 && readPercent(o, "txPan",  learned.hasTxPan,  learned.txPan)
                 && readPercent(o, "rxPan",  learned.hasRxPan,  learned.rxPan);
    if (!ok)
        return p;                 // keeps the parsed monitor, drops the rest

    return learned;
}

QJsonObject SplitAudioProfile::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("v"), kVersion);
    o.insert(QStringLiteral("monitor"),
             monitor == Monitor::Both ? QStringLiteral("both")
                                      : QStringLiteral("solo"));
    // Only what was learned is written. An omitted key round-trips back to
    // has*==false, which is what keeps "never touched" distinguishable from
    // "set to the default value" across a restart.
    if (hasTxMute) o.insert(QStringLiteral("txMuted"), txMuted);
    if (hasTxGain) o.insert(QStringLiteral("txGain"),  txGain);
    if (hasTxPan)  o.insert(QStringLiteral("txPan"),   txPan);
    if (hasRxPan)  o.insert(QStringLiteral("rxPan"),   rxPan);
    return o;
}

void SplitAudioRecorder::arm(int rxPanBefore, bool rxPanMovedByApply)
{
    *this = SplitAudioRecorder{};
    m_armed             = true;
    m_rxPanBefore       = rxPanBefore;
    m_rxPanMovedByApply = rxPanMovedByApply;
}

SplitAudioProfile SplitAudioRecorder::merge(const SplitAudioProfile& existing) const
{
    SplitAudioProfile p = existing;   // carried forward: it was replayed
    if (m_txMuteTouched) { p.hasTxMute = true; p.txMuted = m_txMuted; }
    if (m_txGainTouched) { p.hasTxGain = true; p.txGain  = qBound(0, m_txGain, 100); }
    if (m_txPanTouched)  { p.hasTxPan  = true; p.txPan   = qBound(0, m_txPan,  100); }
    if (m_rxPanTouched)  { p.hasRxPan  = true; p.rxPan   = qBound(0, m_rxPan,  100); }
    // A split that ends with the TX slice muted is the operator saying they
    // want no arrangement. Keeping its pan and gain would replay them onto a
    // slice nobody can hear, and would still move the RX pan every split.
    if (p.hasTxMute && p.txMuted)
        p.forgetLearnedState();       // keeps the chosen monitor mode
    return p;
}

}  // namespace AetherSDR
