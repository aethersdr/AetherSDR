// #7421 — finding FlexRadio's MD5 for the installer the firmware updater
// downloads. Until this fix the hash URL was hardcoded to the one upload folder
// that held v4.1.5's hash file, so every MSI-era release (4.2.18 onward) was
// staged unverified. FlexRadio puts each hash file in its own timestamped
// folder and attaches it to the release's software record, so the updater now
// reads index -> media -> hash file. These tests pin the three parses without
// a network, against responses saved from flexradio.com on 2026-11-25.

#include "core/FirmwareStager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <cstdio>

namespace {

using AetherSDR::FirmwareStager;

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// ─── Saved responses (verbatim, 2026-11-25) ────────────────────────────────

// wp-json/wp/v2/software?search=SmartSDR&per_page=100&_fields=id,slug,title,
// trimmed to the SmartSDR records. The changelogs and the FlexLib record are
// kept: they share the "SmartSDR v" prefix and must not be taken for releases.
const QByteArray kIndex = QByteArrayLiteral(
    R"j([{"id":2260539,"slug":"smartsdr-v4-x-api-flexlib","title":{"rendered":"SmartSDR v4.x API (FlexLib)"}},)j"
    R"({"id":2264915,"slug":"smartsdr-v4-3-19","title":{"rendered":"SmartSDR v4.3.19"}},)"
    R"({"id":2264707,"slug":"smartsdr-v4-3-16","title":{"rendered":"SmartSDR v4.3.16"}},)"
    R"({"id":2260415,"slug":"smartsdr_v4_changelog","title":{"rendered":"SmartSDR v4 Changelog"}},)"
    R"({"id":2263456,"slug":"smartsdr-v4-2-20","title":{"rendered":"SmartSDR v4.2.20"}},)"
    R"({"id":2262266,"slug":"smartsdr-v4-2-18","title":{"rendered":"SmartSDR v4.2.18"}},)"
    R"({"id":2261273,"slug":"smartsdr-v4-1-5","title":{"rendered":"SmartSDR v4.1.5"}},)"
    R"({"id":2260220,"slug":"smartsdr-v3-10-15","title":{"rendered":"SmartSDR v3.10.15"}},)"
    R"({"id":10921,"slug":"smartsdr_v2_changelog","title":{"rendered":"SmartSDR v2.5.1+ Changelog"}},)"
    R"({"id":2238579,"slug":"smartsdr-v2-10-1","title":{"rendered":"SmartSDR v2.10.1"}}])");

// wp-json/wp/v2/media?parent=2264915&per_page=100&_fields=source_url (v4.3.19)
const QByteArray kMedia4319 = QByteArrayLiteral(
    R"([{"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20261116144029\/FlexLib_API_v4.3.19.zip"},)"
    R"({"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20261116143920\/SmartSDR-v4.3.19-arm64-Installer-MD5-Hash-File.txt"},)"
    R"({"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20261116143849\/SmartSDR-v4.3.19-x64-Installer-MD5-Hash-File.txt"}])");

// ...?parent=2262266 (v4.2.18): FlexRadio spelled this release's arm64 tag in capitals.
const QByteArray kMedia4218 = QByteArrayLiteral(
    R"([{"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20260428081114\/FlexLib_API_v4.2.18.zip"},)"
    R"({"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20260428081044\/SmartSDR-v4.2.18-ARM64-Installer-MD5-Hash-File.txt"},)"
    R"({"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20260428081011\/SmartSDR-v4.2.18-x64-Installer-MD5-Hash-File.txt"}])");

// ...?parent=2261273 (v4.1.5): the pre-MSI .exe era, with no architecture tag.
// This is the one file the old hardcoded URL could reach.
const QByteArray kMedia415 = QByteArrayLiteral(
    R"([{"source_url":"https:\/\/edge.flexradio.com\/www\/offload\/20251215133656\/SmartSDR-v4.1.5-Installer-MD5-Hash-File.txt"}])");

// The v4.3.19 x64 hash file, byte for byte (CRLF line ends, tab-indented).
const QByteArray kHash4319 = QByteArrayLiteral(
    "[001/001] SmartSDR_v4.3.19_x64.msi\r\n"
    "\tSize:\t531,615,744\r\n"
    "\tCRC-32:\t8531D11F\r\n"
    "\tMD-5:\tbf6e31f8b4f0b657cbbdfb949427d674\r\n"
    "------------------------ FILE SIGNATURE COMPLETE ------------------------");

const QString kUrl4319x64 = QStringLiteral(
    "https://edge.flexradio.com/www/offload/20261116143849/SmartSDR-v4.3.19-x64-Installer-MD5-Hash-File.txt");
const QString kUrl4319arm = QStringLiteral(
    "https://edge.flexradio.com/www/offload/20261116143920/SmartSDR-v4.3.19-arm64-Installer-MD5-Hash-File.txt");

QByteArray media(const QString& sourceUrl)
{
    return QStringLiteral(R"([{"source_url":"%1"}])").arg(sourceUrl).toUtf8();
}

// ─── Step 2a: which software record is this release? ───────────────────────

void checkTheReleaseRecordIsFoundExactly()
{
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.3.19")) == 2264915,
          "4.3.19's record is found by its title");
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.1.5")) == 2261273,
          "a pre-MSI release's record is found too");
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.3.1")) == 0,
          "4.3.1 does not match 4.3.19's record");
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.3.19x")) == 0,
          "a version with trailing junk is refused, not parsed down to 4.3.19");
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.3.19.42208")) == 0,
          "a four-part radio version is not a published release");
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("2.5.1")) == 0,
          "a changelog's version is not a release");
    check(FirmwareStager::releasePostId(QByteArrayLiteral("{not json"), QStringLiteral("4.3.19")) == 0,
          "a malformed index names no record");

    // The id is untrusted input: it must be a positive whole JSON number.
    check(FirmwareStager::releasePostId(
              R"([{"id":"2264915","slug":"smartsdr-v4-3-19","title":{"rendered":"SmartSDR v4.3.19"}}])",
              QStringLiteral("4.3.19")) == 0,
          "an id sent as a string is refused");
    check(FirmwareStager::releasePostId(
              R"([{"id":-5,"slug":"smartsdr-v4-3-19","title":{"rendered":"SmartSDR v4.3.19"}}])",
              QStringLiteral("4.3.19")) == 0,
          "a negative id is refused");
    check(FirmwareStager::releasePostId(
              R"([{"id":12.5,"slug":"smartsdr-v4-3-19","title":{"rendered":"SmartSDR v4.3.19"}}])",
              QStringLiteral("4.3.19")) == 0,
          "a fractional id is refused");
    check(FirmwareStager::releasePostId(
              R"([{"slug":"smartsdr-v4-3-19","title":{"rendered":"SmartSDR v4.3.19"}}])",
              QStringLiteral("4.3.19")) == 0,
          "a record with no id names none");
}

