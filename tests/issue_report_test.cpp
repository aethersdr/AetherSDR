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
}

} // namespace

int main(int, char**)
{
    testPlantedSecretIsRedacted();
    testRadioPiiScrubbedButPublicKept();
    testOmittedLogNote();
    testSnapshotSectionsPresent();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
