#pragma once

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class RadioModel;
class LogManager;

// Collects diagnostic files into an archive and opens the user's
// email client for sending to support. Used by SupportDialog.

class SupportBundle {
public:
    struct SystemInfo {
        QString aetherVersion;
        QString qtVersion;
        QString osName;
        QString kernelVersion;
        QString cpuArch;
        QString buildDate;
        QString cpu;  // model + arch + SIMD features (SystemInventory::cpuSummary)
        QString ram;  // total physical RAM (SystemInventory::ramSummary)
        QString gpu;  // render GPU decision (GpuSelector::appliedSummary)
    };

    struct RadioInfo {
        QString model;
        QString serial;
        QString firmware;        // radio software version from discovery (e.g. "4.1.5")
        QString protocolVersion; // SmartSDR protocol version from V line (e.g. "1.4.0.0")
        QString callsign;
        QString ip;
        bool connected{false};
    };

    // Collect system info from QSysInfo + app version.
    static SystemInfo collectSystemInfo();

    // The bundle's system-info.json content. Header-inline and pure so the
    // regression test can pin the field set without SupportBundle.cpp's
    // model/zip dependencies: the archive is the structured artifact that
    // survives a user editing the mail body, so the CPU/RAM facts that
    // decide hardware-dependent crash reports (#4986) must be HERE, not
    // only in the email text.
    static QJsonObject systemInfoJson(const SystemInfo& sys)
    {
        QJsonObject obj;
        obj["aetherVersion"] = sys.aetherVersion;
        obj["qtVersion"]     = sys.qtVersion;
        obj["os"]            = sys.osName;
        obj["kernel"]        = sys.kernelVersion;
        // "cpu" carries the full model + arch + SIMD summary; "arch" keeps
        // the bare architecture string.
        obj["cpu"]           = sys.cpu;
        obj["arch"]          = sys.cpuArch;
        obj["ram"]           = sys.ram;
        obj["gpu"]           = sys.gpu;
        obj["buildDate"]     = sys.buildDate;
        return obj;
    }

    // Collect radio info from RadioModel (safe if null/disconnected).
    static RadioInfo collectRadioInfo(const RadioModel* model);

    // Create a timestamped support bundle archive.
    // Returns the full path to the archive, or empty string on failure.
    static QString createBundle(const RadioInfo& radio);

    // Flush the log pipeline and return the last `lines` lines of the most
    // recent on-disk log (#3705).  The on-disk log is already secret-redacted
    // at capture (AsyncLogWriter::formatLine → redactPii), so this is the
    // redacted stream; callers still re-scrub at the render boundary as
    // belt-and-suspenders.  Reads a bounded tail (~200KB) to stay responsive.
    static QString recentLogTail(int lines);

    // Open the default email client with pre-filled subject/body.
    static void openEmailClient(const QString& bundlePath,
                                const SystemInfo& sys,
                                const RadioInfo& radio);
};

} // namespace AetherSDR
