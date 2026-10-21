#include "core/Kpa500FaultHistory.h"

#include <array>
#include <cstdio>

using namespace AetherSDR::Kpa500;

namespace {
int gFailures = 0;

void report(const char* name, bool passed)
{
    std::printf("%s %s\n", passed ? "[ OK ]" : "[FAIL]", name);
    if (!passed) {
        ++gFailures;
    }
}

QByteArray emptyImage()
{
    QByteArray image(1024, char(0xff));
    image[0xfe] = 0;
    return image;
}

void putRecord(QByteArray& image, int slot, const char* hex)
{
    image.replace(0x100 + slot * 6, 6, QByteArray::fromHex(hex));
}
} // namespace

int main()
{
    QString error;
    report("rejects a truncated EEPROM image",
           decodeFaultHistory(QByteArray(1023, 0), &error).isEmpty() && !error.isEmpty());
    report("rejects an oversized EEPROM image",
           decodeFaultHistory(QByteArray(1025, 0), &error).isEmpty() && !error.isEmpty());
    report("rejects an absent EEPROM image",
           decodeFaultHistory({}, &error).isEmpty() && !error.isEmpty());

    QByteArray image = emptyImage();
    report("valid empty ring clears a previous error",
           decodeFaultHistory(image, &error).isEmpty() && error.isEmpty());
    for (const int badPointer : {128, 255}) {
        image[0xfe] = char(badPointer);
        report("rejects an out-of-range ring pointer",
               decodeFaultHistory(image, &error).isEmpty() && !error.isEmpty());
    }

    // Golden records and display values observed in Utility 1.13.7.16 through
    // an isolated virtual COM pair. These are artificial data, not real faults.
    image = emptyImage();
    putRecord(image, 0, "010000699401");
    putRecord(image, 1, "020000367803");
    putRecord(image, 2, "030000102000");
    QVector<FaultEntry> entries = decodeFaultHistory(image, &error);
    report("three populated records exclude erased slots", entries.size() == 3 && error.isEmpty());
    if (entries.size() == 3) {
        report("stored slot2 is newest when following the ring",
               entries[0].slot == 2 && entries[0].uptimeSeconds == 3
               && entries[0].code == 0 && entries[0].name == "PWR ON"
               && entries[0].band == "60m" && entries[0].data == 0x20);
        report("stored band encoding differs from normal BN encoding",
               entries[1].slot == 1 && entries[1].code == 6
               && entries[1].name == "PWRIN HI" && entries[1].band == "80m"
               && entries[1].data == 0x378);
        report("reflected power record preserves both data bytes",
               entries[2].slot == 0 && entries[2].code == 9
               && entries[2].name == "REFL HI" && entries[2].band == "20m"
               && entries[2].data == 0x194
               && entries[2].description.contains("reflected"));
    }

    image = emptyImage();
    putRecord(image, 0, "840300600000");
    putRecord(image, 1, "010000610100");
    putRecord(image, 2, "F40100620200");
    putRecord(image, 3, "000000630300");
    entries = decodeFaultHistory(image);
    report("uptime resets do not reorder history",
           entries.size() == 4 && entries[0].uptimeSeconds == 0
           && entries[1].uptimeSeconds == 500 && entries[2].uptimeSeconds == 1
           && entries[3].uptimeSeconds == 900);

    image = emptyImage();
    image[0xfe] = 5;
    for (int slot = 0; slot < 128; ++slot) {
        putRecord(image, slot, "010000623412");
    }
    entries = decodeFaultHistory(image);
    bool matchesRing = entries.size() == 128;
    for (int i = 0; i < entries.size(); ++i) {
        matchesRing = matchesRing && entries[i].slot == (132 - i) % 128;
    }
    report("next-slot5 traverses4 to0 then127 to5 exactly once", matchesRing);

    image = emptyImage();
    putRecord(image, 0, "FFFFFF623412");
    putRecord(image, 1, "010000FFFFFF");
    putRecord(image, 2, "FFFFFE623412");
    putRecord(image, 3, "000000FFFFFF");
    putRecord(image, 4, "FFFFFFFFFFFF");
    putRecord(image, 5, "000000000000");
    image[0xfe] = 6;
    entries = decodeFaultHistory(image);
    report("only an all-FF record is erased", entries.size() == 5);
    if (entries.size() == 5) {
        report("zero-filled records remain valid power-on events",
               entries[0].slot == 5 && entries[0].code == 0
               && entries[0].uptimeSeconds == 0);
        report("FF fault byte is distinct from gain-error code0F",
               entries[1].slot == 3 && entries[1].code == 0xff
               && entries[1].name == "NO FAULT" && entries[1].data == 0xffff);
        report("24-bit maximum timestamp is not an empty marker",
               entries[4].slot == 0 && entries[4].uptimeSeconds == 0xffffff
               && entries[2].uptimeSeconds == 0xfeffff);
    }

    const std::array<const char*, 16> observedBands {{
        "Unknown", "60m", "160m", "80m", "40m", "30m", "20m", "17m",
        "15m", "12m", "10m", "6m", "Unknown", "Unknown", "Unknown", "Unknown",
    }};
    const std::array<const char*, 16> observedNames {{
        "PWR ON", "PWR OFF", "HI CURR", "Unknown", "HI TEMP", "Unknown",
        "PWRIN HI", "CNTR ERR", "60V HIGH", "REFL HI", "Unknown", "PA DISS",
        "POUT HI", "60V FAIL", "270V ERR", "GAIN ERR",
    }};
    bool bandsMatch = true;
    bool namesMatch = true;
    for (int nibble = 0; nibble < 16; ++nibble) {
        image = emptyImage();
        putRecord(image, 0, "010000003412");
        image[0x103] = char(nibble << 4);
        entries = decodeFaultHistory(image);
        bandsMatch = bandsMatch && entries.size() == 1
            && entries[0].band == QString::fromLatin1(observedBands[nibble]);
        image[0x103] = char(0x60 | nibble);
        entries = decodeFaultHistory(image);
        namesMatch = namesMatch && entries.size() == 1
            && entries[0].code == nibble
            && entries[0].name == QString::fromLatin1(observedNames[nibble]);
    }
    report("all16 band nibbles match observed Utility sweep", bandsMatch);
    report("all16 fault codes match observed Utility sweep", namesMatch);
    return gFailures == 0 ? 0 : 1;
}
