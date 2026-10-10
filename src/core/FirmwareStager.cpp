#include "FirmwareStager.h"
#include "CabExtractor.h"
#include "FirmwareCurrency.h"   // compareReleases() — one definition of "newer"
#include "LogManager.h"
#include "OleCompoundFile.h"
#include "models/ModelCapabilities.h"   // RadioPlatform for firmware-family routing

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QStandardPaths>
#include <QUrl>

namespace AetherSDR {

namespace {
// Stall timeout for the software-page, MD5 and installer requests (#4688 §6).
// 30 s rather than 15: the installer is a large download and this clock resets
// on every byte, so it only fires on a genuinely dead transfer.
constexpr int kTransferTimeoutMs = 30000;
} // namespace

FirmwareStager::FirmwareStager(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTransferTimeoutMs);
}

QString FirmwareStager::stagingDir()
{
    // GenericConfigLocation + "/AetherSDR" matches the path convention
    // AppSettings + the log dir already use, avoiding the double-nested
    // ~/.config/AetherSDR/AetherSDR/ path that AppConfigLocation produces.
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::GenericConfigLocation) + "/AetherSDR/firmware";
    QDir().mkpath(dir);
    return dir;
}

QString FirmwareStager::modelToFamily(const QString& model)
{
    // Firmware family follows the FlexLib RadioPlatform (Principle I), NOT a
    // model-string substring:
    //   Microburst / DeepEddy / BigBend  -> FLEX-6x00 firmware (all consumer
    //     radios, including 8400/8600 and the AU-/ML-/MLS-/CL-/CLS- series)
    //   DragonFire (RT-2122, government)  -> FLEX-9600 firmware
    //
    // The old contains("9600") test misrouted both directions: ML-9600 /
    // ML-9600W (BigBend) were sent to the government 9600 family, while RT-2122
    // (the actual DragonFire radio) fell through to the 6x00 family.
    if (capabilitiesFor(model).platform == RadioPlatform::DragonFire)
        return "9600";
    return "6x00";
}

bool FirmwareStager::versionUsesMsi(const QString& version)
{
    // FlexRadio switched from InnoSetup (.exe) to WiX MSI in SmartSDR v4.2.
    const auto parts = version.split('.');
    if (parts.size() < 2) return false;
    bool ok1 = false, ok2 = false;
    const int major = parts[0].toInt(&ok1);
    const int minor = parts[1].toInt(&ok2);
    if (!ok1 || !ok2) return false;
    return (major > 4) || (major == 4 && minor >= 2);
}

// ─── Step 0: What does FlexRadio publish? ───────────────────────────────────

