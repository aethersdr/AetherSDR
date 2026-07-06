#pragma once

#include <optional>

#include <QLatin1String>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>   // qBound

// aetherd RFC 2.3 / #4070 — one home for the present-only + fail-closed decode
// contract shared by the Flex status decoders (pan/slice/meter/transmit). Each
// carrier reads a key out of a Flex status kv-set and applies it ONLY when the
// wire reported it; the numeric carriers are ok-guarded (a malformed present
// value is dropped, not applied as 0/0.0). Previously this pattern was hand-
// re-rolled in each decoder (SliceDelta's oStr/oInt/…, TransmitDelta's tInt/…,
// the meter inline guards) — single-sourcing it means a future tweak (base-0
// parse, whitespace trim) can't be applied to one copy and silently skipped in
// the others.
namespace AetherSDR::flexkv {

using Kv = QMap<QString, QString>;

// ---- carriers into std::optional<T> delta fields (slice / transmit) ----
inline void carryStr(const Kv& kvs, const char* wire, std::optional<QString>& f)
{
    if (kvs.contains(QLatin1String(wire))) f = kvs.value(QLatin1String(wire));
}
inline void carryBool(const Kv& kvs, const char* wire, std::optional<bool>& f)
{
    if (kvs.contains(QLatin1String(wire)))
        f = kvs.value(QLatin1String(wire)) == QLatin1String("1");
}
inline void carryInt(const Kv& kvs, const char* wire, std::optional<int>& f)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok);
        if (ok) f = v;
    }
}
inline void carryClamp(const Kv& kvs, const char* wire, std::optional<int>& f,
                       int lo, int hi)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok);
        if (ok) f = qBound(lo, v, hi);
    }
}
inline void carryReal(const Kv& kvs, const char* wire, std::optional<double>& f)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const double v = kvs.value(QLatin1String(wire)).toDouble(&ok);
        if (ok) f = v;
    }
}

// ---- carriers into plain struct fields (meter's MeterDef) — same present-only
//      + ok-guard contract, but a plain field keeps its existing/default value
//      when the key is absent or malformed. ----
inline void carryInto(const Kv& kvs, const char* wire, QString& f)
{
    if (kvs.contains(QLatin1String(wire))) f = kvs.value(QLatin1String(wire));
}
inline void carryIntInto(const Kv& kvs, const char* wire, int& f, int base = 10)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok, base);
        if (ok) f = v;
    }
}
inline void carryRealInto(const Kv& kvs, const char* wire, double& f)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const double v = kvs.value(QLatin1String(wire)).toDouble(&ok);
        if (ok) f = v;
    }
}

// Comma-split with per-token trim; empty tokens dropped.
inline QStringList splitList(const QString& raw)
{
    QStringList out;
    for (QString t : raw.split(',', Qt::SkipEmptyParts)) {
        t = t.trimmed();
        if (!t.isEmpty()) out.append(t);
    }
    return out;
}

}  // namespace AetherSDR::flexkv
