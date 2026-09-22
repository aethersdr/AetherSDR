#include "core/SplitAudioProfile.h"

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
    out = qBound(0, static_cast<int>(v.toDouble()), 100);
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
    if (!ver.isDouble() || static_cast<int>(ver.toDouble()) != kVersion)
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

void SplitAudioRecorder::arm(int rxPanBefore, bool txMuted, int txGain, int txPan)
{
    *this = SplitAudioRecorder{};
    m_armed       = true;
    m_rxPanBefore = rxPanBefore;
    m_rxPan       = rxPanBefore;
    // Seeds, not learned values: the touched flags stay false, so a split the
    // operator never touches stores nothing and the next one behaves exactly as
    // it does today.
    m_txMuted = txMuted;
    m_txGain  = txGain;
    m_txPan   = txPan;
}

SplitAudioProfile SplitAudioRecorder::merge(const SplitAudioProfile& existing) const
{
    SplitAudioProfile p = existing;
    p.forgetLearnedState();   // keeps the chosen monitor mode
    if (m_txMuteTouched) { p.hasTxMute = true; p.txMuted = m_txMuted; }
    if (m_txGainTouched) { p.hasTxGain = true; p.txGain  = qBound(0, m_txGain, 100); }
    if (m_txPanTouched)  { p.hasTxPan  = true; p.txPan   = qBound(0, m_txPan,  100); }
    if (m_rxPanTouched)  { p.hasRxPan  = true; p.rxPan   = qBound(0, m_rxPan,  100); }
    return p;
}

}  // namespace AetherSDR
