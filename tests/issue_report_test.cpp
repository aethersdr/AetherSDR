#include "core/IssueReport.h"
#include "core/SupportBundle.h"

#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

SupportBundle::SystemInfo sampleSys()
{
    SupportBundle::SystemInfo sys;
    sys.aetherVersion = "26.7.3";
    sys.qtVersion     = "6.6.2";
    sys.osName        = "Test OS";
    sys.kernelVersion = "1.2.3";
    sys.cpuArch       = "x86_64";
    sys.buildDate     = "Jan 1 2026";
    sys.cpu           = "Test CPU 9000 (x86_64; SSE42 AVX AVX2)";
    sys.ram           = "16384 MB";
    sys.gpu           = "Test GPU (auto)";
    return sys;
}

SupportBundle::RadioInfo sampleRadio()
{
    SupportBundle::RadioInfo radio;
    radio.connected       = true;
    radio.model           = "FLEX-6600";
    radio.serial          = "4424-1213-8600-7836";
    radio.firmware        = "4.1.5";
    radio.protocolVersion = "1.4.0.0";
    radio.callsign        = "W1AW";
    radio.ip              = "192.168.50.121";
    return radio;
}

// Acceptance: a planted secret in the log tail must NOT survive into the
// rendered body — redaction happens at the render boundary.
void testPlantedSecretIsRedacted()
{
    const QString logTail =
        "[12:00:00.000] INF aether.wan: access_token=SECRET123abcdefghijklmnop payload\n"
        "[12:00:00.100] INF aether.wan: refresh_token=RTOPSECRETdeadbeef0123456789\n"
        "[12:00:00.200] INF aether.wan: Authorization: Bearer TOPSECRETjwtheaderpayload\n"
        "[12:00:00.300] DBG aether.wan: application user_settings "
        "first_name=Pat last_name=Jensen\n"
        "[12:00:00.400] DBG aether.connection: gps "
        "lat=47.6205#lon=-122.3493#grid=CN87";

    const QString body = buildIssueReport(sampleSys(), sampleRadio(), logTail);

    report("planted access_token value is absent from body",
           !body.contains("SECRET123abcdefghijklmnop"));
    report("planted refresh_token value is absent from body",
           !body.contains("RTOPSECRETdeadbeef0123456789"));
    report("planted bearer token value is absent from body",
           !body.contains("TOPSECRETjwtheaderpayload"));
    report("SmartLink full name is absent from body",
           !body.contains("Pat") && !body.contains("Jensen"));
    report("GPS coordinates are absent from body",
           !body.contains("47.6205") && !body.contains("-122.3493"));
    report("redaction marker is present where a secret was",
           body.contains("***REDACTED***"));
    report("log block is fenced as ```text",
           body.contains("```text"));
}

// Serial and IP are PII and must be scrubbed even when they appear in the
// snapshot; callsign/model are public and kept.
void testRadioPiiScrubbedButPublicKept()
{
    const QString body = buildIssueReport(sampleSys(), sampleRadio(), QString());
    report("radio serial is redacted in body",
           !body.contains("4424-1213-8600-7836") &&
           body.contains("****-****-****-7836"));
    report("callsign is preserved",  body.contains("W1AW"));
    report("model is preserved",     body.contains("FLEX-6600"));
    report("firmware is preserved",  body.contains("4.1.5"));
}

// Empty tail => the log block is replaced by the omission note (URL-trim path).
void testOmittedLogNote()
{
    const QString body = buildIssueReport(sampleSys(), sampleRadio(), QString());
    report("empty tail omits the fenced block",
           !body.contains("```text"));
    report("empty tail substitutes the omission note",
           body.contains("Recent log omitted"));
}

// Body carries the sectioned snapshot and the bug placeholders.
void testSnapshotSectionsPresent()
{
    const QString body = buildIssueReport(sampleSys(), sampleRadio(), "line");
    report("What happened section present",  body.contains("### What happened?"));
    report("OS & version section present",   body.contains("### OS & version"));
    report("AetherSDR version filled in",    body.contains("26.7.3"));
    report("Qt version filled in",           body.contains("6.6.2"));
    report("specific privacy notice is present",
           body.contains("GPS coordinates")
           && body.contains("SmartLink account names"));
    report("points a crash reporter at the crash-report guide",
           body.contains("docs/debugging-crashes.md#for-users-what-to-attach-to-the-issue"));
}

// The bundle's system-info.json is the structured artifact that survives a
// user editing the mail body, so the CPU/RAM facts that decide
// hardware-dependent crash reports (#4986) must be pinned there — not only
// in the email text.
void testBundleSystemInfoJsonCarriesHardwareFacts()
{
    const QJsonObject obj = SupportBundle::systemInfoJson(sampleSys());
    report("json cpu carries the full model+features summary",
           obj.value("cpu").toString() == "Test CPU 9000 (x86_64; SSE42 AVX AVX2)");
    report("json keeps the bare arch string",
           obj.value("arch").toString() == "x86_64");
    report("json carries total RAM",
           obj.value("ram").toString() == "16384 MB");
    report("json carries the render GPU decision",
           obj.value("gpu").toString() == "Test GPU (auto)");
    report("json keeps the pre-existing os/kernel/version fields",
           obj.value("os").toString() == "Test OS"
           && obj.value("kernel").toString() == "1.2.3"
           && obj.value("aetherVersion").toString() == "26.7.3");
}

