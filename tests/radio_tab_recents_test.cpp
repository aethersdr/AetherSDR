// The title bar's tabs are the radios connected to most recently; every other
// radio waits in the drawer. Pure list logic: no widgets, sockets or settings.
#include "gui/RadioTabRecents.h"

#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::radiotabs;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}

RecentRadio recent(const char* id, const char* name = "")
{
    return {QString::fromLatin1(id), QString::fromLatin1(name), QString()};
}

QStringList ids(const QList<RecentRadio>& radios)
{
    QStringList out;
    for (const auto& r : radios) out << r.id;
    return out;
}

RadioTabEntry known(const char* id, RadioTabStatus status = RadioTabStatus::Available)
{
    RadioTabEntry e;
    e.id = QString::fromLatin1(id);
    e.name = e.id;
    e.status = status;
    return e;
}

const RadioTabEntry* entry(const QList<RadioTabEntry>& tabs, const char* id)
{
    for (const auto& t : tabs) {
        if (t.id == QLatin1String(id)) return &t;
    }
    return nullptr;
}
}

int main()
{
    // ── The recent list ─────────────────────────────────────────────────────
    QList<RecentRadio> list;
    list = withRecent(list, recent("flex"));
    list = withRecent(list, recent("icom"));
    list = withRecent(list, recent("hl2"));
    check(ids(list) == QStringList({"hl2", "icom", "flex"}), "the newest connection is first");
    check(kMaxRecentRadios == 3, "the title bar holds three recent radios");

    list = withRecent(list, recent("demo"));
    check(ids(list) == QStringList({"demo", "hl2", "icom"}),
          "a fourth radio bumps the oldest into the drawer");

    list = withRecent(list, recent("icom"));
    check(ids(list) == QStringList({"icom", "demo", "hl2"}),
          "reconnecting moves a radio to the front without duplicating it");

    list = withoutRecent(list, QStringLiteral("demo"));
    check(ids(list) == QStringList({"icom", "hl2"}), "removing a tab drops that radio only");

    // ── Stored form ─────────────────────────────────────────────────────────
    const QList<RecentRadio> named = {recent("icom:10.0.0.5", "IC-9700"), recent("flex", "FLX8600")};
    check(recentRadiosFromJson(recentRadiosToJson(named)) == named,
          "the stored list reads back as written, in order");

    QJsonArray stored = recentRadiosToJson({recent("a"), recent("b"), recent("a"),
                                            recent("c"), recent("d")});
    check(ids(recentRadiosFromJson(stored)) == QStringList({"a", "b", "c"}),
          "a stored list is read newest first, without duplicates, at most three");
    stored.prepend(QJsonObject{{QStringLiteral("name"), QStringLiteral("no id")}});
    check(ids(recentRadiosFromJson(stored)).first() == QLatin1String("a"),
          "an entry without an id is ignored");

    // ── Arranging the strip and the drawer ──────────────────────────────────
    const QList<RadioTabEntry> all = {known("hl2"), known("flex", RadioTabStatus::Connected),
                                      known("demo"), known("smartlink")};
    const QList<RecentRadio> recents = {recent("flex"), recent("icom:10.0.0.5", "IC-9700"),
                                        recent("hl2")};
    const auto tabs = arrangeTabs(all, recents, QStringLiteral("flex"));

    check(tabs.size() == 5, "every known radio, plus each recent one nothing describes, is listed");
    check(tabs.at(0).id == QLatin1String("flex") && tabs.at(1).id == QLatin1String("icom:10.0.0.5")
              && tabs.at(2).id == QLatin1String("hl2"),
          "the recent radios lead, most recent first");
    const auto* icom = entry(tabs, "icom:10.0.0.5");
    check(icom && icom->status == RadioTabStatus::Saved && icom->name == QLatin1String("IC-9700"),
          "a recent radio nothing describes now is a Saved tab under its stored name");
    check(entry(tabs, "flex")->status == RadioTabStatus::Connected,
          "a recent radio a source describes keeps that source's live status");
    check(entry(tabs, "flex")->visibleInTabs && icom->visibleInTabs
              && entry(tabs, "hl2")->visibleInTabs,
          "the recent radios are tabs");
    check(!entry(tabs, "demo")->visibleInTabs && !entry(tabs, "smartlink")->visibleInTabs,
          "every other radio waits in the drawer");

    const auto withoutHistory = arrangeTabs(all, {}, QStringLiteral("flex"));
    check(entry(withoutHistory, "flex")->visibleInTabs,
          "the connected radio is a tab even before it is recorded as recent");
    check(!entry(withoutHistory, "hl2")->visibleInTabs, "with no history nothing else is a tab");

    // ── One radio reached two ways ──────────────────────────────────────────
    RadioTabEntry lanFlex = known("4324-1213-8600-3564");
    lanFlex.transport = QStringLiteral("172.25.190.100");
    RadioTabEntry smartlink = known("smartlink");
    smartlink.transport = QStringLiteral("SmartLink");
    check(activeTabId({lanFlex, smartlink}, {}, QStringLiteral("172.25.190.100"),
                      QStringLiteral("172.25.190.100")) == lanFlex.id,
          "a Flex connected by IP at its discovered address takes its discovered tab");
    check(activeTabId({lanFlex}, {}, QStringLiteral("10.8.0.5"), QStringLiteral("10.8.0.5"))
              == QLatin1String("10.8.0.5"),
          "a routed radio no discovery lists keeps its own id");
    check(activeTabId({lanFlex}, {}, lanFlex.id, QStringLiteral("172.25.190.100")) == lanFlex.id,
          "a discovered radio connected normally keeps its id");
    check(activeTabId({lanFlex}, {}, QString(), QString()).isEmpty(),
          "with nothing connected there is no active tab");

    RecentRadio lanRecent = recent("4324-1213-8600-3564", "HOME");
    lanRecent.address = QStringLiteral("172.25.190.100");
    check(activeTabId({smartlink}, {lanRecent}, QStringLiteral("172.25.190.100"),
                      QStringLiteral("172.25.190.100")) == lanRecent.id,
          "a Flex reached by IP where discovery can't see it takes its recent tab");
    RecentRadio icomRecent = recent("icom:172.25.190.110", "IC-9700");
    icomRecent.address = QStringLiteral("172.25.190.110");
    check(activeTabId({}, {lanRecent, icomRecent}, icomRecent.id, QStringLiteral("172.25.190.110"))
              == icomRecent.id,
          "a recent radio connected again keeps its own id");

    // ── Saved manual connections in the drawer ──────────────────────────────
    RadioTabEntry savedAtRecent = known("172.25.190.100", RadioTabStatus::Saved);
    savedAtRecent.transport = QStringLiteral("172.25.190.100");
    RadioTabEntry savedElsewhere = known("icom:10.0.0.7", RadioTabStatus::Saved);
    savedElsewhere.transport = QStringLiteral("10.0.0.7");
    RadioTabEntry savedAtLan = known("10.0.0.9", RadioTabStatus::Saved);
    savedAtLan.transport = QStringLiteral("10.0.0.9");
    RadioTabEntry savedListed = known("smartlink", RadioTabStatus::Saved);
    const auto drawer = withSavedRadios({smartlink},
                                        {savedAtRecent, savedElsewhere, savedAtLan, savedListed},
                                        {lanRecent}, {QStringLiteral("10.0.0.9")});
    check(!entry(drawer, "172.25.190.100"),
          "a recent radio and the profile saved at its address are one entry");
    check(entry(drawer, "icom:10.0.0.7") != nullptr, "a saved radio at its own address is listed");
    check(!entry(drawer, "10.0.0.9"), "a profile at an address a source lists is that radio");
    check(drawer.size() == 2, "a profile whose radio is already listed is not listed twice");

    // ── The demo exists only while shown ────────────────────────────────────
    const QList<RecentRadio> withDemo = {recent("flex"), recent("DEMO-0001"), recent("icom")};
    const QStringList ephemeral = {QStringLiteral("DEMO-0001")};
    check(ids(withoutVanished(withDemo, {known("flex")}, ephemeral))
              == QStringList({"flex", "icom"}),
          "a hidden demo leaves the recent list instead of becoming a Saved tab");
    check(ids(withoutVanished(withDemo, {known("flex"), known("DEMO-0001")}, ephemeral))
              == QStringList({"flex", "DEMO-0001", "icom"}),
          "a shown demo stays recent");
    check(ids(withoutVanished(withDemo, {}, ephemeral)).contains(QLatin1String("icom")),
          "a real radio nothing lists stays recent (it is Saved, not gone)");

    // ── The address a recent radio was reached at ───────────────────────────
    RecentRadio withAddress = recent("icom:172.25.190.110", "IC-9700");
    withAddress.address = QStringLiteral("172.25.190.110");
    check(recentRadiosFromJson(recentRadiosToJson({withAddress})) == QList<RecentRadio>{withAddress},
          "a recent radio's address is stored and read back");
    const auto offline = arrangeTabs({}, {withAddress}, QString());
    check(offline.size() == 1 && offline.first().transport == QLatin1String("172.25.190.110")
              && offline.first().detail == QLatin1String("172.25.190.110"),
          "a Saved tab nothing describes shows the address it was reached at");

    RecentRadio unnamed = recent("10.8.0.5");
    const auto fallback = arrangeTabs({}, {unnamed}, QString());
    check(fallback.size() == 1 && fallback.first().name == QLatin1String("10.8.0.5"),
          "a recent radio stored without a name falls back to its id");

    return failures ? 1 : 0;
}