// The software page lists every SmartSDR release it has ever carried, so this
// picks the highest rather than the first. NUMERICALLY: as text, "4.2.5" sorts
// after "4.2.20" and "3.9.19" after "3.10.15", so a string maximum silently
// picks an older release whenever a one-digit component meets a two-digit one.
// The page happens not to contain such a pair today, which is exactly the kind
// of luck an indicator should not be built on.
//
// Only major.minor.patch is published anywhere on the site; the fourth
// component of a radio's reported version (the build, "4.2.20.41343") appears
// only inside a downloaded installer, so nothing here can produce one.
QStringList FirmwareStager::parseAllReleases(const QByteArray& json)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    // Untrusted input, validated at the boundary (Principle VII). Everything
    // below asks what a value IS before using it: the top level must be an
    // array, each record an object, each title or slug a string. A record that
    // is not what it claims is skipped, never coerced.
    if (parseError.error != QJsonParseError::NoError || !doc.isArray())
        return {};

    // The release title, e.g. "SmartSDR v4.2.20". ANCHORED, because the index
    // also carries "SmartSDR v4 Changelog", "SmartSDR v2.5.1+ Changelog" and
    // "SmartSDR v4.x API (FlexLib)" -- an unanchored search would read the
    // 2.5.1 out of a changelog's name and publish it as a release.
    static const QRegularExpression titleRe(
        QStringLiteral(R"(^SmartSDR v(\d+\.\d+\.\d+)$)"),
        QRegularExpression::CaseInsensitiveOption);
    // The same release as a slug, e.g. "smartsdr-v4-2-20". Read only when the
    // title does not match, so an edited title ("SmartSDR v4.2.20 (Windows)")
    // costs us a release only if BOTH spellings have drifted. The changelogs
    // spell their slugs with underscores, so they do not collide here.
    static const QRegularExpression slugRe(
        QStringLiteral(R"(^smartsdr-v(\d+)-(\d+)-(\d+)$)"),
        QRegularExpression::CaseInsensitiveOption);

    QList<QVersionNumber> found;
    const QJsonArray records = doc.array();
    for (const QJsonValue& value : records) {
        if (!value.isObject())
            continue;
        const QJsonObject record = value.toObject();

        QString text;
        const QRegularExpressionMatch byTitle = titleRe.match(
            record.value(QLatin1String("title"))
                .toObject()
                .value(QLatin1String("rendered"))
                .toString());
        if (byTitle.hasMatch()) {
            text = byTitle.captured(1);
        } else {
            const QRegularExpressionMatch bySlug =
                slugRe.match(record.value(QLatin1String("slug")).toString());
            if (!bySlug.hasMatch())
                continue;
            text = QStringLiteral("%1.%2.%3").arg(bySlug.captured(1),
                                                  bySlug.captured(2),
                                                  bySlug.captured(3));
        }

        const QVersionNumber v = QVersionNumber::fromString(text);
        // An int-overflowing component does not parse to nothing: fromString()
        // stops at it and returns the PREFIX before it, so "99.2147483648.0"
        // becomes QVersionNumber(99) -- not null, and it outranks 4.2.20. The
        // patterns guarantee three components, so anything shorter is an
        // overflow and must be discarded, not merely a null (Principle VII).
        if (v.isNull() || v.segmentCount() != 3)
            continue;

        if (!found.contains(v))
            found.append(v);
    }

    // NEWEST FIRST, numerically. Every caller depends on this order:
    // parsePublishedReleases() takes the first entry it sees per major, and
    // Radio Setup's list shows the newest release at the top.
    std::sort(found.begin(), found.end(),
              [](const QVersionNumber& a, const QVersionNumber& b) {
                  return QVersionNumber::compare(a, b) > 0;
              });

    QStringList releases;
    releases.reserve(found.size());
    for (const QVersionNumber& v : std::as_const(found))
        releases.append(v.toString());
    return releases;
}

// Derived from parseAllReleases() rather than parsed again, so there is exactly
// one place that decides what counts as a release.
//
// GROUPED BY MAJOR, not reduced to one maximum. FlexRadio offers several lines
// side by side -- today 2.10.1, 3.10.15 and 4.1.5/4.2.18/4.2.20 -- and a radio
// must be judged against the newest release of the line it is actually running.
// Taking a single maximum tells a v3 radio it is behind 4.2.20 and links
// release notes for a line it is not on.
QMap<int, QString> FirmwareStager::parsePublishedReleases(const QByteArray& json)
{
    QMap<int, QString> newestByMajor;
    const QStringList all = parseAllReleases(json);
    for (const QString& release : all) {
        // The list is newest-first, so the first entry for a major IS its
        // newest and later ones must not displace it.
        const int major = QVersionNumber::fromString(release).majorVersion();
        if (!newestByMajor.contains(major))
            newestByMajor.insert(major, release);
    }
    return newestByMajor;
}

void FirmwareStager::requestPublishedReleases(
    std::function<void(const QMap<int, QString>&, const QStringList&,
                       const QString&)> done)
{
    auto* reply = m_nam.get(QNetworkRequest(QUrl(SOFTWARE_INDEX_URL)));
    connect(reply, &QNetworkReply::finished, this,
            // `this` is the connect context (lifetime), not a capture: every
            // member this lambda touches is static.
            [reply, done = std::move(done)]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, {}, "Cannot reach FlexRadio: " + reply->errorString());
            return;
        }

        // Untrusted input, validated at the boundary (Principle VII): this is a
        // public endpoint over which we have no control, so an implausibly
        // large answer is refused before it is parsed at all.
        if (reply->bytesAvailable() > kMaxSoftwareIndexBytes) {
            done({}, {}, "FlexRadio's software index is implausibly large; ignoring it.");
            return;
        }

        const QByteArray body = reply->readAll();
        const QStringList all = parseAllReleases(body);
        const QMap<int, QString> releases = parsePublishedReleases(body);
        if (releases.isEmpty()) {
            done({}, {}, "Could not determine any SmartSDR release from FlexRadio's software index.");
            return;
        }
        done(releases, all, {});
    });
}

