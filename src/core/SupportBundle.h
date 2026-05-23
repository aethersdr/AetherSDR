#pragma once

#include <QString>

namespace MasterSDR {

class RadioModel;
class LogManager;

// Collects diagnostic files into an archive and opens the user's
// email client for sending to support. Used by SupportDialog.

class SupportBundle {
public:
    struct SystemInfo {
        QString masterVersion;
        QString qtVersion;
        QString osName;
        QString kernelVersion;
        QString cpuArch;
        QString buildDate;
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

    // Collect radio info from RadioModel (safe if null/disconnected).
    static RadioInfo collectRadioInfo(const RadioModel* model);

    // Create a timestamped support bundle archive.
    // Returns the full path to the archive, or empty string on failure.
    static QString createBundle(const RadioInfo& radio);

    // Open the default email client with pre-filled subject/body.
    static void openEmailClient(const QString& bundlePath,
                                const SystemInfo& sys,
                                const RadioInfo& radio);
};

} // namespace MasterSDR
