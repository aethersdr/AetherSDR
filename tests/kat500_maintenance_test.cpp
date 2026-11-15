#include <cstdio>
#include "core/Kat500Maintenance.h"
#include <QFile>
#include <QTimeZone>
using namespace AetherSDR::Kat500;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main()
{
    CHECK(buildEepromRead(false, 0, 64) == "ERP000040C0;");
    CHECK(buildEepromRead(false, 2, 1) == "ERP000201FD;");
    CHECK(buildEepromWrite(false, 0, QByteArray(32,'x')).isEmpty());
    CHECK(buildEepromWrite(true, 4095, QByteArray(32,'x')).isEmpty());
    CHECK(!parseEepromReply("ERP000201FE00"));
    CHECK(parseEepromReply("ERP00020100FD")->data == QByteArray(1, '\0'));
    QFile fixture(QStringLiteral(KAT500_FIXTURE_PATH)); CHECK(fixture.open(QIODevice::ReadOnly));
    const QByteArray original = fixture.readAll(); QString error;
    const auto backup = decodeBackup(original, &error);
    CHECK(backup && backup->internal.size() == 1024 && backup->external.size() == 65536);
    CHECK(backup->firmware == "02.16" && backup->serial == "12345");
    QByteArray corrupt = original; corrupt[500] = 'E'; CHECK(!decodeBackup(corrupt));
    CHECK(!decodeBackup(original.left(original.size()/2)));
    const int recordStart = original.indexOf("MEM2 ");
    const int recordLength = original.indexOf('\n',recordStart) - recordStart + 1;
    QByteArray missing = original; missing.remove(recordStart,recordLength);
    missing.replace("ECCFA104","C006FDBE");
    CHECK(!decodeBackup(missing)); // Correct independently calculated CRC, missing coverage.
    QByteArray duplicate = original; duplicate.insert(recordStart,original.mid(recordStart,recordLength));
    duplicate.replace("ECCFA104","92719607"); CHECK(!decodeBackup(duplicate));
    Backup varied = *backup;
    for (int i = 0; i < varied.internal.size(); ++i) { varied.internal[i] = static_cast<char>(i * 17); }
    for (int i = 0; i < varied.external.size(); ++i) { varied.external[i] = static_cast<char>(i ^ (i >> 8)); }
    const QByteArray variedEncoding = encodeBackup(varied, QDateTime::fromSecsSinceEpoch(0,QTimeZone::UTC));
    // Independently calculated with Python zlib over the documented native lines.
    CHECK(variedEncoding.endsWith("CRC32 = 0x69488867\r\n"));
    const auto roundtrip = decodeBackup(variedEncoding);
    CHECK(roundtrip && roundtrip->internal == varied.internal && roundtrip->external == varied.external);
    QByteArray faults(4096, char(0xff)); faults.replace(255*16,16,QByteArray(16,0)); faults.replace(0,16,QByteArray(16,0));
    faults[0] = 2; faults[1] = 1; faults[2] = 1; faults[10] = 7; faults[11] = 8;
    const QList<FaultRecord> records = decodeFaultHistory(0, faults);
    CHECK(records.size() == 2 && records[0].slot == 0 && records[1].slot == 255);
    CHECK(records[0].elapsedSeconds == 257 && records[0].swr == 8.02734375);
    CHECK(records[1].description == "Power On" && records[1].raw.size() == 16);
    CHECK(decodeFaultHistory(256, faults).isEmpty());
    return 0;
}