void FirmwareStager::fetchLatestVersion()
{
    requestPublishedReleases([this](const QMap<int, QString>& releases,
                                    const QStringList&, const QString& error) {
        if (releases.isEmpty())
            emit latestVersionUnavailable(error);
        else
            emit publishedReleasesKnown(releases);
    });
}

// ─── Step 1: Check for update ────────────────────────────────────────────────

void FirmwareStager::checkForUpdate(const QString& currentVersion)
{
    emit stageProgress(0, "Checking for updates...");

    requestPublishedReleases([this, currentVersion](const QMap<int, QString>& releases,
                                                    const QStringList& all,
                                                    const QString& error) {
        if (releases.isEmpty()) {
            emit updateCheckFailed(error);
            return;
        }
        // Radio Setup's button asks a DIFFERENT question from the status bar:
        // "is there a newer SmartSDR I could stage and upload?", which is not
        // line-scoped. So it keeps using the highest release across all lines.
        const QString latest = std::max_element(
            releases.constBegin(), releases.constEnd(),
            [](const QString& a, const QString& b) {
                return QVersionNumber::compare(QVersionNumber::fromString(a),
                                               QVersionNumber::fromString(b)) < 0;
            }).value();

        // Compare major.minor.patch only: the build number is not published, so
        // the radio's fourth component has nothing on the other side to meet.
        const bool updateAvailable =
            FirmwareCurrency::compareReleases(latest, currentVersion) > 0;
        // The full list goes with the verdict: Radio Setup shows every release
        // FlexRadio offers, not just the newest, so an operator can see which
        // line they are on and what else exists.
        emit updateCheckComplete(latest, updateAvailable, all);
    });
}

// ─── Step 2a: Stage from a user-selected local file ─────────────────────────

void FirmwareStager::stageFromLocalFile(const QString& installerPath,
                                         const QString& modelFamily)
{
    m_modelFamily   = modelFamily;
    m_cancelled     = false;
    m_stagedPath.clear();
    m_stagedVersion.clear();
    m_installerPath = installerPath;
    m_expectedMd5.clear();   // user's responsibility — we don't have an authoritative source

    QFileInfo fi(installerPath);
    if (!fi.exists() || !fi.isReadable()) {
        emit stageFailed("Cannot read installer: " + installerPath);
        return;
    }

    // Parse version from filename so the staged .ssdr ends up named correctly.
    // Patterns we support:
    //   SmartSDR_v4.2.18_x64.msi
    //   SmartSDR_v4.1.5_Installer.exe
    //   FLEX-6x00_v4.2.18.41174.ssdr      (already-extracted firmware)
    QRegularExpression verRe(R"(v(\d+\.\d+\.\d+(?:\.\d+)?))",
                              QRegularExpression::CaseInsensitiveOption);
    auto m = verRe.match(fi.fileName());
    m_targetVersion = m.hasMatch() ? m.captured(1) : QStringLiteral("unknown");

    // If the user picked a pre-extracted .ssdr, no extraction needed —
    // copy it into the staging dir under our canonical name and we're done.
    if (fi.suffix().compare("ssdr", Qt::CaseInsensitive) == 0) {
        emit stageProgress(20, "Validating .ssdr file...");
        QFile probe(installerPath);
        if (!probe.open(QIODevice::ReadOnly)) {
            emit stageFailed("Cannot open .ssdr: " + probe.errorString());
            return;
        }
        const QByteArray hdr = probe.read(8);
        probe.close();
        if (hdr != QByteArrayLiteral("Salted__")) {
            emit stageFailed("Selected .ssdr file has invalid header.");
            return;
        }

        emit stageProgress(60, "Staging firmware...");
        const QString outName = "FLEX-" + m_modelFamily + "_v" + m_targetVersion + ".ssdr";
        const QString outPath = stagingDir() + "/" + outName;
        QFile::remove(outPath);
        if (!QFile::copy(installerPath, outPath)) {
            emit stageFailed("Failed to copy .ssdr into staging directory.");
            return;
        }

        QFile h(outPath);
        if (!h.open(QIODevice::ReadOnly)) {
            emit stageFailed("Cannot read staged firmware for hashing.");
            return;
        }
        QCryptographicHash fwHash(QCryptographicHash::Md5);
        fwHash.addData(&h);
        const QString fwMd5 = fwHash.result().toHex().toLower();
        const qint64 fwSize = h.size();
        h.close();

        m_stagedPath = outPath;
        m_stagedVersion = m_targetVersion;
        emit stageProgress(100, QString("Firmware staged and ready ✓\n"
            "%1 (%2 MB)\nMD5: %3").arg(outName).arg(fwSize / (1024*1024)).arg(fwMd5));
        emit stageComplete(outPath, m_targetVersion);
        return;
    }

    // Otherwise it's an installer (.msi or .exe) — go straight to extraction.
    // verifyAndExtract() handles format detection from the file header.
    verifyAndExtract();
}

