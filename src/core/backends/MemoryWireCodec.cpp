#include "core/backends/MemoryWireCodec.h"

// The present-only + ok-guarded carry() family. It lives under flex/ because
// the Flex status decoders were its first callers, but the contract it encodes
// is the wire-decode contract, not a Flex one — the memory kv-set has the same
// shape whoever produced it.
#include "core/backends/flex/FlexKvCarry.h"

namespace AetherSDR::MemoryWire {

using namespace flexkv;

MemoryDelta decodeStatus(int index, const QMap<QString, QString>& kvs)
{
    MemoryDelta d;
    d.index = index;
    if (kvs.value(QStringLiteral("in_use")) == QLatin1String("0")
        || kvs.contains(QStringLiteral("removed"))) {
        d.removed = true;
        return d;
    }

    carry(kvs, "group", d.group);
    carry(kvs, "owner", d.owner);
    carry(kvs, "name", d.name);
    carry(kvs, "mode", d.mode);
    carry(kvs, "repeater", d.offsetDir);
    carry(kvs, "tone_mode", d.toneMode);

    carry(kvs, "freq", d.freq);
    carry(kvs, "repeater_offset", d.repeaterOffset);
    carry(kvs, "tone_value", d.toneValue);
    carry(kvs, "step", d.step);
    carry(kvs, "squelch", d.squelch);
    carry(kvs, "squelch_level", d.squelchLevel);
    carry(kvs, "rx_filter_low", d.rxFilterLow);
    carry(kvs, "rx_filter_high", d.rxFilterHigh);
    carry(kvs, "rtty_mark", d.rttyMark);
    carry(kvs, "rtty_shift", d.rttyShift);
    carry(kvs, "digl_offset", d.diglOffset);
    carry(kvs, "digu_offset", d.diguOffset);
    return d;
}

QMap<QString, QString> parseKvTail(const QString& tail)
{
    QMap<QString, QString> kvs;
    for (const QString& token : tail.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = token.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        kvs.insert(token.left(eq), token.mid(eq + 1));
    }
    return kvs;
}

}  // namespace AetherSDR::MemoryWire