SupportBundle::RadioInfo sampleHl2Radio()
{
    SupportBundle::RadioInfo radio;
    radio.connected       = true;
    radio.model           = "Hermes-Lite 2";
    radio.serial          = "00:1C:C0:A2:13:DD";
    radio.firmware        = "75";
    radio.ip              = "192.168.50.77";
    radio.hl2.present     = true;
    radio.hl2.boardVariant = "AK4951 (HL2+ companion board)";
    radio.hl2.sampleRateHz = 192000;
    radio.hl2.receivers    = 2;
    radio.hl2.txModulator  = "wdsp-txa";
    radio.hl2.mode         = "USB, CWU";
    radio.hl2.droppedPackets = 12;
    radio.hl2.ep4Drops     = 0;
    radio.hl2.txFifoFault  = 1;
    radio.hl2.dspUnderruns = 3;
    radio.hl2.dspFaults    = 0;
    radio.hl2.txModulatorFaultBlocks = 5;
    return radio;
}

// An HL2 report carries the Hermes-Lite 2 block with the facts the app holds.
void testHl2BlockPresent()
{
    const QString body = buildIssueReport(sampleSys(), sampleHl2Radio(), QString());
    report("HL2 section heading present",       body.contains("### Hermes-Lite 2\n"));
    report("gateware line",                     body.contains("- Gateware: 75\n"));
    report("board variant line",
           body.contains("- Board variant: AK4951 (HL2+ companion board)\n"));
    report("sample rate line",                  body.contains("- Sample rate: 192000 Hz\n"));
    report("receivers line",                    body.contains("- Receivers: 2\n"));
    report("TX path line",                      body.contains("- TX path: wdsp-txa\n"));
    report("mode line",                         body.contains("- Mode: USB, CWU\n"));
    report("dropped EP6 packets line",          body.contains("- Dropped EP6 packets: 12\n"));
    report("dropped EP4 packets line",          body.contains("- Dropped EP4 packets: 0\n"));
    report("TX pacing fault line",              body.contains("- TX pacing fault: yes\n"));
    report("DSP underruns line",                body.contains("- DSP pipeline underruns: 3\n"));
    report("DSP faults line",                   body.contains("- DSP processing faults: 0\n"));
    report("TX modulator fault blocks line",
           body.contains("- TX modulator fault blocks: 5\n"));
    report("HL2 section sits before OS & version",
           body.indexOf("### Hermes-Lite 2") < body.indexOf("### OS & version"));

    // Unreported values are left out, never printed as zero.
    SupportBundle::RadioInfo sparse = sampleHl2Radio();
    sparse.hl2 = SupportBundle::Hl2Info{};
    sparse.hl2.present = true;
    const QString sparseBody = buildIssueReport(sampleSys(), sparse, QString());
    report("unreported HL2 values are omitted",
           sparseBody.contains("### Hermes-Lite 2\n")
           && !sparseBody.contains("Sample rate")
           && !sparseBody.contains("Dropped EP6")
           && !sparseBody.contains("TX path"));
}

// No HL2 section for any other radio, and the report is exactly what it was:
// the HL2 fields on a Flex or disconnected radio are ignored.
void testHl2BlockAbsentElsewhere()
{
    const QString flex = buildIssueReport(sampleSys(), sampleRadio(), "line");
    report("Flex report has no HL2 section", !flex.contains("Hermes-Lite 2"));

    SupportBundle::RadioInfo flexWithStray = sampleRadio();
    flexWithStray.hl2.droppedPackets = 99;    // present stays false
    flexWithStray.hl2.sampleRateHz   = 48000;
    report("Flex report is byte-identical when hl2.present is false",
           buildIssueReport(sampleSys(), flexWithStray, "line") == flex);

    SupportBundle::RadioInfo none;
    const QString noRadio = buildIssueReport(sampleSys(), none, QString());
    report("no-radio report has no HL2 section", !noRadio.contains("Hermes-Lite 2"));

    SupportBundle::RadioInfo gone = sampleHl2Radio();
    gone.connected = false;
    report("disconnected radio gets no HL2 section",
           !buildIssueReport(sampleSys(), gone, QString()).contains("### Hermes-Lite 2"));
}

// The HL2's serial is its MAC: redacted like any serial, never listed whole.
void testHl2MacRedacted()
{
    const QString body = buildIssueReport(sampleSys(), sampleHl2Radio(), QString());
    report("HL2 MAC is absent from body",
           !body.contains("00:1C:C0:A2:13:DD") && !body.contains("A2:13"));
    report("HL2 serial keeps only its last octet",
           body.contains("**:**:**:**:**:DD"));
    report("radio IP is absent from body", !body.contains("192.168.50.77"));
}

} // namespace

int main(int, char**)
{
    testPlantedSecretIsRedacted();
    testRadioPiiScrubbedButPublicKept();
    testOmittedLogNote();
    testSnapshotSectionsPresent();
    testBundleSystemInfoJsonCarriesHardwareFacts();
    testHl2BlockPresent();
    testHl2BlockAbsentElsewhere();
    testHl2MacRedacted();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