// ─── Step 2: Download installer ──────────────────────────────────────────────

void FirmwareStager::downloadAndStage(const QString& version, const QString& modelFamily)
{
    m_targetVersion = version;
    m_modelFamily = modelFamily;
    m_cancelled = false;
    m_stagedPath.clear();
    m_stagedVersion.clear();

    // First, fetch the MD5 hash file
    emit stageProgress(0, "Fetching integrity hash...");

    const QString md5Url = QString(MD5_URL_FMT).arg(version);
    auto* md5Reply = m_nam.get(QNetworkRequest(QUrl(md5Url)));
    connect(md5Reply, &QNetworkReply::finished, this, [this, md5Reply, version]() {
        md5Reply->deleteLater();
        if (m_cancelled) return;

        if (md5Reply->error() == QNetworkReply::NoError) {
            // Parse MD5 from the hash file
            // Format: "MD-5:	17b7880f646dbaec9e387c2db35a997c"
            const QString body = QString::fromUtf8(md5Reply->readAll());
            QRegularExpression re(R"(MD-5:\s*([0-9a-fA-F]{32}))");
            auto m = re.match(body);
            if (m.hasMatch()) {
                m_expectedMd5 = m.captured(1).toLower();
                qCDebug(lcFirmware) << "FirmwareStager: expected installer MD5:" << m_expectedMd5;
            } else {
                qCWarning(lcFirmware) << "FirmwareStager: could not parse MD5 from hash file";
            }
        } else {
            qCWarning(lcFirmware) << "FirmwareStager: MD5 hash file not available (non-fatal)";
        }

        // Now download the installer. v4.2+ ships as a WiX MSI; older
        // releases used an InnoSetup .exe.
        const bool useMsi = versionUsesMsi(version);
        const QString installerUrl = useMsi
            ? QString(INSTALLER_URL_FMT_MSI).arg(version)
            : QString(INSTALLER_URL_FMT_EXE).arg(version);
        m_installerPath = stagingDir()
            + (useMsi ? "/SmartSDR_v" + version + "_x64.msi"
                      : "/SmartSDR_v" + version + "_Installer.exe");

        // Check if we already have it cached
        if (QFileInfo::exists(m_installerPath) && !m_expectedMd5.isEmpty()) {
            emit stageProgress(50, "Verifying cached installer...");
            QFile f(m_installerPath);
            if (f.open(QIODevice::ReadOnly)) {
                QCryptographicHash hash(QCryptographicHash::Md5);
                hash.addData(&f);
                const QString md5 = hash.result().toHex().toLower();
                f.close();
                if (md5 == m_expectedMd5) {
                    emit stageProgress(50, "Using cached installer (MD5 verified) ✓");
                    verifyAndExtract();
                    return;
                }
                qCDebug(lcFirmware) << "FirmwareStager: cached installer MD5 mismatch, re-downloading";
            }
        }

        emit stageProgress(1, "Downloading SmartSDR installer...");

        QNetworkRequest req{QUrl{installerUrl}};
        m_downloadReply = m_nam.get(req);
        connect(m_downloadReply, &QNetworkReply::downloadProgress,
                this, &FirmwareStager::onInstallerDownloadProgress);
        connect(m_downloadReply, &QNetworkReply::finished,
                this, &FirmwareStager::onInstallerDownloadFinished);
    });
}

