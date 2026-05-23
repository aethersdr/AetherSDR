#include "core/ProfileTransfer.h"

#include <QByteArray>
#include <QString>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;

    MasterSDR::ExportSelection profilesOnly;
    profilesOnly.globalProfiles = true;
    profilesOnly.txProfiles = true;
    profilesOnly.micProfiles = true;
    profilesOnly.globalProfileNames = {QStringLiteral("Morning"), QStringLiteral("Contest")};
    profilesOnly.txProfileNames = {QStringLiteral("Ragchew")};
    profilesOnly.micProfileNames = {QStringLiteral("Studio Mic")};
    QByteArray expectedProfilesOnly =
        "GLOBAL_PROFILES^Morning^Contest^\r\n"
        "HW_PROFILES^Ragchew^\r\n"
        "MIC_PROFILES^Studio Mic^\r\n";
    ok &= expect(MasterSDR::buildMetaSubset(profilesOnly) == expectedProfilesOnly,
                 "profiles-only meta_subset matches SmartSDR caret format");

    MasterSDR::ExportSelection selectAll = profilesOnly;
    selectAll.memories = true;
    selectAll.preferences = true;
    selectAll.tnf = true;
    selectAll.xvtr = true;
    selectAll.usbCables = true;
    selectAll.memoryGroups = {{QStringLiteral("Owner Name"), QStringLiteral("Group A")}};
    QByteArray expectedAll =
        expectedProfilesOnly
        + QByteArray("MEMORIES^Owner") + char(0x7f) + "Name|Group" + char(0x7f) + "A^\r\n"
        + QByteArray("BAND_PERSISTENCE^\r\n"
                     "MODE_PERSISTENCE^\r\n"
                     "GLOBAL_PERSISTENCE^\r\n"
                     "TNFS^\r\n"
                     "XVTRS^\r\n"
                     "USB_CABLES^\r\n");
    ok &= expect(MasterSDR::buildMetaSubset(selectAll) == expectedAll,
                 "select-all meta_subset includes memories, persistence, TNF, XVTR, and USB cables");

    MasterSDR::ExportSelection noPreferences = selectAll;
    noPreferences.preferences = false;
    const QByteArray noPreferencesBytes = MasterSDR::buildMetaSubset(noPreferences);
    ok &= expect(!noPreferencesBytes.contains("BAND_PERSISTENCE"),
                 "select-all-except-preferences omits band persistence");
    ok &= expect(!noPreferencesBytes.contains("MODE_PERSISTENCE"),
                 "select-all-except-preferences omits mode persistence");
    ok &= expect(!noPreferencesBytes.contains("GLOBAL_PERSISTENCE"),
                 "select-all-except-preferences omits global persistence");
    ok &= expect(noPreferencesBytes.contains("TNFS^\r\n")
                 && noPreferencesBytes.contains("XVTRS^\r\n")
                 && noPreferencesBytes.contains("USB_CABLES^\r\n"),
                 "select-all-except-preferences keeps the other selected categories");

    const QVersionNumber parsed =
        MasterSDR::parseSmartSdrVersionFromFilename(
            QStringLiteral("ASDR_Config_2026-05-13_10-30-00_v4.1.5.123.ssdr_cfg"));
    ok &= expect(!parsed.isNull() && parsed.toString() == QStringLiteral("4.1.5.123"),
                 "MasterSDR filename version parser extracts firmware version");
    const QVersionNumber legacyParsed =
        MasterSDR::parseSmartSdrVersionFromFilename(
            QStringLiteral("SSDR_Config_2026-05-13_10-30-00_v3.7.9.ssdr_cfg"));
    ok &= expect(!legacyParsed.isNull() && legacyParsed.toString() == QStringLiteral("3.7.9"),
                 "SmartSDR filename version parser still accepts legacy export names");
    ok &= expect(MasterSDR::parseSmartSdrVersionFromFilename(
                     QStringLiteral("backup.ssdr_cfg")).isNull(),
                 "SmartSDR filename version parser tolerates absent version");
    ok &= expect(MasterSDR::compareFirmwareVersions(QStringLiteral("4.1.5"),
                                                     QStringLiteral("4.0.9")) > 0,
                 "firmware comparison detects newer version");
    ok &= expect(MasterSDR::compareFirmwareVersions(QStringLiteral("3.7.9"),
                                                     QStringLiteral("4.0.0")) < 0,
                 "firmware comparison detects older version");
    ok &= expect(MasterSDR::compareFirmwareVersions(QStringLiteral("4.1.5"),
                                                     QStringLiteral("4.1.5.0")) == 0,
                 "firmware comparison normalizes equivalent versions");

    ok &= expect(MasterSDR::parseTransferPort(QStringLiteral("4995")).value_or(0) == 4995,
                 "port parser accepts bare command reply body");
    ok &= expect(MasterSDR::parseTransferPort(QStringLiteral("port=42607")).value_or(0) == 42607,
                 "port parser accepts key-value command reply body");
    ok &= expect(MasterSDR::parseTransferPort(QStringLiteral("endpoint=192.168.1.10:4995")).value_or(0) == 4995,
                 "port parser prefers endpoint port over IP address octets");
    ok &= expect(!MasterSDR::parseTransferPort(QStringLiteral("no usable port")).has_value(),
                 "port parser rejects replies without a valid TCP port");

    return ok ? 0 : 1;
}
