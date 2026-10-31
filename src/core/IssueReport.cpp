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
    // Release builds ship symbols as release assets, so the OS crash report is
    // what turns "it crashed" into a stack trace (docs/debugging-crashes.md).
    body += "_If AetherSDR crashed, please attach the crash report your system "
            "kept: [where to find it](https://github.com/aethersdr/AetherSDR/blob/"
            "main/docs/debugging-crashes.md#for-users-what-to-attach-to-the-issue)._\n\n";

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

    if (radio.connected && radio.hl2.present) {
        // Hermes-Lite 2 only: what the app already holds that makes an HL2
        // report diagnosable. Lines the backend did not report are left out.
        // Run through redactPii like the log tail, so nothing here can carry
        // an identifier even if a field later gains one; the HL2's MAC (its
        // serial) is deliberately not listed, and its Serial line above is
        // already redacted.
        const SupportBundle::Hl2Info& hl2 = radio.hl2;
        QString block = "### Hermes-Lite 2\n";
        if (!radio.firmware.isEmpty())
            block += QString("- Gateware: %1\n").arg(radio.firmware);
        if (!hl2.boardVariant.isEmpty())
            block += QString("- Board variant: %1\n").arg(hl2.boardVariant);
        if (hl2.sampleRateHz > 0)
            block += QString("- Sample rate: %1 Hz\n").arg(hl2.sampleRateHz);
        if (hl2.receivers > 0)
            block += QString("- Receivers: %1\n").arg(hl2.receivers);
        if (!hl2.txModulator.isEmpty())
            block += QString("- TX path: %1\n").arg(hl2.txModulator);
        if (!hl2.mode.isEmpty())
            block += QString("- Mode: %1\n").arg(hl2.mode);
        if (hl2.droppedPackets >= 0)
            block += QString("- Dropped EP6 packets: %1\n").arg(hl2.droppedPackets);
        if (hl2.ep4Drops >= 0)
            block += QString("- Dropped EP4 packets: %1\n").arg(hl2.ep4Drops);
        if (hl2.txFifoFault >= 0)
            block += QString("- TX pacing fault: %1\n")
                         .arg(hl2.txFifoFault ? QStringLiteral("yes") : QStringLiteral("no"));
        if (hl2.dspUnderruns >= 0)
            block += QString("- DSP pipeline underruns: %1\n").arg(hl2.dspUnderruns);
        if (hl2.dspFaults >= 0)
            block += QString("- DSP processing faults: %1\n").arg(hl2.dspFaults);
        if (hl2.txModulatorFaultBlocks >= 0)
            block += QString("- TX modulator fault blocks: %1\n")
                         .arg(hl2.txModulatorFaultBlocks);
        body += redactPii(block);
        body += "\n";
    }

    body += "### OS & version\n";
    body += QString("- AetherSDR: %1\n").arg(sys.aetherVersion);
    body += QString("- Qt: %1\n").arg(sys.qtVersion);
    body += QString("- OS: %1 (kernel %2)\n").arg(sys.osName, sys.kernelVersion);
    // Model + arch + SIMD features in one line: the hardware facts that
    // decide GPU/ISA crash reports (#4986) — not sensitive, unlike serial/IP.
    body += QString("- CPU: %1\n").arg(sys.cpu);
    body += QString("- RAM: %1\n").arg(sys.ram);
    body += QString("- GPU: %1\n").arg(sys.gpu);
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
