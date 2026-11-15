#include "Kpa500FaultHistory.h"

#include <array>

namespace AetherSDR::Kpa500 {
namespace {

constexpr int kImageSize = 1024;
constexpr int kNextSlotOffset = 0xfe;
constexpr int kRingOffset = 0x100;
constexpr int kSlotCount = 128;
constexpr int kRecordSize = 6;

struct FaultLabel {
    const char* name;
    const char* description;
};

constexpr std::array<FaultLabel, 16> kFaultLabels {{
    {"PWR ON", "Power was turned on"},
    {"PWR OFF", "Power lost without controlled shutdown"},
    {"HI CURR", "Excessive power amplifier current"},
    {"Unknown", "Unrecognized stored fault code"},
    {"HI TEMP", "Power amplifier temperature over limit"},
    {"Unknown", "Unrecognized stored fault code"},
    {"PWRIN HI", "Excessive driving power"},
    {"CNTR ERR", "Frequency counter failure"},
    {"60V HIGH", "60V supply over limit"},
    {"REFL HI", "Excessive reflected power (high SWR)"},
    {"Unknown", "Unrecognized stored fault code"},
    {"PA DISS", "Power amplifiers are dissipating excessive power"},
    {"POUT HI", "Excessive power output"},
    {"60V FAIL", "60V supply failure"},
    {"270V ERR", "270V supply failure"},
    {"GAIN ERR", "Excessive overall amplifier gain"},
}};

constexpr std::array<const char*, 16> kBands {{
    "Unknown", "60m", "160m", "80m", "40m", "30m", "20m", "17m",
    "15m", "12m", "10m", "6m", "Unknown", "Unknown", "Unknown", "Unknown",
}};

quint8 byteAt(const QByteArray& image, int offset)
{
    return static_cast<quint8>(image.at(offset));
}

} // namespace

QVector<FaultEntry> decodeFaultHistory(const QByteArray& image, QString* error)
{
    if (error) {
        error->clear();
    }
    if (image.size() != kImageSize) {
        if (error) {
            *error = QStringLiteral("Fault history requires exactly 1024 EEPROM bytes.");
        }
        return {};
    }
    const int nextSlot = byteAt(image, kNextSlotOffset);
    if (nextSlot >= kSlotCount) {
        if (error) {
            *error = QStringLiteral("Fault history has an invalid next-slot index (%1).")
                         .arg(nextSlot);
        }
        return {};
    }

    // KPA Utility 1.13.7.16 virtual-COM observations: EEPROM 0xfe is the next
    // write slot, and each record is uptime24/packed-band-code/data16, little
    // endian. Uptime resets on power-on, so it cannot establish chronology.
    QVector<FaultEntry> entries;
    entries.reserve(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i) {
        const int slot = (nextSlot + kSlotCount - 1 - i) % kSlotCount;
        const int offset = kRingOffset + slot * kRecordSize;
        bool empty = true;
        for (int j = 0; j < kRecordSize; ++j) {
            empty = empty && byteAt(image, offset + j) == 0xff;
        }
        if (empty) {
            continue;
        }

        FaultEntry entry;
        entry.slot = slot;
        entry.uptimeSeconds = static_cast<quint32>(byteAt(image, offset))
            | (static_cast<quint32>(byteAt(image, offset + 1)) << 8)
            | (static_cast<quint32>(byteAt(image, offset + 2)) << 16);
        const quint8 packed = byteAt(image, offset + 3);
        entry.code = packed == 0xff ? 0xff : packed & 0x0f;
        entry.band = QString::fromLatin1(kBands[packed >> 4]);
        entry.data = static_cast<quint16>(byteAt(image, offset + 4)
            | (static_cast<quint16>(byteAt(image, offset + 5)) << 8));
        if (entry.code == 0xff) {
            entry.name = QStringLiteral("NO FAULT");
            entry.description = QStringLiteral("No fault recorded");
        } else {
            const FaultLabel& label = kFaultLabels[entry.code];
            entry.name = QString::fromLatin1(label.name);
            entry.description = QString::fromLatin1(label.description);
        }
        entries.append(entry);
    }
    return entries;
}

} // namespace AetherSDR::Kpa500
