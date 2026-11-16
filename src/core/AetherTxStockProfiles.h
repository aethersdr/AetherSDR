#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace AetherSDR {

// The stock AetherTX presets: read-only, compiled in, listed ahead of the
// operator's own profiles. Each is a full TX profile (the captureTxJson()
// shape) plus the radio TX filter it was tuned for; applying one also sets
// that filter, an operator's profile never does. Names compare
// case-insensitively and are reserved: nothing in the operator's library
// may be saved under one.
class AetherTxStockProfiles {
public:
    struct TxFilter {
        int lowHz;
        int highHz;
    };

    static QStringList names();                         // display order
    static bool        contains(const QString& name);
    static QString     canonicalName(const QString& name); // empty if not stock
    static QJsonObject profile(const QString& name);    // empty if not stock
    static std::optional<TxFilter> txFilter(const QString& name);
    static QString     summary(const QString& name);    // one line, for a tooltip
};

} // namespace AetherSDR
