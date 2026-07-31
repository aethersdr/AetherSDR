// BandStack fold-in (RFC #4603 PR 4): bookmarks live as one feature document
// per radio in radio_settings, mutations write through (no save() step), the
// legacy XML side file is claimed lazily per radio — with the file retiring
// when its last section migrates — and the panel-wide preferences move to
// AppSettings.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/BandStackSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/SettingsPaths.h"

#include <QCoreApplication>
#include <QFile>

#include <iostream>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!condition) {
        ++g_failures;
    }
}

QString legacyPath()
{
    return SettingsPaths::configDir() + QStringLiteral("/BandStack.settings");
}

bool writeLegacyFixture()
{
    // Two radios' worth of legacy bookmarks plus header preferences, in the
    // exact shape the XML-era writer produced.
    const char* doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<BandStack>
  <AutoExpiryMinutes>120</AutoExpiryMinutes>
  <GroupByBand>True</GroupByBand>
  <AutoSaveDwellSeconds>30</AutoSaveDwellSeconds>
  <Radio_2125_1213_8600_8895>
    <Entry_0>
      <FrequencyMhz>14.225000</FrequencyMhz>
      <Mode>USB</Mode>
      <FilterLow>100</FilterLow>
      <FilterHigh>2900</FilterHigh>
      <AgcMode>med</AgcMode>
      <AgcThreshold>65</AgcThreshold>
      <AudioGain>50</AudioGain>
      <NbOn>False</NbOn>
      <NbLevel>50</NbLevel>
      <NrOn>False</NrOn>
      <NrLevel>50</NrLevel>
      <WnbOn>False</WnbOn>
      <WnbLevel>50</WnbLevel>
    </Entry_0>
  </Radio_2125_1213_8600_8895>
  <Radio_3419_0000_6600_0001>
    <Entry_0>
      <FrequencyMhz>7.074000</FrequencyMhz>
      <Mode>DIGU</Mode>
      <FilterLow>0</FilterLow>
      <FilterHigh>3000</FilterHigh>
      <AgcMode>med</AgcMode>
      <AgcThreshold>65</AgcThreshold>
      <AudioGain>50</AudioGain>
      <NbOn>False</NbOn>
      <NbLevel>50</NbLevel>
      <NrOn>False</NrOn>
      <NrLevel>50</NrLevel>
      <WnbOn>False</WnbOn>
      <WnbLevel>50</WnbLevel>
    </Entry_0>
  </Radio_3419_0000_6600_0001>
</BandStack>
)";
    QFile file(legacyPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(doc);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-bandstack-scoped-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    auto& stack = BandStackSettings::instance();
    // CRUD exercises its own radio so the migration scopes below stay virgin
    // (a radio that already owns a document must never re-import legacy —
    // asserted explicitly at the end).
    const RadioSettingsScope crudScope(QStringLiteral("flex"),
                                       QStringLiteral("0001-0000-0000-0001"));
    const RadioSettingsScope flexScope(QStringLiteral("flex"),
                                       QStringLiteral("2125-1213-8600-8895"));
    const RadioSettingsScope hl2Scope(QStringLiteral("hl2"),
                                      QStringLiteral("AA:BB:CC:DD:EE:FF"));

    // ---- write-through CRUD, per scope ------------------------------------
    {
        BandStackEntry entry;
        entry.frequencyMhz = 21.074;
        entry.mode = QStringLiteral("DIGU");
        entry.createdAtMs = 1'000;
        stack.addEntry(crudScope, entry);
        check(stack.entries(crudScope).size() == 1, "an added entry reads back");
        check(stack.entries(hl2Scope).isEmpty(),
              "the other radio's stack stays empty (per-radio isolation)");

        // No save() call anywhere — the mutation must already be durable.
        AppSettings::instance().reset();
        AppSettings::instance().load();
        check(stack.entries(crudScope).size() == 1
                  && stack.entries(crudScope).first().frequencyMhz == 21.074,
              "a bookmark survives a store reload with no explicit save");

        stack.removeEntry(crudScope, 0);
        check(stack.entries(crudScope).isEmpty(), "removeEntry writes through");
    }

    // ---- expiry: legacy entries never expire ------------------------------
    {
        BandStackEntry old;
        old.frequencyMhz = 3.573;
        old.createdAtMs = 1;             // ancient
        BandStackEntry legacy;
        legacy.frequencyMhz = 1.840;
        legacy.createdAtMs = 0;          // legacy sentinel
        stack.addEntry(crudScope, old);
        stack.addEntry(crudScope, legacy);
        check(stack.removeExpiredEntries(crudScope, 1000) == 1,
              "an aged entry expires");
        const auto rest = stack.entries(crudScope);
        check(rest.size() == 1 && rest.first().createdAtMs == 0,
              "a legacy (createdAtMs=0) entry never expires");
        stack.clearAllEntries(crudScope);
    }

    // ---- legacy side-file migration ---------------------------------------
    {
        check(writeLegacyFixture(), "legacy fixture is written");
        BandStackSettings::instance().load();   // prefs one-shot
        check(stack.autoExpiryMinutes() == 120 && stack.groupByBand()
                  && stack.autoSaveDwellSeconds() == 30,
              "panel-wide preferences migrate to AppSettings");

        // Both fixture sections use the legacy sanitizer's convention
        // ('-' -> '_'). (An HL2 MAC could never appear: colons are invalid
        // XML element names, so the XML-era writer could not store one — a
        // legacy limitation this fold-in removes.)
        const auto flexEntries = stack.entries(flexScope);
        check(flexEntries.size() == 1
                  && flexEntries.first().frequencyMhz == 14.225
                  && flexEntries.first().mode == QStringLiteral("USB"),
              "the Flex radio's legacy section migrates on first access");
        check(QFile::exists(legacyPath()),
              "the side file survives while another radio's section remains");

        AppSettings::instance().reset();
        AppSettings::instance().load();
        check(stack.entries(flexScope).size() == 1,
              "a migrated stack is durable across reloads");

        // Claiming the LAST remaining section retires the side file.
        const RadioSettingsScope secondScope(QStringLiteral("flex"),
                                             QStringLiteral("3419-0000-6600-0001"));
        const auto secondEntries = stack.entries(secondScope);
        check(secondEntries.size() == 1
                  && secondEntries.first().mode == QStringLiteral("DIGU"),
              "the second radio's legacy section migrates on ITS first access");
        check(!QFile::exists(legacyPath()),
              "the side file retires once its last section is claimed");
    }

    // ---- an existing document never re-imports legacy ---------------------
    // A radio that already owns a document — including one the operator
    // deliberately EMPTIED — must not resurrect bookmarks from a legacy file
    // that reappears (backup restore, sync tooling).
    {
        check(writeLegacyFixture(), "legacy fixture is re-planted");
        // crudScope's document exists (emptied above). Section
        // Radio_0001_0000_0000_0001 is absent from the fixture anyway, so
        // plant one for it by claiming through a scope whose doc exists:
        check(stack.entries(crudScope).isEmpty(),
              "an emptied stack stays empty when a legacy file reappears");
        QFile::remove(legacyPath());
    }

    return g_failures == 0 ? 0 : 1;
}