void FirmwareStager::cancel()
{
    m_cancelled = true;
    if (m_downloadReply) {
        m_downloadReply->abort();
        m_downloadReply = nullptr;
    }
    emit stageFailed("Cancelled.");
}

void FirmwareStager::onInstallerDownloadProgress(qint64 received, qint64 total)
{
    if (m_cancelled) return;
    if (total <= 0) {
        emit stageProgress(1, QString("Downloading... %1 MB").arg(received / (1024*1024)));
        return;
    }
    // Download is 0-50% of total progress
    const int pct = static_cast<int>(received * 50 / total);
    emit stageProgress(pct, QString("Downloading... %1 / %2 MB")
                           .arg(received / (1024*1024))
                           .arg(total / (1024*1024)));
}

void FirmwareStager::onInstallerDownloadFinished()
{
    auto* reply = m_downloadReply;
    m_downloadReply = nullptr;
    if (!reply) return;
    reply->deleteLater();

    if (m_cancelled) return;

    if (reply->error() != QNetworkReply::NoError) {
        emit stageFailed("Download failed: " + reply->errorString());
        return;
    }

    // Save to disk
    emit stageProgress(50, "Saving installer...");
    QFile f(m_installerPath);
    if (!f.open(QIODevice::WriteOnly)) {
        emit stageFailed("Cannot write installer: " + f.errorString());
        return;
    }
    f.write(reply->readAll());
    f.close();

    verifyAndExtract();
}

// ─── Steps 3-4: Verify and extract ──────────────────────────────────────────

