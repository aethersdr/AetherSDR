#pragma once

#include "RadioTabBar.h"

#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace AetherSDR::radiotabs {

// The title bar holds the radios the operator connected to most recently;
// every other known radio lives in the drawer (the radio list).
constexpr int kMaxRecentRadios = 3;

// What a recent radio's tab needs when no discovery, SmartLink or saved
// profile describes it (an HL2 switched off, a SmartLink radio while logged
// out), so the tab can still name it.
struct RecentRadio {
    QString id;
    QString name;
    QString model;
    QString address;   // the IP it was reached at, when known

    bool operator==(const RecentRadio& o) const
    {
        return id == o.id && name == o.name && model == o.model && address == o.address;
    }
};

QList<RecentRadio> recentRadiosFromJson(const QJsonArray& array);
QJsonArray recentRadiosToJson(const QList<RecentRadio>& radios);

// `radio` first, its older entry dropped, at most kMaxRecentRadios kept.
QList<RecentRadio> withRecent(QList<RecentRadio> radios, const RecentRadio& radio);
QList<RecentRadio> withoutRecent(QList<RecentRadio> radios, const QString& id);

// The tab id of the connected radio. A radio reached by IP reports that
// address as its id; when discovery lists a radio at the same address (a Flex
// connected by IP on its own LAN), or a recent radio was reached there (the
// same Flex later reached over a VPN), that is the same radio, so it keeps
// that tab rather than gaining a second one.
QString activeTabId(const QList<RadioTabEntry>& known, const QList<RecentRadio>& recents,
                    const QString& activeId, const QString& activeAddress);

// `tabs` plus each `saved` manual connection not already among them by id, or
// by an address in `seenAddresses` or a recent radio's address. One address is
// one entry: a recent radio's tab opens the profile saved there.
QList<RadioTabEntry> withSavedRadios(QList<RadioTabEntry> tabs,
                                     const QList<RadioTabEntry>& saved,
                                     const QList<RecentRadio>& recents,
                                     QSet<QString> seenAddresses);

// `recents` without any `ephemeralIds` radio (the demo simulator) that no
// source lists now: it exists only while shown, so it is never a Saved tab.
QList<RecentRadio> withoutVanished(QList<RecentRadio> recents,
                                   const QList<RadioTabEntry>& known,
                                   const QStringList& ephemeralIds);

// The recent radios first, most recent first, then every other radio for the
// drawer. A recent radio no source knows about gets a Saved tab from its
// stored name. Only the recent radios, and the connected one, show as tabs.
QList<RadioTabEntry> arrangeTabs(const QList<RadioTabEntry>& known,
                                 const QList<RecentRadio>& recents,
                                 const QString& activeId);

} // namespace AetherSDR::radiotabs
