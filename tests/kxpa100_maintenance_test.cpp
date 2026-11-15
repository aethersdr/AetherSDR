#include "core/Kxpa100Maintenance.h"
#include <QFile>
#include <QTimeZone>
#include <cstdio>
using namespace AetherSDR::Kxpa100;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main()
{
    CHECK(buildEepromRead(false, 0, 64) == "^ERP000040C0;");
    CHECK(buildEepromRead(true, 16384, 1).isEmpty());
    CHECK(buildEepromWrite(false, 31, QByteArray(1, 'x')).isEmpty());
    CHECK(buildEepromWrite(false, 224, QByteArray(64, 'x')).isEmpty());
    CHECK(!buildEepromWrite(false, 224, QByteArray(32, 'x')).isEmpty());
    CHECK(buildEepromWrite(true, 0x3800, QByteArray(1, 'x')).isEmpty());
    CHECK(buildEepromWrite(true, 0x37ff, QByteArray(2, 'x')).isEmpty());
    CHECK(!buildEepromWrite(true, 0x37c0, QByteArray(64, 'x')).isEmpty());
    CHECK(!parseEepromReply("^ERP000201FE00"));
    CHECK(parseEepromReply("^ERP00020100FD")->data == QByteArray(1, '\0'));
    CHECK(parseEepromReply("^EWP002040A0")->writeAck);
    CHECK(!parseEepromReply("^ERP00000000"));
    QFile fixture(QStringLiteral(KXPA100_FIXTURE_PATH)); CHECK(fixture.open(QIODevice::ReadOnly));
    const QByteArray original = fixture.readAll(); QString error;
    const auto backup = decodeBackup(original, &error);
    CHECK(backup && backup->internal.size() == 1024 && backup->external.size() == 16384);
    CHECK(backup->serial == "12345" && backup->firmware == "01.39");
    CHECK(restoreAllowed(*backup, "12345", "01.39").isEmpty());
    CHECK(!restoreAllowed(*backup, "00031", "01.39").isEmpty());
    CHECK(!restoreAllowed(*backup, "12345", "01.40").isEmpty());
    Backup newer = *backup; newer.firmware = "01.40";
    CHECK(!restoreAllowed(newer, "12345", "01.40").isEmpty());
    QByteArray bad = original; bad[500] = bad[500] == 'A' ? 'B' : 'A'; CHECK(!decodeBackup(bad));
    CHECK(!decodeBackup(original.left(original.size()/2)) && !decodeBackup(original + "EXTRA\n"));
    for (const char* name : {"missing", "duplicate", "invalid-metadata", "out-of-range"}) {
        QFile invalid(QStringLiteral(KXPA100_INVALID_FIXTURE_DIR) + '/' + QString::fromLatin1(name) + ".KXPAConfig");
        CHECK(invalid.open(QIODevice::ReadOnly)); CHECK(!decodeBackup(invalid.readAll()));
    }
    const auto roundtrip = decodeBackup(encodeBackup(*backup, QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC)));
    CHECK(roundtrip && roundtrip->internal == backup->internal && roundtrip->external == backup->external);
    QByteArray faults(2048, char(0xff));
    faults.replace(0, 8, QByteArray::fromHex("4301020304050607"));
    faults.replace(255*8, 8, QByteArray::fromHex("5000000000002301"));
    const auto records = decodeFaultHistory(0, faults);
    CHECK(records.size() == 2 && records[0].elapsedSeconds == 197121 && records[0].frequencyKhz == 1284);
    CHECK(records[0].value == 179.8 && records[0].unit == "A");
    CHECK(records[1].slot == 255 && records[1].value == 2.91 && records[1].unit == "W");
    for (const auto& item : QList<QPair<char,double>>{{'N',291},{'A',29.1},{'S',29.1},{'C',29.1},{'D',2.91},{'P',2.91},{'R',2.91},{'H',0.291},{'L',0.291},{'T',29.1}}) {
        QByteArray sample(2048, char(0xff)); QByteArray record = QByteArray::fromHex("0000000000002301"); record[0] = item.first;
        sample.replace(0, 8, record); CHECK(decodeFaultHistory(0, sample).front().value == item.second);
    }
    CHECK(decodeFaultHistory(256, faults).isEmpty() && decodeFaultHistory(0, faults.left(100)).isEmpty());
    return 0;
}
