#pragma once

#include <optional>

#include <QLatin1String>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>   // qBound

// aetherd RFC 2.3 / #4070 — one home for the present-only + fail-closed decode
// contract shared by the Flex status decoders (pan/slice/meter/transmit). Every
// carrier reads a key out of a Flex status kv-set and applies it ONLY when the
// wire reported it. Numeric parses are ok-guarded (a malformed present value is
// dropped, not applied as 0/0.0) — for an std::optional target that leaves the
// field disengaged (the model keeps its previous value); for a plain field or a
// map entry the value is simply not written. `carry()` is overloaded on the
// target type so every call site reads uniformly, and the guarded parse lives in
// exactly one place (parseInt/parseReal), so a future tweak (base-0 parse,
// whitespace trim) can't be applied to one copy and skipped in another.
namespace AetherSDR::flexkv {

using Kv = QMap<QString, QString>;

// The single-sourced guarded parses everything else builds on.
inline std::optional<int> parseInt(const Kv& kvs, const char* wire, int base = 10)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok, base);
        if (ok) return v;
    }
    return std::nullopt;
}
inline std::optional<double> parseReal(const Kv& kvs, const char* wire)
{
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const double v = kvs.value(QLatin1String(wire)).toDouble(&ok);
        if (ok) return v;
    }
    return std::nullopt;
}

// carry() — present-only apply, overloaded on the target type.
inline void carry(const Kv& kvs, const char* wire, std::optional<QString>& f)
{
    if (kvs.contains(QLatin1String(wire))) f = kvs.value(QLatin1String(wire));
}
inline void carry(const Kv& kvs, const char* wire, std::optional<bool>& f)
{
    if (kvs.contains(QLatin1String(wire)))
        f = kvs.value(QLatin1String(wire)) == QLatin1String("1");
}
inline void carry(const Kv& kvs, const char* wire, std::optional<int>& f)
{
    if (auto v = parseInt(kvs, wire)) f = v;
}
inline void carry(const Kv& kvs, const char* wire, std::optional<double>& f)
{
    if (auto v = parseReal(kvs, wire)) f = v;
}
// Plain-field targets (e.g. MeterDef): keep the field's existing/default value
// when the key is absent or malformed.
inline void carry(const Kv& kvs, const char* wire, QString& f)
{
    if (kvs.contains(QLatin1String(wire))) f = kvs.value(QLatin1String(wire));
}
inline void carry(const Kv& kvs, const char* wire, int& f, int base = 10)
{
    if (auto v = parseInt(kvs, wire, base)) f = *v;
}
inline void carry(const Kv& kvs, const char* wire, double& f)
{
    if (auto v = parseReal(kvs, wire)) f = *v;
}
// Map target (namespaced extension bundles, e.g. pan "panState"): raw string,
// keyed by the wire key, for the model to parse.
inline void carry(const Kv& kvs, const char* wire, QVariantMap& m)
{
    if (kvs.contains(QLatin1String(wire)))
        m.insert(QLatin1String(wire), kvs.value(QLatin1String(wire)));
}

// Clamped int (present-only, ok-guarded) — the transmit ranges.
inline void carryClamp(const Kv& kvs, const char* wire, std::optional<int>& f,
                       int lo, int hi)
{
    if (auto v = parseInt(kvs, wire)) f = qBound(lo, *v, hi);
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
