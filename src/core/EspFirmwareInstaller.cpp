#include "EspFirmwareInstaller.h"

#include <QCoreApplication>
#include <QCryptographicHash>

#include <utility>

namespace AetherSDR {

namespace {

constexpr quint32 kSector = EspFlashTarget::kSectorSize;
constexpr quint32 kOtadataSize = 2 * kSector;
constexpr quint32 kWriteChunk = 16 * kSector;   // compared, and rewritten, as a unit
constexpr quint32 kOtaStateInvalid = 3;
constexpr quint32 kOtaStateAborted = 4;
constexpr quint32 kOtaStateValid = 2;
constexpr quint8 kAppImageMagic = 0xE9;

QString tr(const char* text)
{
    return QCoreApplication::translate("EspFirmwareInstaller", text);
}

quint32 le32(const QByteArray& b, int at)
{
    const auto u = [&](int i) { return static_cast<quint32>(static_cast<quint8>(b[at + i])); };
    return u(0) | (u(1) << 8) | (u(2) << 16) | (u(3) << 24);
}

void putLe32(QByteArray* b, int at, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        (*b)[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// CRC-32 (reflected, 0xEDB88320) as ESP-IDF's esp_rom_crc32_le(UINT32_MAX, ...)
// computes it over an otadata entry's sequence number.
quint32 otaCrc(quint32 seq)
{
    quint32 c = 0;   // ~UINT32_MAX
    for (int i = 0; i < 4; ++i) {
        c ^= (seq >> (8 * i)) & 0xFF;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

quint64 sectorCeil(quint64 a)
{
    return (a + kSector - 1) / kSector * kSector;
}

} // namespace

EspFirmwareInstaller::EspFirmwareInstaller(EspFlashTarget* target, Report report)
    : m_target(target)
    , m_report(std::move(report))
{
}

bool EspFirmwareInstaller::step(const QString& name, qint64 done, qint64 total)
{
    return !m_report || m_report(name, done, total);
}

QList<EspFirmwareInstaller::OtaEntry> EspFirmwareInstaller::readOtadata(const QByteArray& otadata)
{
    QList<OtaEntry> entries;
    for (int i = 0; i < 2; ++i) {
        OtaEntry e;
        const int at = i * static_cast<int>(kSector);
        if (otadata.size() >= at + 32) {
            e.seq = le32(otadata, at);
            const quint32 state = le32(otadata, at + 24);
            e.valid = e.seq != 0xFFFFFFFFu && le32(otadata, at + 28) == otaCrc(e.seq)
                      && state != kOtaStateInvalid && state != kOtaStateAborted;
        }
        entries.append(e);
    }
    return entries;
}

QByteArray EspFirmwareInstaller::selectSlot(const QByteArray& otadata, int slot, int slotCount)
{
    const QList<OtaEntry> e = readOtadata(otadata);
    int current = -1;
    for (int i = 0; i < 2; ++i) {
        if (e[i].valid && (current < 0 || e[i].seq > e[current].seq))
            current = i;
    }
    quint32 seq = current >= 0 ? e[current].seq + 1 : 1;
    while (static_cast<int>((seq - 1) % static_cast<quint32>(slotCount)) != slot)
        ++seq;
    QByteArray out = otadata;
    out.resize(kOtadataSize, '\xFF');
    const int at = (current >= 0 ? current ^ 1 : 0) * static_cast<int>(kSector);
    out.replace(at, static_cast<int>(kSector), QByteArray(static_cast<int>(kSector), '\xFF'));
    putLe32(&out, at, seq);                   // 20 label bytes stay 0xFF
    putLe32(&out, at + 24, kOtaStateValid);
    putLe32(&out, at + 28, otaCrc(seq));
    return out;
}

int EspFirmwareInstaller::newestOf(const QByteArray& otadata, const QList<int>& candidates,
                                   int slotCount)
{
    int best = -1;
    quint32 bestSeq = 0;
    for (const OtaEntry& e : readOtadata(otadata)) {
        const int slot = static_cast<int>((e.seq - 1) % static_cast<quint32>(slotCount));
        if (e.valid && candidates.contains(slot) && (best < 0 || e.seq > bestSeq)) {
            best = slot;
            bestSeq = e.seq;
        }
    }
    return best;
}

bool EspFirmwareInstaller::check(const EspFirmwareManifest& manifest, QString* problem)
{
    m_protect = manifest.protect;
    if (!EspFirmwareManifest::checkPlan(manifest, problem))
        return false;   // a manifest built in code, never parsed
    if (m_target->chipName().compare(manifest.chip, Qt::CaseInsensitive) != 0) {
        *problem = tr("This firmware is for an %1; the device is an %2.")
                       .arg(manifest.chip, m_target->chipName());
        return false;
    }
    if (m_target->flashSize() != manifest.flashSize) {
        *problem = tr("This firmware is for a device with %1 MB of flash; this one has %2 MB.")
                       .arg(manifest.flashSize >> 20)
                       .arg(m_target->flashSize() >> 20);
        return false;
    }
    for (const EspFirmwareManifest::Region& r : manifest.require) {
        QByteArray bytes;
        if (!step(tr("Checking the device"), 0, r.size)) {
            *problem = tr("Cancelled.");
            return false;
        }
        if (!m_target->read(r.offset, r.size, &bytes, {}, problem))
            return false;
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != r.sha256) {
            *problem = tr("This device is not the one this firmware is for: its %1 is not as "
                          "expected. Nothing was written.")
                           .arg(r.name.isEmpty() ? tr("layout") : r.name);
            return false;
        }
    }
    return true;
}

bool EspFirmwareInstaller::backup(QByteArray* flash, QString* problem)
{
    const QString name = tr("Backing up the flash");
    return m_target->read(0, m_target->flashSize(), flash,
                          [&](qint64 done, qint64 total) { return step(name, done, total); },
                          problem);
}

bool EspFirmwareInstaller::writeRegion(quint32 offset, const QByteArray& data, const QString& name,
                                       QString* problem)
{
    // Belt and braces: the manifest was checked when parsed, but nothing
    // reaches the flash over a protected region, erased sectors included.
    const quint64 start = offset & ~(kSector - 1);
    const quint64 end = sectorCeil(quint64(offset) + data.size());
    for (const EspFirmwareManifest::Region& p : m_protect) {
        if (start < quint64(p.offset) + p.size && p.offset < end) {
            *problem = tr("Refused to write over %1.")
                           .arg(p.name.isEmpty() ? tr("a protected region") : p.name);
            return false;
        }
    }
    const qint64 total = data.size();
    for (qint64 done = 0; done < total;) {
        // A cancel is honored before each write starts, never in the middle
        // of one.
        if (!step(name, done, total)) {
            *problem = done ? tr("Cancelled part way through.") : tr("Cancelled.");
            return false;
        }
        const QByteArray chunk = data.mid(done, kWriteChunk);
        const quint32 at = offset + static_cast<quint32>(done);
        if (!m_target->matches(at, chunk)) {
            if (!m_target->write(at, chunk, {}, problem))
                return false;
        }
        done += chunk.size();
    }
    if (!step(name, total, total)) {
        *problem = tr("Cancelled.");
        return false;
    }
    return true;
}

bool EspFirmwareInstaller::writeOtadata(const EspFirmwareManifest& manifest, int slot,
                                        QString* problem)
{
    QByteArray otadata;
    if (!m_target->read(manifest.otadata, kOtadataSize, &otadata, {}, problem))
        return false;
    const QByteArray next = selectSlot(otadata, slot, manifest.appSlots.size());
    // Only the entry selectSlot replaced is written, one sector on its own:
    // the other entry, the one the device starts from now, stays intact
    // until the new one is in place, so an interrupted write never leaves
    // the device without a valid boot selection.
    for (quint32 at = 0; at < kOtadataSize; at += kSector) {
        const QByteArray sector = next.mid(at, kSector);
        if (sector == otadata.mid(at, kSector))
            continue;
        if (!writeRegion(manifest.otadata + at, sector, tr("Selecting the firmware to start"),
                         problem))
            return false;
    }
    return true;
}

bool EspFirmwareInstaller::install(const EspFirmwareManifest& manifest,
                                   const QHash<QString, QByteArray>& files, QString* problem)
{
    for (const EspFirmwareManifest::Image& image : manifest.images) {
        const QByteArray bytes = files.value(image.file);
        if (static_cast<quint32>(bytes.size()) != image.size
            || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != image.sha256) {
            *problem = tr("%1 is not the file the manifest names. Nothing was written.")
                           .arg(image.file);
            return false;
        }
    }
    if (!check(manifest, problem))
        return false;

    // A required region must read the same after the install: work out what
    // the images (and the erasure that comes with them) leave there.
    for (const EspFirmwareManifest::Region& r : manifest.require) {
        QByteArray after;
        if (!m_target->read(r.offset, r.size, &after, {}, problem))
            return false;
        const quint64 rEnd = quint64(r.offset) + r.size;
        for (const EspFirmwareManifest::Image& image : manifest.images) {
            const QByteArray bytes = files.value(image.file);
            const quint64 iEnd = quint64(image.offset) + image.size;
            const quint64 eEnd = sectorCeil(iEnd);
            for (quint64 a = qMax<quint64>(r.offset, image.offset); a < qMin(rEnd, eEnd); ++a) {
                after[static_cast<qsizetype>(a - r.offset)] =
                    a < iEnd ? bytes.at(static_cast<qsizetype>(a - image.offset)) : '\xFF';
            }
        }
        if (QCryptographicHash::hash(after, QCryptographicHash::Sha256) != r.sha256) {
            *problem = tr("This firmware would change the device's %1, which it must keep. "
                          "Nothing was written.")
                           .arg(r.name.isEmpty() ? tr("layout") : r.name);
            return false;
        }
    }

    for (const EspFirmwareManifest::Image& image : manifest.images) {
        if (!writeRegion(image.offset, files.value(image.file), tr("Writing %1").arg(image.file),
                         problem))
            return false;
    }
    return writeOtadata(manifest, manifest.ownSlots.first(), problem);
}

bool EspFirmwareInstaller::start(const EspFirmwareManifest& manifest, QString* problem)
{
    if (!check(manifest, problem))
        return false;
    QByteArray otadata;
    if (!m_target->read(manifest.otadata, kOtadataSize, &otadata, {}, problem))
        return false;
    int slot = newestOf(otadata, manifest.ownSlots, manifest.appSlots.size());
    if (slot < 0)
        slot = manifest.ownSlots.first();
    QByteArray head;
    if (!m_target->read(manifest.appSlots[slot], 4, &head, {}, problem))
        return false;
    if (static_cast<quint8>(head[0]) != kAppImageMagic) {
        *problem = tr("%1 is not installed on this device.").arg(manifest.product);
        return false;
    }
    return writeOtadata(manifest, slot, problem);
}

bool EspFirmwareInstaller::restore(const QByteArray& flash, QString* problem)
{
    m_protect.clear();   // a backup puts back the whole flash as it was
    if (static_cast<quint64>(flash.size()) != m_target->flashSize()) {
        *problem = tr("This backup is %1 bytes; the device's flash is %2 bytes. Nothing was "
                      "written.")
                       .arg(flash.size())
                       .arg(m_target->flashSize());
        return false;
    }
    return writeRegion(0, flash, tr("Restoring the backup"), problem);
}

} // namespace AetherSDR
