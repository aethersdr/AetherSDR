#pragma once

#include <QList>
#include <QString>

namespace AetherSDR {

// One radio the operator has connected to at least once, remembered so its tab
// survives a restart and the radio being off the network.
//
// Discovery alone cannot carry the tab strip: a radio that is powered down, on
// a routed/VPN address, or simply slow to answer a broadcast has no discovery
// record, so a discovery-only strip loses the tab exactly when the operator
// wants to click it to reconnect.  This store is the durable half; discovery
// supplies live status on top of it.
//
// `id` is the stable identity and the settings key:
//   flex / icom  -> the serial, which discovery and the radio both report
//   hl2          -> "hl2:<address>", because HPSDR discovery reports a MAC-ish
//                   serial that is not stable across gateware revisions
// SavedRadio::makeId() is the single place that decision lives, so every
// call site keys the same way.
struct SavedRadio {
    QString  id;
    QString  family{QStringLiteral("flex")};  // "flex" | "icom" | "hl2"
    QString  name;        // as the radio reports itself
    QString  customName;  // operator's rename; empty = use `name`
    QString  address;     // last known host/IP, for reconnecting off-discovery
    quint16  port{0};
    QString  serial;

    // What the tab shows on its top line.
    QString displayName() const { return customName.isEmpty() ? name : customName; }

    static QString makeId(const QString& family, const QString& serial,
                          const QString& address);
};

// Persisted in AppSettings under "RadioTabs" as a JSON array.  A JSON blob
// rather than N keys so the whole set is written atomically — a half-applied
// reorder or rename would otherwise be a reachable state.
class RadioTabStore {
public:
    static QList<SavedRadio> load();
    static void save(const QList<SavedRadio>& radios);

    // Insert, or update in place if the id is already known.  Preserves an
    // existing customName: re-connecting to a radio must not silently undo the
    // operator's rename.
    static void upsert(const SavedRadio& radio);

    static void remove(const QString& id);
    static void rename(const QString& id, const QString& customName);
    static bool contains(const QString& id);
};

} // namespace AetherSDR
