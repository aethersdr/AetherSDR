#include "MemoryFieldValues.h"

namespace AetherSDR::MemoryFields {

QString sanitizeText(const QString& in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar ch : in) {
        const char16_t u = ch.unicode();
        // Drop C0 control chars (incl. NUL, TAB, CR, LF) and the DEL byte that
        // the protocol reserves as the space encoding.
        if (u < 0x20 || u == 0x7f)
            continue;
        out.append(ch);
    }
    return out;
}

const QStringList& modes()
{
    static const QStringList kModes = {
        "USB", "LSB", "CW", "AM", "SAM", "DSB",
        "FM", "NFM", "DFM", "DIGU", "DIGL", "RTTY",
        "FDV", "DSTR", "AME"
    };
    return kModes;
}

const QStringList& offsetDirectionsDisplay()
{
    static const QStringList kDirs = { "SIMPLEX", "UP", "DOWN" };
    return kDirs;
}

const QStringList& toneModesDisplay()
{
    static const QStringList kModes = { "OFF", "CTCSS_TX" };
    return kModes;
}

const QStringList& ctcssTones()
{
    static const QStringList kTones = {
        "67.0",  "69.3",  "71.9",  "74.4",  "77.0",  "79.7",  "82.5",  "85.4",
        "88.5",  "91.5",  "94.8",  "97.4",  "100.0", "103.5", "107.2", "110.9",
        "114.8", "118.8", "123.0", "127.3", "131.8", "136.5", "141.3", "146.2",
        "151.4", "156.7", "159.8", "162.2", "165.5", "167.9", "171.3", "173.8",
        "177.3", "179.9", "183.5", "186.2", "189.9", "192.8", "196.6", "199.5",
        "203.5", "206.5", "210.7", "218.1", "225.7", "229.1", "233.6", "241.8",
        "250.3", "254.1"
    };
    return kTones;
}

const QStringList& tuningSteps()
{
    static const QStringList kSteps = {
        "10", "100", "500", "1000", "2500", "5000",
        "6250", "9000", "10000", "12500", "25000", "50000", "100000"
    };
    return kSteps;
}

bool isKnownMode(const QString& mode)
{
    const QString upper = sanitizeText(mode).trimmed().toUpper();
    return modes().contains(upper);
}

QString offsetDirToWire(const QString& any)
{
    const QString upper = sanitizeText(any).trimmed().toUpper();
    if (upper == "UP")      return "up";
    if (upper == "DOWN")    return "down";
    if (upper == "SIMPLEX") return "simplex";
    return {};
}

QString offsetDirToDisplay(const QString& any)
{
    const QString upper = sanitizeText(any).trimmed().toUpper();
    if (upper == "UP")   return "UP";
    if (upper == "DOWN") return "DOWN";
    return "SIMPLEX";
}

QString toneModeToWire(const QString& any)
{
    const QString upper = sanitizeText(any).trimmed().toUpper();
    if (upper == "OFF")      return "off";
    if (upper == "CTCSS_TX") return "ctcss_tx";
    return {};
}

QString toneModeToDisplay(const QString& any)
{
    const QString upper = sanitizeText(any).trimmed().toUpper();
    if (upper == "CTCSS_TX") return "CTCSS_TX";
    return "OFF";
}

QString modeToWire(const QString& any)
{
    return sanitizeText(any).trimmed().toUpper();
}

} // namespace AetherSDR::MemoryFields
