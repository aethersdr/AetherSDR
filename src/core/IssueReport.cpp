#include "IssueReport.h"
#include "AsyncLogWriter.h"  // redactPii — GHSA-ccrg-j8cp-qhc4

namespace AetherSDR {

QString buildIssueReport(const SupportBundle::SystemInfo& sys,
                         const SupportBundle::RadioInfo& radio,
                         const QString& logTail)
{
    QString body;

    // State the concrete privacy guarantees without implying that arbitrary
    // user-authored log text can be proven free of every kind of PII.
    body += "<!-- Pre-filled by AetherSDR (Help \xE2\x86\x92 File an Issue). "
            "Known sensitive fields are redacted: authentication tokens, "
            "network/radio identifiers, GPS coordinates, and SmartLink "
            "account names. Please review the report, then "
            "replace the italic placeholders with your own words. -->\n\n";

    body += "### What happened?\n";
    body += "_Describe what went wrong (e.g. \"the waterfall freezes after "
            "about 10 minutes\")._\n\n";

    body += "### What did you expect?\n";
    body += "_Describe the expected behavior._\n\n";

    body += "### Steps to reproduce\n";
    body += "1. _First step_\n";
    body += "2. _\xE2\x80\xA6_\n\n";

    body += "### Radio model & firmware\n";
    if (radio.connected) {
        // Serial/IP are PII — redact to the same form used in logs so support
        // can correlate without seeing cleartext.  Callsign is FCC public
        // record; model/firmware/protocol are not sensitive.
        body += QString("- Model: %1\n").arg(radio.model);
        body += QString("- Firmware: %1\n").arg(radio.firmware);
        if (!radio.protocolVersion.isEmpty()) {
            body += QString("- Protocol: %1\n").arg(radio.protocolVersion);
        }
        if (!radio.callsign.isEmpty()) {
            body += QString("- Callsign: %1\n").arg(radio.callsign);
        }
        if (!radio.serial.isEmpty()) {
            body += QString("- Serial: %1\n").arg(redactPii(radio.serial));
        }
        body += "- Connection: connected\n\n";
    } else {
        body += "- Connection: not connected\n\n";
    }

    body += "### OS & version\n";
    body += QString("- AetherSDR: %1\n").arg(sys.aetherVersion);
    body += QString("- Qt: %1\n").arg(sys.qtVersion);
    body += QString("- OS: %1 (kernel %2)\n").arg(sys.osName, sys.kernelVersion);
    body += QString("- Arch: %1\n").arg(sys.cpuArch);
    body += QString("- Build: %1\n\n").arg(sys.buildDate);

    body += "### Recent log\n";
    if (logTail.isEmpty()) {
        body += "_(Recent log omitted from this link to keep it short \xE2\x80\x94 "
                "see the attached support bundle, or paste it from your "
                "clipboard.)_\n";
    } else {
        body += QString("Last %1 lines, secret-redacted:\n\n")
                    .arg(kIssueLogTailLines);
        // Redact at the render boundary: even though the on-disk tail is
        // already scrubbed by AsyncLogWriter::formatLine, re-run redactPii so
        // the guarantee holds regardless of the tail's provenance.
        body += "```text\n";
        body += redactPii(logTail).trimmed();
        body += "\n```\n";
    }

    return body;
}

} // namespace AetherSDR
