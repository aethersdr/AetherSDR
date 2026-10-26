#include "RadioTabRecents.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace AetherSDR::radiotabs {

QList<RecentRadio> recentRadiosFromJson(const QJsonArray& array)
{
    QList<RecentRadio> radios;
    QSet<QString> seen;
    for (const QJsonValue& value : array) {
        if (radios.size() >= kMaxRecentRadios) {
            break;
        }
        const QJsonObject object = value.toObject();
        RecentRadio radio;
        radio.id = object.value(QStringLiteral("id")).toString();
        radio.name = object.value(QStringLiteral("name")).toString();
        radio.model = object.value(QStringLiteral("model")).toString();
        radio.address = object.value(QStringLiteral("address")).toString();
        if (radio.id.isEmpty() || seen.contains(radio.id)) {
            continue;
        }
        seen.insert(radio.id);
        radios.append(radio);
    }
    return radios;
}

QJsonArray recentRadiosToJson(const QList<RecentRadio>& radios)
{
    QJsonArray array;
    for (const RecentRadio& radio : radios) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), radio.id);
        object.insert(QStringLiteral("name"), radio.name);
        object.insert(QStringLiteral("model"), radio.model);
        if (!radio.address.isEmpty()) {
            object.insert(QStringLiteral("address"), radio.address);
        }
        array.append(object);
    }
    return array;
}

QList<RecentRadio> withRecent(QList<RecentRadio> radios, const RecentRadio& radio)
{
    radios = withoutRecent(std::move(radios), radio.id);
    radios.prepend(radio);
    while (radios.size() > kMaxRecentRadios) {
        radios.removeLast();
    }
    return radios;
}

QList<RecentRadio> withoutRecent(QList<RecentRadio> radios, const QString& id)
{
    radios.removeIf([&id](const RecentRadio& radio) { return radio.id == id; });
    return radios;
}

QString activeTabId(const QList<RadioTabEntry>& known, const QList<RecentRadio>& recents,
                    const QString& activeId, const QString& activeAddress)
{
    if (activeId.isEmpty() || activeAddress.isEmpty()) {
        return activeId;
    }
    for (const RadioTabEntry& entry : known) {
        if (entry.id == activeId) {
            return activeId;   // it has a tab of its own
        }
    }
    for (const RadioTabEntry& entry : known) {
        if (entry.transport == activeAddress) {
            return entry.id;
        }
    }
    for (const RecentRadio& recent : recents) {
        if (recent.id == activeId) {
            return activeId;
        }
    }
    for (const RecentRadio& recent : recents) {
        if (recent.address == activeAddress) {
            return recent.id;
        }
    }
    return activeId;
}

QList<RadioTabEntry> withSavedRadios(QList<RadioTabEntry> tabs,
                                     const QList<RadioTabEntry>& saved,
                                     const QList<RecentRadio>& recents,
                                     QSet<QString> seenAddresses)
{
    QSet<QString> seen;
    for (const RadioTabEntry& entry : std::as_const(tabs)) {
        seen.insert(entry.id);
    }
    for (const RecentRadio& recent : recents) {
        if (!recent.address.isEmpty() && !seen.contains(recent.id)) {
            seenAddresses.insert(recent.address);
        }
    }
    for (const RadioTabEntry& entry : saved) {
        if (seen.contains(entry.id) || seenAddresses.contains(entry.transport)) {
            continue;
        }
        seen.insert(entry.id);
        tabs.append(entry);
    }
    return tabs;
}

QList<RecentRadio> withoutVanished(QList<RecentRadio> recents,
                                   const QList<RadioTabEntry>& known,
                                   const QStringList& ephemeralIds)
{
    recents.removeIf([&](const RecentRadio& radio) {
        return ephemeralIds.contains(radio.id)
            && std::none_of(known.cbegin(), known.cend(),
                            [&radio](const RadioTabEntry& e) { return e.id == radio.id; });
    });
    return recents;
}

QList<RadioTabEntry> arrangeTabs(const QList<RadioTabEntry>& known,
                                 const QList<RecentRadio>& recents,
                                 const QString& activeId)
{
    QList<RadioTabEntry> arranged;
    QSet<QString> placed;
    for (const RecentRadio& recent : recents) {
        if (placed.contains(recent.id)) {
            continue;
        }
        placed.insert(recent.id);
        auto it = std::find_if(known.cbegin(), known.cend(),
                               [&recent](const RadioTabEntry& e) { return e.id == recent.id; });
        RadioTabEntry entry;
        if (it != known.cend()) {
            entry = *it;
        } else {
            // Nothing describes it right now, so whether it is on is unknown.
            entry.id = recent.id;
            entry.name = recent.name.isEmpty() ? recent.id : recent.name;
            entry.model = recent.model;
            entry.detail = recent.address;
            entry.transport = recent.address;
            entry.status = RadioTabStatus::Saved;
        }
        entry.visibleInTabs = true;
        arranged.append(entry);
    }
    for (RadioTabEntry entry : known) {
        if (placed.contains(entry.id)) {
            continue;
        }
        placed.insert(entry.id);
        entry.visibleInTabs = !activeId.isEmpty() && entry.id == activeId;
        arranged.append(entry);
    }
    return arranged;
}

} // namespace AetherSDR::radiotabs
