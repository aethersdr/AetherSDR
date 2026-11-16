#include "AetherPDPackage.h"
#include "AetherPDProtocol.h"
#include "ZipArchive.h"
#include "monocypher-ed25519.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>
#include <cstdint>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AetherSDR {
namespace {
QByteArray blake(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256); }
const uint8_t* raw(const QByteArray& bytes) { return reinterpret_cast<const uint8_t*>(bytes.constData()); }
quint32 number(const QByteArray& bytes, qsizetype offset) { return qFromBigEndian<quint32>(bytes.constData() + offset); }
bool elf(const QByteArray& bytes, quint32 arch)
{
    return bytes.size() >= (arch == 64 ? 64 : 52) && bytes.first(4) == QByteArray::fromHex("7f454c46")
        && bytes[4] == (arch == 64 ? 2 : 1) && bytes[5] == 1 && bytes[6] == 1
        && qFromLittleEndian<quint16>(bytes.constData() + 16) == 2
        && qFromLittleEndian<quint16>(bytes.constData() + 18) == (arch == 64 ? 183 : 40);
}
std::optional<QByteArray> snapshot(const QString& path, const AetherPDPackage::Pin& pin, qint64 maximum)
{
    if (pin.size <= 0 || pin.size > maximum || pin.sha256.size() != 32) { return std::nullopt; }
    QFile file(path);
#ifdef Q_OS_UNIX
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { return std::nullopt; }
    struct stat info{};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size != pin.size
        || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd); return std::nullopt;
    }
