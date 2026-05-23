// Standalone test harness for OleCompoundFile.
//
// Verifies the reader against a real WiX MSI containing six cab*.cab
// streams. The test fixture is the SmartSDR v4.2.18 installer, which
// is FlexRadio's licensed property and therefore not checked into the
// repo. The test:
//   1. Looks for the MSI at $MasterSDR_TEST_MSI
//   2. Falls back to ~/build/reference/SmartSDR_v4.2.18_x64.msi
//   3. Skips with code 77 (autotools "skipped") if neither is present.
//
// Build: produced by CMake as `ole_compound_file_test`.
// Run:   ./build/ole_compound_file_test
// Exit:  0 = pass, 1 = fail, 77 = skipped (no fixture available).

#include "core/OleCompoundFile.h"
#include "core/CabExtractor.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdio>
#include <cstdlib>

using MasterSDR::OleCompoundFile;
using MasterSDR::CabExtractor;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {}) {
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toUtf8().constData());
    if (!ok) ++g_failed;
}

QString locateFixture() {
    if (auto* env = std::getenv("MasterSDR_TEST_MSI")) {
        const QString p = QString::fromUtf8(env);
        if (QFileInfo::exists(p)) return p;
    }
    const QString home = QString::fromUtf8(std::getenv("HOME") ? std::getenv("HOME") : "");
    const QString fallback = home + "/build/reference/SmartSDR_v4.2.18_x64.msi";
    if (QFileInfo::exists(fallback)) return fallback;
    return QString();
}

QString md5Of(const QByteArray& data) {
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(data);
    return QString::fromLatin1(hash.result().toHex().toLower());
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    const QString fixture = locateFixture();
    if (fixture.isEmpty()) {
        std::printf("[SKIP] no MSI fixture available — set MasterSDR_TEST_MSI\n");
        return 77;
    }
    std::printf("Using fixture: %s\n", fixture.toUtf8().constData());

    OleCompoundFile cfb;
    const bool opened = cfb.open(fixture);
    report("open() succeeds on real MSI", opened, cfb.lastError());
    if (!opened) return 1;

    const auto msiNames = cfb.msiStreamNames();
    report("MSI has at least 6 cab streams",
           [&]() {
               int cabCount = 0;
               for (const auto& n : msiNames)
                   if (n.startsWith("cab") && n.endsWith(".cab"))
                       ++cabCount;
               return cabCount >= 6;
           }(),
           QString("found %1 stream entries total").arg(msiNames.size()));

    // Read cab*.cab streams using the MSI-aware accessor.
    const auto cabs = cfb.readMsiStreamsByPrefixSuffix("cab", ".cab");
    report("readStreamsByPrefixSuffix returns 6 cabs",
           cabs.size() == 6,
           QString("got %1").arg(cabs.size()));

    // Validate cab1.cab and cab2.cab — these are the firmware-bearing ones.
    QByteArray cab1, cab2;
    for (const auto& [name, data] : cabs) {
        if (name == "cab1.cab") cab1 = data;
        if (name == "cab2.cab") cab2 = data;
    }
    report("cab1.cab present", !cab1.isEmpty());
    report("cab2.cab present", !cab2.isEmpty());

    // Cab files start with "MSCF" magic.
    report("cab1.cab starts with MSCF", cab1.startsWith("MSCF"),
           QString("first 8 bytes: %1").arg(QString::fromLatin1(cab1.left(8).toHex())));
    report("cab2.cab starts with MSCF", cab2.startsWith("MSCF"),
           QString("first 8 bytes: %1").arg(QString::fromLatin1(cab2.left(8).toHex())));

    // Sizes from the MSI File table (we measured these directly with 7z
    // extraction earlier in the project): cab1=64,953,412, cab2=386,480,873.
    report("cab1.cab size matches MSI File table",
           cab1.size() == 64953412,
           QString("got %1, expected 64953412").arg(cab1.size()));
    report("cab2.cab size matches MSI File table",
           cab2.size() == 386480873,
           QString("got %1, expected 386480873").arg(cab2.size()));

    // Cab MD5s — bit-identical to 7-Zip's MSI extraction (cross-checked).
    report("cab1.cab MD5 matches 7z extraction",
           md5Of(cab1) == "2e135892456cfe663d9a4ac146e10394",
           md5Of(cab1));
    report("cab2.cab MD5 matches 7z extraction",
           md5Of(cab2) == "93238ef77d8d65d5ccd8e4e738411dd2",
           md5Of(cab2));

    // CabExtractor — pull out every file in each cab, then locate the
    // firmware blob by Salted__ magic. cab1 has multiple files (driver
    // DLLs, INFs, etc. plus the FLEX-9600 firmware); cab2 has just the
    // FLEX-6x00 firmware blob.
    CabExtractor cx;

    QList<QPair<QString, QByteArray>> cab1Files;
    report("CabExtractor extracts all files from cab1",
           cx.extractAll(cab1, cab1Files), cx.lastError());
    std::printf("       cab1 contained %lld files:\n", (long long)cab1Files.size());
    for (const auto& [name, data] : cab1Files)
        std::printf("         - %s (%lld bytes)\n",
                    name.toUtf8().constData(), (long long)data.size());

    QByteArray ssdr6x00;
    report("CabExtractor finds Salted__ blob in cab2",
           cx.extractFirstMatchingMagic(cab2, "Salted__", ssdr6x00),
           cx.lastError());

    QByteArray ssdr9600;
    report("CabExtractor finds Salted__ blob in cab1",
           cx.extractFirstMatchingMagic(cab1, "Salted__", ssdr9600),
           cx.lastError());

    // The FLEX-6x00 firmware was the one we flashed to the radio
    // earlier this session — bit-identical match is required.
    report("FLEX-6x00 firmware MD5 matches the bytes we flashed",
           md5Of(ssdr6x00) == "9e8888dc0558ee420ed82f370f805025",
           md5Of(ssdr6x00));
    report("FLEX-6x00 firmware size",
           ssdr6x00.size() == 386289360,
           QString("got %1").arg(ssdr6x00.size()));

    // FLEX-9600 firmware (we don't have an authoritative MD5 — record
    // observed value as a regression check for future builds).
    std::printf("       FLEX-9600 firmware: size=%lld md5=%s\n",
                (long long)ssdr9600.size(),
                md5Of(ssdr9600).toUtf8().constData());

    if (g_failed == 0)
        std::printf("\nAll OleCompoundFile + CabExtractor tests passed.\n");
    else
        std::printf("\n%d test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