// ─── Step 2b: which attachment is this installer's hash file? ──────────────

void checkTheHashFileIsSelectedByReleaseAndArchitecture()
{
    const QString x64 = QStringLiteral("x64");
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.19"), x64) == kUrl4319x64,
          "4.3.19's x64 hash file is selected, not the arm64 one or the FlexLib zip");
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.19"),
                                           QStringLiteral("arm64")) == kUrl4319arm,
          "asking for arm64 selects the arm64 hash file");
    check(FirmwareStager::selectMd5HashUrl(kMedia4218, QStringLiteral("4.2.18"),
                                           QStringLiteral("arm64")).endsWith(
              QStringLiteral("SmartSDR-v4.2.18-ARM64-Installer-MD5-Hash-File.txt")),
          "4.2.18's capitalised ARM64 tag still matches");
    check(FirmwareStager::selectMd5HashUrl(kMedia4218, QStringLiteral("4.2.18"), x64).endsWith(
              QStringLiteral("/SmartSDR-v4.2.18-x64-Installer-MD5-Hash-File.txt")),
          "4.2.18's x64 hash file is selected");
    check(FirmwareStager::selectMd5HashUrl(kMedia415, QStringLiteral("4.1.5"), QString()).endsWith(
              QStringLiteral("/20251215133656/SmartSDR-v4.1.5-Installer-MD5-Hash-File.txt")),
          "a pre-MSI release's untagged hash file is selected with no architecture");

    // Never another release's or another architecture's file.
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.1"), x64).isEmpty(),
          "4.3.1 does not take 4.3.19's hash file");
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.190"), x64).isEmpty(),
          "4.3.190 does not take 4.3.19's hash file");
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.16"), x64).isEmpty(),
          "a release absent from the attachments has no hash file");
    check(FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.19"), QString()).isEmpty(),
          "an MSI release's tagged file does not satisfy an untagged lookup");
    check(FirmwareStager::selectMd5HashUrl(kMedia415, QStringLiteral("4.1.5"), x64).isEmpty(),
          "an untagged file does not satisfy an x64 lookup");
    check(FirmwareStager::selectMd5HashUrl(
              media(QStringLiteral("https://edge.flexradio.com/a/SmartSDR-v4x3x1-x64-Installer-MD5-Hash-File.txt")),
              QStringLiteral("4.3.1"), x64).isEmpty(),
          "the version's dots are literal, not regex wildcards");

    // Only FlexRadio's own hosts, over https, may vouch for the installer.
    const QString name = QStringLiteral("SmartSDR-v4.3.19-x64-Installer-MD5-Hash-File.txt");
    check(FirmwareStager::selectMd5HashUrl(media(QStringLiteral("https://evil.example/") + name),
                                           QStringLiteral("4.3.19"), x64).isEmpty(),
          "a hash file on a foreign host is refused");
    check(FirmwareStager::selectMd5HashUrl(media(QStringLiteral("https://evilflexradio.com/") + name),
                                           QStringLiteral("4.3.19"), x64).isEmpty(),
          "a host that merely ends in 'flexradio.com' is refused");
    check(FirmwareStager::selectMd5HashUrl(
              media(QStringLiteral("https://flexradio.com.evil.example/") + name),
              QStringLiteral("4.3.19"), x64).isEmpty(),
          "a host that merely starts with 'flexradio.com' is refused");
    check(FirmwareStager::selectMd5HashUrl(media(QStringLiteral("http://edge.flexradio.com/") + name),
                                           QStringLiteral("4.3.19"), x64).isEmpty(),
          "a plain-http hash file is refused");
    check(FirmwareStager::selectMd5HashUrl(media(QStringLiteral("https://flexradio.com/") + name),
                                           QStringLiteral("4.3.19"), x64)
              == QStringLiteral("https://flexradio.com/") + name,
          "the bare flexradio.com host is accepted");
    check(FirmwareStager::selectMd5HashUrl(
              media(QStringLiteral("https://evil.example@edge.flexradio.com/") + name),
              QStringLiteral("4.3.19"), x64).isEmpty(),
          "a hash-file URL carrying userinfo is refused");
    check(FirmwareStager::selectMd5HashUrl(
              media(QStringLiteral("https://edge.flexradio.com/") + name + QStringLiteral("?v=1")),
              QStringLiteral("4.3.19"), x64).isEmpty(),
          "a hash-file URL carrying a query is refused");
    check(FirmwareStager::selectMd5HashUrl(
              media(QStringLiteral("https://edge.flexradio.com/") + name + QStringLiteral("#x")),
              QStringLiteral("4.3.19"), x64).isEmpty(),
          "a hash-file URL carrying a fragment is refused");

    // Ambiguity is no answer.
    const QByteArray twoDifferent = QStringLiteral(R"([{"source_url":"https://edge.flexradio.com/a/%1"},)"
                                                   R"({"source_url":"https://edge.flexradio.com/b/%1"}])")
                                        .arg(name).toUtf8();
    check(FirmwareStager::selectMd5HashUrl(twoDifferent, QStringLiteral("4.3.19"), x64).isEmpty(),
          "two different files claiming one release are both distrusted");
    const QByteArray sameTwice = QStringLiteral(R"([{"source_url":"https://edge.flexradio.com/a/%1"},)"
                                                R"({"source_url":"https://edge.flexradio.com/a/%1"}])")
                                     .arg(name).toUtf8();
    check(!FirmwareStager::selectMd5HashUrl(sameTwice, QStringLiteral("4.3.19"), x64).isEmpty(),
          "the same file listed twice is still one answer");

    check(FirmwareStager::selectMd5HashUrl(QByteArrayLiteral("[{"), QStringLiteral("4.3.19"), x64).isEmpty(),
          "malformed media JSON selects nothing");
    check(FirmwareStager::selectMd5HashUrl(QByteArrayLiteral(R"([{"source_url":42}])"),
                                           QStringLiteral("4.3.19"), x64).isEmpty(),
          "a non-string source_url is skipped");
}

// ─── Step 2c: what MD5 does it record for our installer? ───────────────────

void checkTheHashFileIsReadForTheInstallerItNames()
{
    check(FirmwareStager::parseMd5HashFile(kHash4319, QStringLiteral("SmartSDR_v4.3.19_x64.msi"))
              == QStringLiteral("bf6e31f8b4f0b657cbbdfb949427d674"),
          "the real 4.3.19 hash file yields the installer's MD5");
    check(FirmwareStager::parseMd5HashFile(kHash4319, QStringLiteral("SmartSDR_v4.3.19_arm64.msi")).isEmpty(),
          "a hash file for another installer yields nothing");
    check(FirmwareStager::parseMd5HashFile(kHash4319, QString()).isEmpty(),
          "no installer name, no MD5");

    QByteArray upper = kHash4319;
    upper.replace("bf6e31f8b4f0b657cbbdfb949427d674", "BF6E31F8B4F0B657CBBDFB949427D674");
    check(FirmwareStager::parseMd5HashFile(upper, QStringLiteral("SmartSDR_v4.3.19_x64.msi"))
              == QStringLiteral("bf6e31f8b4f0b657cbbdfb949427d674"),
          "an upper-case MD5 is normalised to lower case");

    const QByteArray lf = QByteArray(kHash4319).replace("\r\n", "\n");
    check(FirmwareStager::parseMd5HashFile(lf, QStringLiteral("SmartSDR_v4.3.19_x64.msi"))
              == QStringLiteral("bf6e31f8b4f0b657cbbdfb949427d674"),
          "LF line ends parse too");

    const QByteArray two = QByteArrayLiteral(
        "[001/002] SmartSDR_v4.3.19_arm64.msi\r\n"
        "\tMD-5:\t11111111111111111111111111111111\r\n"
        "[002/002] SmartSDR_v4.3.19_x64.msi\r\n"
        "\tMD-5:\t22222222222222222222222222222222\r\n");
    check(FirmwareStager::parseMd5HashFile(two, QStringLiteral("SmartSDR_v4.3.19_x64.msi"))
              == QStringLiteral("22222222222222222222222222222222"),
          "in a multi-file hash file, the MD5 is the named installer's");
    check(FirmwareStager::parseMd5HashFile(two, QStringLiteral("SmartSDR_v4.3.19_arm64.msi"))
              == QStringLiteral("11111111111111111111111111111111"),
          "and the other entry's MD5 is not borrowed");

    const QByteArray noMd5 = QByteArrayLiteral(
        "[001/002] SmartSDR_v4.3.19_x64.msi\r\n"
        "\tSize:\t1\r\n"
        "[002/002] SmartSDR_v4.3.19_arm64.msi\r\n"
        "\tMD-5:\t11111111111111111111111111111111\r\n");
    check(FirmwareStager::parseMd5HashFile(noMd5, QStringLiteral("SmartSDR_v4.3.19_x64.msi")).isEmpty(),
          "an entry without an MD5 does not borrow the next entry's");
    check(FirmwareStager::parseMd5HashFile(QByteArrayLiteral("MD-5:\tbf6e31f8b4f0b657cbbdfb949427d674"),
                                           QStringLiteral("SmartSDR_v4.3.19_x64.msi")).isEmpty(),
          "an MD5 not attributed to any file is not trusted");
}

// ─── The installer, the cache and the hash agree on one architecture ───────

void checkTheInstallerAndItsHashNameTheSameFile()
{
    check(FirmwareStager::installerFileName(QStringLiteral("4.3.19"))
              == QStringLiteral("SmartSDR_v4.3.19_x64.msi"),
          "an MSI release downloads the x64 MSI");
    check(FirmwareStager::installerFileName(QStringLiteral("4.1.5"))
              == QStringLiteral("SmartSDR_v4.1.5_Installer.exe"),
          "a pre-4.2 release downloads the .exe");

    // End to end over the saved responses: what the updater downloads for
    // 4.3.19 is exactly the file FlexRadio's selected hash file vouches for.
    check(FirmwareStager::releasePostId(kIndex, QStringLiteral("4.3.19")) == 2264915
              && FirmwareStager::selectMd5HashUrl(kMedia4319, QStringLiteral("4.3.19"),
                                                  QStringLiteral("x64")) == kUrl4319x64
              && FirmwareStager::parseMd5HashFile(
                     kHash4319, FirmwareStager::installerFileName(QStringLiteral("4.3.19")))
                     == QStringLiteral("bf6e31f8b4f0b657cbbdfb949427d674"),
          "index -> media -> hash file resolves 4.3.19's installer MD5");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    checkTheReleaseRecordIsFoundExactly();
    checkTheHashFileIsSelectedByReleaseAndArchitecture();
    checkTheHashFileIsReadForTheInstallerItNames();
    checkTheInstallerAndItsHashNameTheSameFile();
    if (g_failures == 0)
        std::printf("firmware_md5_url_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