#else
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != pin.size || !file.open(QIODevice::ReadOnly)) { return std::nullopt; }
#endif
    const QByteArray bytes = file.read(pin.size + 1);
    if (bytes.size() != pin.size || file.error() != QFileDevice::NoError
        || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != pin.sha256) { return std::nullopt; }
    return bytes;
}
struct Release { quint32 arch, profile; QString firmware; QByteArray source; };
std::optional<Release> release(const QByteArray& bytes, const QByteArray& key)
{
    if (bytes.size() < 432 || bytes.size() > 18 * 1024 * 1024 || key.size() != 32
        || crypto_ed25519_check(raw(bytes) + 368, raw(key), raw(bytes), 368)) { return std::nullopt; }
    if (bytes.first(7) != "AEPDR00" || bytes[7] < '1' || bytes[7] > '5' || number(bytes, 12) != 8) { return std::nullopt; }
    Release result{number(bytes, 8), number(bytes, 48), {}, bytes.mid(16, 32)};
    if (!((result.arch == 64 && result.profile == 1) || (result.arch == 7 && result.profile == 2))) { return std::nullopt; }
    const QByteArray field = bytes.mid(52, 28);
    const qsizetype end = field.indexOf('\0');
    if (end <= 0 || field.mid(end) != QByteArray(28 - end, '\0')) { return std::nullopt; }
    result.firmware = QString::fromLatin1(field.first(end));
    static const QRegularExpression version(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){3}\\z"));
    if (!version.match(result.firmware).hasMatch()) { return std::nullopt; }
    qsizetype offset = 432;
    for (qsizetype i = 0; i < 8; ++i) {
        const quint32 count = number(bytes, 80 + i * 36);
        const quint32 maximum = i == 2 ? 16 * 1024 * 1024 : i >= 4 ? 16384 : 512 * 1024;
        if (!count || count > maximum || count > bytes.size() - offset) { return std::nullopt; }
        const QByteArray member = bytes.mid(offset, count);
        if (blake(member) != bytes.mid(84 + i * 36, 32) || (i < 2 && !elf(member, result.arch))) { return std::nullopt; }
        offset += count;
    }
    return offset == bytes.size() ? std::optional<Release>(result) : std::nullopt;
}
bool worker(const QByteArray& bytes, const QByteArray& key, const Release& target, const QByteArray& candidate, quint32 role = 2)
{
    return bytes.size() > 184 && bytes.size() <= 184 + 512 * 1024 && bytes.first(8) == "AEPDX001"
        && number(bytes, 8) == target.arch && number(bytes, 12) == role && number(bytes, 20) == 0
        && number(bytes, 16) == bytes.size() - 184 && bytes.mid(24, 32) == target.source
        && bytes.mid(56, 32) == blake(candidate) && bytes.mid(88, 32) == blake(bytes.mid(184))
        && elf(bytes.mid(184), target.arch) && !crypto_ed25519_check(raw(bytes) + 120, raw(key), raw(bytes), 120);
}
}
std::array<QString, 8> AetherPDPackage::fileNames()
{
    return {QStringLiteral("worker.envelope"), QStringLiteral("candidate.release"), QStringLiteral("previous.release"),
        QStringLiteral("pd-accept"), QStringLiteral("pd-carrier"), QStringLiteral("pd-send"), QStringLiteral("pd-deliver"), QStringLiteral("LICENSES")};
}
std::array<qint64, 8> AetherPDPackage::fileSizeLimits()
{
    return {184 + 512 * 1024, 18 * 1024 * 1024, 18 * 1024 * 1024,
        8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 512 * 1024};
}
bool AetherPDPackage::validTrust(const Trust& trust)
{
    if (trust.candidateKey.size() != 32 || trust.previousKey.size() != 32) { return false; }
    const auto limits = fileSizeLimits();
    qint64 codeBytes = 0;
    for (std::size_t i = 0; i < trust.files.size(); ++i) {
        const Pin& pin = trust.files[i];
        if (pin.size <= 0 || pin.size > limits[i] || pin.sha256.size() != 32) { return false; }
        if (i >= 3 && i < 7) { codeBytes += pin.size; }
    }
    return codeBytes <= 16 * 1024 * 1024;
}
std::optional<AetherPDPackage> AetherPDPackage::load(const QString& directory, const Trust& trust)
{
    if (!validTrust(trust)) { return std::nullopt; }
    const QDir root(directory);
    if (!root.exists()) { return std::nullopt; }
    const auto names = fileNames();
    const auto limits = fileSizeLimits();
    Files files;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto bytes = snapshot(root.filePath(names[i]), trust.files[i], limits[i]);
        if (!bytes) { return std::nullopt; }
        files[i] = *bytes;
    }
    return verify(files, trust);
}
std::optional<AetherPDPackage> AetherPDPackage::verify(const Files& files, const Trust& trust)
{
    if (!validTrust(trust)) { return std::nullopt; }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].size() != trust.files[i].size
            || QCryptographicHash::hash(files[i], QCryptographicHash::Sha256) != trust.files[i].sha256) { return std::nullopt; }
    }
    const auto candidate = release(files[1], trust.candidateKey), previous = release(files[2], trust.previousKey);
    if (!candidate || !previous || candidate->arch != previous->arch || candidate->profile != previous->profile
        || candidate->firmware != previous->firmware || files[1] == files[2]
        || !worker(files[0], trust.candidateKey, *candidate, files[1])) { return std::nullopt; }
    qsizetype codeBytes = 0;
    for (std::size_t i = 3; i < 7; ++i) {
        if (!elf(files[i], candidate->arch)) { return std::nullopt; }
        codeBytes += files[i].size();
    }
    if (codeBytes > 16 * 1024 * 1024) { return std::nullopt; }
    AetherPDPackage result;
    result.m_worker = files[0]; result.m_candidate = files[1]; result.m_previous = files[2];
    result.m_code = {files[3], files[4], files[5], files[6]}; result.m_licenses = files[7];
    result.m_profile = candidate->profile; result.m_firmware = candidate->firmware;
    result.m_candidateKey = trust.candidateKey; result.m_previousKey = trust.previousKey;
    return result;
}
QByteArray AetherPDPackage::plan(const QString& serial) const
{
    const QByteArray serialBytes = serial.toLatin1(), firmwareBytes = m_firmware.toLatin1();
    if (serialBytes.isEmpty() || serialBytes.size() >= 64 || firmwareBytes.isEmpty() || firmwareBytes.size() >= 28) { return {}; }
    const auto target = [&](const QByteArray& digest) {
        QByteArray bytes(140, '\0'); bytes.replace(0, 8, "PDAPRV02");
        qToBigEndian(m_profile, bytes.data() + 8);
        bytes.replace(16, 32, digest); bytes.replace(48, firmwareBytes.size(), firmwareBytes);
        bytes.replace(76, serialBytes.size(), serialBytes); return bytes;
    };
    const QByteArray result = QByteArray("PDUPGR01") + target(blake(m_previous)) + target(blake(m_candidate)) + m_previousKey + m_candidateKey;
    return aetherpd::parseInstallationPlan(result) ? result : QByteArray{};
}
std::array<QString, 8> AetherPDFirstPackage::fileNames()
{
    return {QStringLiteral("worker.envelope"), QStringLiteral("candidate.release"), QStringLiteral("pd-first-request"),
        QStringLiteral("pd-accept"), QStringLiteral("pd-carrier"), QStringLiteral("pd-send"), QStringLiteral("pd-deliver"), QStringLiteral("LICENSES")};
}
std::array<qint64, 8> AetherPDFirstPackage::fileSizeLimits()
{
    return {184 + 512 * 1024, 18 * 1024 * 1024, 512 * 1024,
        8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 512 * 1024};
}
bool AetherPDFirstPackage::validTrust(const Trust& trust)
{
    if (trust.publisherKey.size() != 32 || trust.publisherKey == QByteArray(32, '\0')) { return false; }
    const auto limits = fileSizeLimits();
    qint64 codeBytes = 0;
    for (std::size_t i = 0; i < trust.files.size(); ++i) {
        const auto& pin = trust.files[i];
        if (pin.size <= 0 || pin.size > limits[i] || pin.sha256.size() != 32) { return false; }
        if (i >= 3 && i < 7) { codeBytes += pin.size; }
    }
    return codeBytes <= 16 * 1024 * 1024;
}
std::optional<AetherPDFirstPackage> AetherPDFirstPackage::load(const QString& directory, const Trust& trust)
{
    if (!validTrust(trust)) { return std::nullopt; }
    const QDir root(directory);
    if (!root.exists()) { return std::nullopt; }
    const auto names = fileNames();
    const auto limits = fileSizeLimits();
    Files files;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto bytes = snapshot(root.filePath(names[i]), trust.files[i], limits[i]);
        if (!bytes) { return std::nullopt; }
        files[i] = *bytes;
    }
    return verify(files, trust);
}
std::optional<AetherPDFirstPackage> AetherPDFirstPackage::verify(const Files& files, const Trust& trust)
{
    if (!validTrust(trust)) { return std::nullopt; }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].size() != trust.files[i].size
            || QCryptographicHash::hash(files[i], QCryptographicHash::Sha256) != trust.files[i].sha256) { return std::nullopt; }
    }
    const auto candidate = release(files[1], trust.publisherKey);
    if (!candidate || candidate->arch != 64 || candidate->profile != 1
        || !worker(files[0], trust.publisherKey, *candidate, files[1], 1)) { return std::nullopt; }
    for (std::size_t i = 2; i < 7; ++i) {
        if (!elf(files[i], 64)) { return std::nullopt; }
    }
    AetherPDFirstPackage result;
    result.m_files = files; result.m_key = trust.publisherKey; result.m_firmware = candidate->firmware;
    return result;
}
std::optional<AetherPDFirstPackage::Uploads> AetherPDFirstPackage::prepare(const QString& serial,
    const QByteArray& tokenHash, const QString& hostname, const QByteArray& installationId) const
{
    if (tokenHash.size() != 32 || installationId.size() != 32 || serial.isEmpty() || serial.size() >= 64
        || hostname.isEmpty() || hostname.size() >= 64 || serial != QString::fromLatin1(serial.toLatin1())
        || hostname != QString::fromLatin1(hostname.toLatin1())) { return std::nullopt; }
    QByteArray target(140, '\0'); target.replace(0, 8, "PDAPRV02");
    qToBigEndian<quint32>(1, target.data() + 8);
    target.replace(16, 32, blake(m_files[1]));
    target.replace(48, m_firmware.size(), m_firmware.toLatin1()); target.replace(76, serial.size(), serial.toLatin1());
    const QByteArray plan = QByteArray("PDFST001") + target + tokenHash
        + hostname.toLatin1().leftJustified(64, '\0') + installationId + QByteArray(32, '\0');
    if (!aetherpd::parseFirstInstallationPlan(plan, m_key)) { return std::nullopt; }
    QByteArray receipt("PDFPUB01PDDELIV1"); receipt.resize(24);
    qToBigEndian<quint32>(64, receipt.data() + 16); qToBigEndian<quint32>(1, receipt.data() + 20);
    receipt += blake(m_files[0]) + blake(plan) + installationId;
    const auto carrier = aetherpd::buildCarrierImage({m_files[3], m_files[4], m_files[5], m_files[6]}, receipt, m_files[7]);
    if (!carrier) { return std::nullopt; }
    const QString name = QStringLiteral("AetherPDFirst") + QString::fromLatin1(installationId.first(8).toHex());
    const QByteArray cfg = "[header]\nName: " + name.toLatin1()
        + "\nVersion: \"0.1.0\"\nMinimum-SmartSDR-Version: 4.2.18.0\nAuthor: AetherSDR\nLicense: GPL-3.0\nExecutable: \"start.sh\"\n\n[setup]\n\n[end]\n";
    constexpr quint16 kDate = ((2026 - 1980) << 9) | (1 << 5) | 1;
    const QString prefix = name + u'/';
    const QList<ZipEntryData> entries{
        {prefix + name + QStringLiteral(".cfg"), cfg, 0644, kDate},
        {prefix + QStringLiteral("LICENSES"), m_files[7], 0444, kDate},
        {prefix + QStringLiteral("pd-first-request"), m_files[2], 0755, kDate},
        {prefix + QStringLiteral("plan"), plan, 0444, kDate},
        {prefix + QStringLiteral("release"), m_files[1], 0444, kDate},
        {prefix + QStringLiteral("request"), receipt, 0444, kDate},
        {prefix + QStringLiteral("start.sh"), "#!/bin/sh\nset -eu\nexec \"${0%/*}/pd-first-request\"\n", 0755, kDate},
        {prefix + QStringLiteral("worker"), m_files[0], 0444, kDate}};
    const QByteArray archive = writeDeflatedZip(entries);
    if (archive.isEmpty() || archive.size() > 24 * 1024 * 1024) { return std::nullopt; }
    return Uploads{plan, m_key, name, archive, *carrier};
}
} // namespace AetherSDR
