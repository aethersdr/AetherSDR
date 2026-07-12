// Standalone test harness for FlexWaveformModel protocol parsing.
//
// Build: produced by CMake as `flex_waveform_model_test`.
// Run:   ./build/flex_waveform_model_test
// Exit:  0 = pass, 1 = fail.

#include "models/FlexWaveformModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <cstdlib>

using AetherSDR::FlexWaveformModel;
using AetherSDR::FlexWaveformEntry;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {}) {
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toUtf8().constData());
    if (!ok) ++g_failed;
}

QMap<QString, QString> makeKvs(std::initializer_list<std::pair<QString, QString>> pairs)
{
    QMap<QString, QString> kvs;
    for (const auto& [k, v] : pairs)
        kvs.insert(k, v);
    return kvs;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── installed_list — two entries with DEL separator ────────────────────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::waveformsChanged);

        // Entry format: name<DEL>version  (DEL = 0x7F, ASCII 127)
        const QString list = QString("FREEDV") + QChar(0x7F) + "v1"
                           + ","
                           + QString("RTTY") + QChar(0x7F) + "v2";
        m.handleInstalledList(makeKvs({{"installed_list", list}}));

        report("installedList: waveformsChanged emitted",   spy.size() == 1);
        report("installedList: two entries",                m.waveforms().size() == 2);
        report("installedList: first name = FREEDV",        m.waveforms()[0].name == "FREEDV");
        report("installedList: first version = v1",         m.waveforms()[0].version == "v1");
        report("installedList: first isContainer = false",  !m.waveforms()[0].isContainer);
        report("installedList: second name = RTTY",         m.waveforms()[1].name == "RTTY");
        report("installedList: displayName = 'RTTY v2'",   m.waveforms()[1].displayName() == "RTTY v2");
    }

    // ── installed_list — empty entry is skipped without crash ──────────────
    {
        FlexWaveformModel m;

        const QString list = QString("FREEDV") + QChar(0x7F) + "v1"
                           + ",,"   // empty middle entry
                           + QString("RTTY") + QChar(0x7F) + "v2";
        m.handleInstalledList(makeKvs({{"installed_list", list}}));

        report("installedList: empty entry skipped", m.waveforms().size() == 2);
    }

    // ── installed_list — tolerate double DEL emitted by some legacy packages ─
    {
        FlexWaveformModel m;

        const QString list = QString("ThumbDV") + QChar(0x7F) + QChar(0x7F) + "v1.2.0";
        m.handleInstalledList(makeKvs({{"installed_list", list}}));

        report("installedList: double DEL entry retained", m.waveforms().size() == 1);
        report("installedList: double DEL name", m.waveforms()[0].name == "ThumbDV");
        report("installedList: double DEL version", m.waveforms()[0].version == "v1.2.0");
    }

    // ── container add ──────────────────────────────────────────────────────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::waveformsChanged);

        m.handleContainerStatus(makeKvs({{"name", "MyWaveform"}, {"version", "2.1"}}));

        report("containerAdd: waveformsChanged emitted",    spy.size() == 1);
        report("containerAdd: one entry",                   m.waveforms().size() == 1);
        report("containerAdd: name = MyWaveform",           m.waveforms()[0].name == "MyWaveform");
        report("containerAdd: version = 2.1",               m.waveforms()[0].version == "2.1");
        report("containerAdd: isContainer = true",          m.waveforms()[0].isContainer);
    }

    // ── container remove (bare-word 'removed' key) ─────────────────────────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::waveformsChanged);

        m.handleContainerStatus(makeKvs({{"name", "MyWaveform"}, {"version", "2.1"}}));
        spy.clear();

        // "removed" is a bare-word key — value is empty string in the parsed kvs
        m.handleContainerStatus(makeKvs({{"name", "MyWaveform"}, {"removed", ""}}));

        report("containerRemove: waveformsChanged emitted", spy.size() == 1);
        report("containerRemove: list is now empty",        m.waveforms().isEmpty());
    }

    // ── wfp_status — all three fields, case-insensitive booleans ──────────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::wfpStatusChanged);

        m.handleWfpStatus(makeKvs({{"power", "on"}, {"ready", "true"}, {"ipaddr", "192.168.1.10"}}));

        report("wfpStatus: wfpStatusChanged emitted",       spy.size() == 1);
        report("wfpStatus: status seen",                    m.wfpStatusSeen());
        report("wfpStatus: power=on → wfpPowered=true",     m.wfpPowered());
        report("wfpStatus: ready=true → wfpReady=true",     m.wfpReady());
        report("wfpStatus: ipaddr stored",                  m.wfpIpAddress() == "192.168.1.10");

        spy.clear();
        m.handleWfpStatus(makeKvs({{"power", "OFF"}, {"ready", "False"}}));
        report("wfpStatus: power=OFF (uppercase) → false",  !m.wfpPowered());
        report("wfpStatus: ready=False → false",            !m.wfpReady());
        report("wfpStatus: no change = no signal",          spy.size() == 1); // changed from prior state
    }

    // ── clear resets all state ─────────────────────────────────────────────
    {
        FlexWaveformModel m;
        m.handleInstalledList(makeKvs({{"installed_list", QString("X") + QChar(0x7F) + "1"}}));
        m.handleWfpStatus(makeKvs({{"power", "on"}, {"ready", "true"}, {"ipaddr", "10.0.0.1"}}));
        m.handleContainerStatus(makeKvs({{"name", "C"}, {"version", "1"}}));
        m.handleGenericStatus(makeKvs({{"name", "ExampleWaveform"}, {"phase", "complete"}}));

        m.clear();

        report("clear: waveforms empty",                    m.waveforms().isEmpty());
        report("clear: status reports empty",               m.statusReports().isEmpty());
        report("clear: wfpStatusSeen false",                !m.wfpStatusSeen());
        report("clear: wfpPowered false",                   !m.wfpPowered());
        report("clear: wfpReady false",                     !m.wfpReady());
        report("clear: wfpIpAddress empty",                 m.wfpIpAddress().isEmpty());
    }

    // ── generic waveform status reports are retained for diagnostics ───────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::statusReportsChanged);

        m.handleGenericStatus(makeKvs({
            {"name", "ExampleWaveform"},
            {"phase", "handshake"},
            {"result", "success"},
            {"backend", "termios"},
        }));

        report("genericStatus: statusReportsChanged emitted", spy.size() == 1);
        report("genericStatus: one report retained", m.statusReports().size() == 1);
        report("genericStatus: report phase retained",
               m.statusReports().constFirst().value("phase") == "handshake");
        report("genericStatus: report result retained",
               m.statusReports().constFirst().value("result") == "success");
    }

    // ── command names are protocol-safe tokens ─────────────────────────────
    {
        FlexWaveformModel m;
        QSignalSpy spy(&m, &FlexWaveformModel::commandReady);

        m.requestRestart(QStringLiteral("ThumbDV"));
        report("command: safe name emits command", spy.size() == 1);
        report("command: restart command text",
               spy.takeFirst().at(0).toString() == QStringLiteral("waveform restart ThumbDV"));

        m.requestRemoveContainer(QStringLiteral("Bad\nName"));
        report("command: newline name rejected", spy.isEmpty());

        m.requestUninstall(QStringLiteral("Bad Name"));
        report("command: whitespace name rejected", spy.isEmpty());
    }

    if (g_failed == 0)
        std::printf("\nAll FlexWaveformModel tests passed.\n");
    else
        std::printf("\n%d test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
