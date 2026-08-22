#include "MemoryFieldValues.h"

#include <cmath>

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

const QStringList& extendedToneModesDisplay()
{
    static const QStringList kModes = {
        "OFF", "CTCSS_TX", "CTCSS_RX", "CTCSS_TXRX",
        "DTCS_TX", "DTCS_TXRX", "CTCSS_TX_DTCS_RX",
        "DTCS_TX_CTCSS_RX"
    };
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

const QVector<CtcssChoice>& ctcssChoices()
{
    static const QVector<CtcssChoice> kChoices = {
        { 1, "XZ", "67.0"},  { 2, "XA", "71.9"},  { 3, "WA", "74.4"},
        { 4, "XB", "77.0"},  { 5, "WB", "79.7"},  { 6, "YZ", "82.5"},
        { 7, "YA", "85.4"},  { 8, "YB", "88.5"},  { 9, "ZZ", "91.5"},
        {10, "ZA", "94.8"},  {11, "ZB", "97.4"},  {12, "1Z", "100.0"},
        {13, "1A", "103.5"}, {14, "1B", "107.2"}, {15, "2Z", "110.9"},
        {16, "2A", "114.8"}, {17, "2B", "118.8"}, {18, "3Z", "123.0"},
        {19, "3A", "127.3"}, {20, "3B", "131.8"}, {21, "4Z", "136.5"},
        {22, "4A", "141.3"}, {23, "4B", "146.2"}, {24, "5Z", "151.4"},
        {25, "5A", "156.7"}, {26, "5B", "162.2"}, {27, "6Z", "167.9"},
        {28, "6A", "173.8"}, {29, "6B", "179.9"}, {30, "7Z", "186.2"},
        {31, "7A", "192.8"}, {32, "M1", "203.5"}, {33, "8Z", "206.5"},
        {34, "M2", "210.7"}, {35, "M3", "218.1"}, {36, "M4", "225.7"},
        {37, "9Z", "229.1"}, {38, "M5", "233.6"}, {39, "M6", "241.8"},
        {40, "M7", "250.3"}, {41, "0Z", "254.1"},
    };
    return kChoices;
}

const QList<int>& dtcsCodes()
{
    static const QList<int> kCodes = {
        23,25,26,31,32,36,43,47,51,53,54,65,71,72,73,74,
        114,115,116,122,125,131,132,134,143,145,152,155,156,162,165,172,174,
        205,212,223,225,226,243,244,245,246,251,252,255,261,263,265,266,271,
        274,306,311,315,325,331,332,343,346,351,356,364,365,371,411,412,413,
        423,431,432,445,446,452,454,455,462,464,465,466,503,506,516,523,526,
        532,546,565,606,612,624,627,631,632,654,662,664,703,712,723,731,732,
        734,746,754,
    };
    return kCodes;
}

bool isCtcssTone(double hz)
{
    if (!std::isfinite(hz)) {
        return false;
    }
    for (const CtcssChoice& choice : ctcssChoices()) {
        if (std::abs(choice.frequency.toDouble() - hz) < 0.01) {
            return true;
        }
    }
    return false;
}

bool isDtcsCode(int code)
{
    return dtcsCodes().contains(code);
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
    if (upper == "CTCSS_RX") { return "ctcss_rx"; }
    if (upper == "CTCSS_TXRX") { return "ctcss_txrx"; }
    if (upper == "DTCS_TX") { return "dtcs_tx"; }
    if (upper == "DTCS_TXRX") { return "dtcs_txrx"; }
    if (upper == "CTCSS_TX_DTCS_RX") { return "ctcss_tx_dtcs_rx"; }
    if (upper == "DTCS_TX_CTCSS_RX") { return "dtcs_tx_ctcss_rx"; }
    return {};
}

QString toneModeToDisplay(const QString& any)
{
    const QString upper = sanitizeText(any).trimmed().toUpper();
    if (extendedToneModesDisplay().contains(upper)) { return upper; }
    return "OFF";
}

QString modeToWire(const QString& any)
{
    return sanitizeText(any).trimmed().toUpper();
}

} // namespace AetherSDR::MemoryFields