void FirmwareStager::verifyAndExtract()
{
    if (m_cancelled) return;

    // Step 3: Verify MD5
    if (!m_expectedMd5.isEmpty()) {
        emit stageProgress(55, "Verifying installer integrity...");

        QFile f(m_installerPath);
        if (!f.open(QIODevice::ReadOnly)) {
            emit stageFailed("Cannot read installer for verification.");
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(&f);
        const QString md5 = hash.result().toHex().toLower();
        f.close();

        if (md5 != m_expectedMd5) {
            emit stageFailed(QString("Installer integrity check failed.\n"
                "Expected: %1\nGot: %2").arg(m_expectedMd5, md5));
            QFile::remove(m_installerPath);
            return;
        }
        emit stageProgress(65, "Installer integrity verified ✓");
    } else {
        emit stageProgress(65, "MD5 hash not available — skipping verification.");
    }

    // Step 4: Extract .ssdr firmware
    emit stageProgress(70, "Extracting firmware...");

    // Detect installer format from the first 8 bytes:
    //   InnoSetup .exe → "MZ" (PE/COFF) header
    //   WiX MSI        → OLE Compound File: D0 CF 11 E0 A1 B1 1A E1
    QFile probe(m_installerPath);
    if (!probe.open(QIODevice::ReadOnly)) {
        emit stageFailed("Cannot open installer for extraction.");
        return;
    }
    const QByteArray header = probe.read(8);
    probe.close();

    const QByteArray oleMagic("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    const bool isMsi = (header == oleMagic);

    // Compute output path early so both extractors can target the same place.
    const QString targetName = "FLEX-" + m_modelFamily + "_v" + m_targetVersion + ".ssdr";
    const QString outPath = stagingDir() + "/" + targetName;
    QFile::remove(outPath);

    bool ok = false;
    if (isMsi) {
        ok = extractFromMsi(m_installerPath, outPath);
    } else {
        QFile installer(m_installerPath);
        if (!installer.open(QIODevice::ReadOnly)) {
            emit stageFailed("Cannot open installer for extraction.");
            return;
        }
        const QByteArray data = installer.readAll();
        installer.close();
        ok = extractFromInnoSetup(data, outPath);
    }
    if (!ok) {
        // The format-specific extractor already emitted stageFailed.
        return;
    }

    // Common downstream: validate header, MD5, emit completion.
    emit stageProgress(92, "Validating extracted firmware...");
    QFile check(outPath);
    if (!check.open(QIODevice::ReadOnly)) {
        emit stageFailed("Cannot read extracted firmware for validation.");
        return;
    }
    const QByteArray fwHeader = check.read(8);
    check.close();

    if (fwHeader != QByteArrayLiteral("Salted__")) {
        emit stageFailed("Extracted firmware failed validation (bad header).");
        QFile::remove(outPath);
        return;
    }

    QFile hashFile(outPath);
    if (!hashFile.open(QIODevice::ReadOnly)) {
        qCWarning(lcFirmware) << "FirmwareStager: cannot open staged firmware for hashing:" << outPath;
        emit stageFailed("Cannot read staged firmware for verification.");
        return;
    }
    QCryptographicHash fwHash(QCryptographicHash::Md5);
    fwHash.addData(&hashFile);
    const QString fwMd5 = fwHash.result().toHex().toLower();
    const qint64 fwSize = hashFile.size();
    hashFile.close();

    m_stagedPath = outPath;
    m_stagedVersion = m_targetVersion;

    emit stageProgress(100, QString("Firmware staged and ready ✓\n"
        "%1 (%2 MB)\nMD5: %3")
        .arg(targetName)
        .arg(fwSize / (1024*1024))
        .arg(fwMd5));
    emit stageComplete(outPath, m_targetVersion);
}

// ─── Format-specific extractors ─────────────────────────────────────────────

bool FirmwareStager::extractFromInnoSetup(const QByteArray& data, const QString& outPath)
{
    // v4.1.x and earlier: InnoSetup self-extracting .exe with two .ssdr blobs
    // stored as OpenSSL-encrypted "Salted__"-prefixed payloads, separated by an
    // InnoSetup LZMA block marker. Boundaries are deduced by scanning.

    const QByteArray saltSig("Salted__");
    QList<qint64> saltOffsets;
    qint64 pos = 0;
    while (true) {
        qint64 idx = data.indexOf(saltSig, pos);
        if (idx < 0) break;
        saltOffsets.append(idx);
        pos = idx + 1;
    }

    if (saltOffsets.size() < 2) {
        emit stageFailed(QString("Expected 2 firmware files in installer, found %1.\n"
            "The installer format may have changed.")
            .arg(saltOffsets.size()));
        return false;
    }

    emit stageProgress(75, QString("Found %1 firmware files.").arg(saltOffsets.size()));

    const qint64 off1 = saltOffsets[0];
    const qint64 off2 = saltOffsets[1];
    const qint64 size1 = off2 - off1;

    // Find end of file 2: next InnoSetup LZMA block header at least 50 MB out.
    const QByteArray zlbSig("\x7a\x6c\x62\x1a\x5d\x00\x00\x80", 8);
    qint64 size2 = -1;
    pos = off2 + 1024;
    while (true) {
        qint64 idx = data.indexOf(zlbSig, pos);
        if (idx < 0) break;
        const qint64 candidate = idx - off2;
        if (candidate > 50 * 1024 * 1024) {
            size2 = candidate;
            break;
        }
        pos = idx + 1;
    }
    if (size2 < 0) {
        size2 = data.size() - off2;
        qCWarning(lcFirmware) << "FirmwareStager: could not find LZMA boundary, using"
                              << size2 << "bytes";
    }

    // File 1 is FLEX-6x00 (consumer multi-platform), File 2 is FLEX-9600.
    const qint64 offset = (m_modelFamily == "9600") ? off2  : off1;
    const qint64 size   = (m_modelFamily == "9600") ? size2 : size1;

    emit stageProgress(85, QString("Extracting %1 MB...").arg(size / (1024*1024)));

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) {
        emit stageFailed("Cannot write firmware file: " + out.errorString());
        return false;
    }
    out.write(data.constData() + offset, size);
    out.close();
    return true;
}

bool FirmwareStager::extractFromMsi(const QString& msiPath, const QString& outPath)
{
    // v4.2+: WiX MSI (OLE Compound File) with `.ssdr` blobs hidden inside
    // LZX-compressed CAB streams (cab1.cab, cab2.cab, ...). Native pipeline:
    //
    //   OleCompoundFile → all cab*.cab streams (in memory)
    //   CabExtractor (libmspack) → all files inside each cab (in memory)
    //   Filter for Salted__ magic → list of firmware blobs
    //   Sort by size desc, pick by family, write to outPath
    //
    // No external tools, no temp files.

    OleCompoundFile cfb;
    if (!cfb.open(msiPath)) {
        emit stageFailed("Could not open MSI: " + cfb.lastError());
        return false;
    }

    emit stageProgress(72, "Reading installer streams...");
    const auto cabStreams = cfb.readMsiStreamsByPrefixSuffix("cab", ".cab");
    if (cabStreams.isEmpty()) {
        emit stageFailed("No CAB streams found in MSI.");
        return false;
    }

    emit stageProgress(78,
        QString("Decompressing %1 CAB streams...").arg(cabStreams.size()));

    struct Blob {
        QString cabName;
        QByteArray data;
    };
    QList<Blob> blobs;

    CabExtractor cx;
    int progressBase = 78;
    int progressSpan = 12;  // 78..90
    int idx = 0;
    for (const auto& [cabName, cabBytes] : cabStreams) {
        QByteArray ssdr;
        if (!cx.extractFirstMatchingMagic(cabBytes,
                                          QByteArrayLiteral("Salted__"),
                                          ssdr)) {
            // No firmware in this cab — that's expected for cabs that
            // hold drivers/installer support files. Move on.
            ++idx;
            const int pct = progressBase + (progressSpan * idx) / cabStreams.size();
            emit stageProgress(pct,
                QString("Scanning %1 (no firmware)...").arg(cabName));
            continue;
        }
        blobs.append({cabName, std::move(ssdr)});
        ++idx;
        const int pct = progressBase + (progressSpan * idx) / cabStreams.size();
        emit stageProgress(pct,
            QString("Found firmware in %1 (%2 MB)...")
                .arg(cabName).arg(blobs.last().data.size() / (1024*1024)));
    }

    if (blobs.isEmpty()) {
        emit stageFailed(
            "No firmware blobs (Salted__ header) found in any MSI cabinet.");
        return false;
    }

    // FLEX-6x00 consumer firmware bundles every consumer platform and is
    // significantly larger than the FLEX-9600 government build, so size
    // order gives us the family mapping reliably.
    std::sort(blobs.begin(), blobs.end(),
              [](const Blob& a, const Blob& b) {
                  return a.data.size() > b.data.size();
              });

    const Blob* target = nullptr;
    if (m_modelFamily == "9600") {
        if (blobs.size() < 2) {
            emit stageFailed("FLEX-9600 firmware not present in this installer.");
            return false;
        }
        target = &blobs.last();   // smallest = government 9600 build
    } else {
        target = &blobs.first();  // largest = multi-platform consumer
    }

    emit stageProgress(92,
        QString("Staging firmware (%1 MB)...").arg(target->data.size() / (1024*1024)));

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) {
        emit stageFailed("Cannot write firmware file: " + out.errorString());
        return false;
    }
    const qint64 written = out.write(target->data);
    out.close();
    if (written != target->data.size()) {
        emit stageFailed(
            QString("Short write: %1 of %2 bytes").arg(written).arg(target->data.size()));
        QFile::remove(outPath);
        return false;
    }
    return true;
}

} // namespace AetherSDR
